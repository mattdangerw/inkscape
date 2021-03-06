/*
 * SnapManager class.
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   Nathan Hurst <njh@njhurst.com>
 *   Carl Hetherington <inkscape@carlh.net>
 *   Diederik van Lierop <mail@diedenrezi.nl>
 *
 * Copyright (C) 2006-2007 Johan Engelen <johan@shouraizou.nl>
 * Copyrigth (C) 2004      Nathan Hurst
 * Copyright (C) 1999-2012 Authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include <utility>
#include <2geom/transforms.h>

#include "sp-namedview.h"
#include "snap.h"
#include "snap-enums.h"
#include "snapped-line.h"
#include "snapped-curve.h"

#include "display/canvas-grid.h"
#include "display/snap-indicator.h"

#include "inkscape.h"
#include "desktop.h"
#include "selection.h"
#include "sp-guide.h"
#include "preferences.h"
#include "ui/tools/tool-base.h"
#include "util/mathfns.h"
using std::vector;
using Inkscape::Util::round_to_upper_multiple_plus;
using Inkscape::Util::round_to_lower_multiple_plus;

SnapManager::SnapManager(SPNamedView const *v) :
    guide(this, 0),
    object(this, 0),
    snapprefs(),
    _named_view(v),
    _rotation_center_source_items(NULL),
    _guide_to_ignore(NULL),
    _desktop(NULL),
    _snapindicator(true),
    _unselected_nodes(NULL)
{
}

SnapManager::SnapperList SnapManager::getSnappers() const
{
    SnapManager::SnapperList s;
    s.push_back(&guide);
    s.push_back(&object);

    SnapManager::SnapperList gs = getGridSnappers();
    s.splice(s.begin(), gs);

    return s;
}

SnapManager::SnapperList SnapManager::getGridSnappers() const
{
    SnapperList s;

    if (_desktop && _desktop->gridsEnabled() && snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_GRID)) {
        for ( GSList const *l = _named_view->grids; l != NULL; l = l->next) {
            Inkscape::CanvasGrid *grid = (Inkscape::CanvasGrid*) l->data;
            s.push_back(grid->snapper);
        }
    }

    return s;
}

bool SnapManager::someSnapperMightSnap(bool immediately) const
{
    if ( !snapprefs.getSnapEnabledGlobally() ) {
        return false;
    }

    // If we're asking if some snapper might snap RIGHT NOW (without the snap being postponed)...
    if ( immediately && snapprefs.getSnapPostponedGlobally() ) {
        return false;
    }

    SnapperList const s = getSnappers();
    SnapperList::const_iterator i = s.begin();
    while (i != s.end() && (*i)->ThisSnapperMightSnap() == false) {
        ++i;
    }

    return (i != s.end());
}

bool SnapManager::gridSnapperMightSnap() const
{
    if ( !snapprefs.getSnapEnabledGlobally() || snapprefs.getSnapPostponedGlobally() ) {
        return false;
    }

    SnapperList const s = getGridSnappers();
    SnapperList::const_iterator i = s.begin();
    while (i != s.end() && (*i)->ThisSnapperMightSnap() == false) {
        ++i;
    }

    return (i != s.end());
}

void SnapManager::freeSnapReturnByRef(Geom::Point &p,
                                      Inkscape::SnapSourceType const source_type,
                                      Geom::OptRect const &bbox_to_snap) const
{
    Inkscape::SnappedPoint const s = freeSnap(Inkscape::SnapCandidatePoint(p, source_type), bbox_to_snap);
    s.getPointIfSnapped(p);
}

Inkscape::SnappedPoint SnapManager::freeSnap(Inkscape::SnapCandidatePoint const &p,
                                             Geom::OptRect const &bbox_to_snap) const
{
    if (!someSnapperMightSnap()) {
        return Inkscape::SnappedPoint(p, Inkscape::SNAPTARGET_UNDEFINED, Geom::infinity(), 0, false, false, false);
    }

    IntermSnapResults isr;
    SnapperList const snappers = getSnappers();

    for (SnapperList::const_iterator i = snappers.begin(); i != snappers.end(); ++i) {
        (*i)->freeSnap(isr, p, bbox_to_snap, &_items_to_ignore, _unselected_nodes);
    }

    return findBestSnap(p, isr, false);
}

void SnapManager::preSnap(Inkscape::SnapCandidatePoint const &p)
{
    // setup() must have been called before calling this method!

    if (_snapindicator) {
        _snapindicator = false; // prevent other methods from drawing a snap indicator; we want to control this here
        Inkscape::SnappedPoint s = freeSnap(p);
        g_assert(_desktop != NULL);
        if (s.getSnapped()) {
            _desktop->snapindicator->set_new_snaptarget(s, true);
        } else {
            _desktop->snapindicator->remove_snaptarget(true);
        }
        _snapindicator = true; // restore the original value
    }
}

Geom::Point SnapManager::multipleOfGridPitch(Geom::Point const &t, Geom::Point const &origin)
{
    if (!snapprefs.getSnapEnabledGlobally() || snapprefs.getSnapPostponedGlobally())
        return t;

    if (_desktop && _desktop->gridsEnabled()) {
        bool success = false;
        Geom::Point nearest_multiple;
        Geom::Coord nearest_distance = Geom::infinity();
        Inkscape::SnappedPoint bestSnappedPoint(t);

        // It will snap to the grid for which we find the closest snap. This might be a different
        // grid than to which the objects were initially aligned. I don't see an easy way to fix
        // this, so when using multiple grids one can get unexpected results

        // Cannot use getGridSnappers() because we need both the grids AND their snappers
        // Therefore we iterate through all grids manually
        for (GSList const *l = _named_view->grids; l != NULL; l = l->next) {
            Inkscape::CanvasGrid *grid = (Inkscape::CanvasGrid*) l->data;
            const Inkscape::Snapper* snapper = grid->snapper;
            if (snapper && snapper->ThisSnapperMightSnap()) {
                // To find the nearest multiple of the grid pitch for a given translation t, we
                // will use the grid snapper. Simply snapping the value t to the grid will do, but
                // only if the origin of the grid is at (0,0). If it's not then compensate for this
                // in the translation t
                Geom::Point const t_offset = t + grid->origin;
                IntermSnapResults isr;
                // Only the first three parameters are being used for grid snappers
                snapper->freeSnap(isr, Inkscape::SnapCandidatePoint(t_offset, Inkscape::SNAPSOURCE_GRID_PITCH),Geom::OptRect(), NULL, NULL);
                // Find the best snap for this grid, including intersections of the grid-lines
                bool old_val = _snapindicator;
                _snapindicator = false;
                Inkscape::SnappedPoint s = findBestSnap(Inkscape::SnapCandidatePoint(t_offset, Inkscape::SNAPSOURCE_GRID_PITCH), isr, false, true);
                _snapindicator = old_val;
                if (s.getSnapped() && (s.getSnapDistance() < nearest_distance)) {
                    // use getSnapDistance() instead of getWeightedDistance() here because the pointer's position
                    // doesn't tell us anything about which node to snap
                    success = true;
                    nearest_multiple = s.getPoint() - grid->origin;
                    nearest_distance = s.getSnapDistance();
                    bestSnappedPoint = s;
                }
            }
        }

        if (success) {
            bestSnappedPoint.setPoint(origin + nearest_multiple);
            _desktop->snapindicator->set_new_snaptarget(bestSnappedPoint);
            return nearest_multiple;
        }
    }

    return t;
}

void SnapManager::constrainedSnapReturnByRef(Geom::Point &p,
                                             Inkscape::SnapSourceType const source_type,
                                             Inkscape::Snapper::SnapConstraint const &constraint,
                                             Geom::OptRect const &bbox_to_snap) const
{
    Inkscape::SnappedPoint const s = constrainedSnap(Inkscape::SnapCandidatePoint(p, source_type), constraint, bbox_to_snap);
    p = s.getPoint(); // If we didn't snap, then we will return the point projected onto the constraint
}

Inkscape::SnappedPoint SnapManager::constrainedSnap(Inkscape::SnapCandidatePoint const &p,
                                                    Inkscape::Snapper::SnapConstraint const &constraint,
                                                    Geom::OptRect const &bbox_to_snap) const
{
    // First project the mouse pointer onto the constraint
    Geom::Point pp = constraint.projection(p.getPoint());

    Inkscape::SnappedPoint no_snap = Inkscape::SnappedPoint(pp, p.getSourceType(), p.getSourceNum(), Inkscape::SNAPTARGET_CONSTRAINT, Geom::infinity(), 0, false, true, false);

    if (!someSnapperMightSnap()) {
        // Always return point on constraint
        return no_snap;
    }

    Inkscape::SnappedPoint result = no_snap;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if ((prefs->getBool("/options/snapmousepointer/value", false)) && p.isSingleHandle()) {
        // Snapping the mouse pointer instead of the constrained position of the knot allows
        // to snap to things which don't intersect with the constraint line; this is basically
        // then just a freesnap with the constraint applied afterwards
        // We'll only to this if we're dragging a single handle, and for example not when transforming an object in the selector tool
        result = freeSnap(p, bbox_to_snap);
        if (result.getSnapped()) {
            // only change the snap indicator if we really snapped to something
            if (_snapindicator && _desktop) {
                _desktop->snapindicator->set_new_snaptarget(result);
            }
            // Apply the constraint
            result.setPoint(constraint.projection(result.getPoint()));
            return result;
        }
        return no_snap;
    }

    IntermSnapResults isr;
    SnapperList const snappers = getSnappers();
    for (SnapperList::const_iterator i = snappers.begin(); i != snappers.end(); ++i) {
        (*i)->constrainedSnap(isr, p, bbox_to_snap, constraint, &_items_to_ignore, _unselected_nodes);
    }

    result = findBestSnap(p, isr, true);


    if (result.getSnapped()) {
        // only change the snap indicator if we really snapped to something
        if (_snapindicator && _desktop) {
            _desktop->snapindicator->set_new_snaptarget(result);
        }
        return result;
    }
    return no_snap;
}

/* See the documentation for constrainedSnap() directly above for more details.
 * The difference is that multipleConstrainedSnaps() will take a list of constraints instead of a single one,
 * and will try to snap the SnapCandidatePoint to all of the provided constraints and see which one fits best
 *  \param p Source point to be snapped
 *  \param constraints List of directions or lines along which snapping must occur
 *  \param dont_snap If true then we will only apply the constraint, without snapping
 *  \param bbox_to_snap Bounding box hulling the set of points, all from the same selection and having the same transformation
 */


Inkscape::SnappedPoint SnapManager::multipleConstrainedSnaps(Inkscape::SnapCandidatePoint const &p,
                                                    std::vector<Inkscape::Snapper::SnapConstraint> const &constraints,
                                                    bool dont_snap,
                                                    Geom::OptRect const &bbox_to_snap) const
{

    Inkscape::SnappedPoint no_snap = Inkscape::SnappedPoint(p.getPoint(), p.getSourceType(), p.getSourceNum(), Inkscape::SNAPTARGET_CONSTRAINT, Geom::infinity(), 0, false, true, false);
    if (constraints.size() == 0) {
        return no_snap;
    }

    IntermSnapResults isr;
    SnapperList const snappers = getSnappers();
    std::vector<Geom::Point> projections;
    bool snapping_is_futile = !someSnapperMightSnap() || dont_snap;

    Inkscape::SnappedPoint result = no_snap;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool snap_mouse = prefs->getBool("/options/snapmousepointer/value", false);

    for (std::vector<Inkscape::Snapper::SnapConstraint>::const_iterator c = constraints.begin(); c != constraints.end(); ++c) {
        // Project the mouse pointer onto the constraint; In case we don't snap then we will
        // return the projection onto the constraint, such that the constraint is always enforced
        Geom::Point pp = (*c).projection(p.getPoint());
        projections.push_back(pp);
    }

    if (snap_mouse && p.isSingleHandle() && !dont_snap) {
        // Snapping the mouse pointer instead of the constrained position of the knot allows
        // to snap to things which don't intersect with the constraint line; this is basically
        // then just a freesnap with the constraint applied afterwards
        // We'll only to this if we're dragging a single handle, and for example not when transforming an object in the selector tool
        result = freeSnap(p, bbox_to_snap);
    } else {
        // Iterate over the constraints
        for (std::vector<Inkscape::Snapper::SnapConstraint>::const_iterator c = constraints.begin(); c != constraints.end(); ++c) {
            // Try to snap to the constraint
            if (!snapping_is_futile) {
                for (SnapperList::const_iterator i = snappers.begin(); i != snappers.end(); ++i) {
                    (*i)->constrainedSnap(isr, p, bbox_to_snap, *c, &_items_to_ignore,_unselected_nodes);
                }
            }
        }
        result = findBestSnap(p, isr, true);
    }

    if (result.getSnapped()) {
        if (snap_mouse) {
            // If "snap_mouse" then we still have to apply the constraint, because so far we only tried a freeSnap
            Geom::Point result_closest;
            for (std::vector<Inkscape::Snapper::SnapConstraint>::const_iterator c = constraints.begin(); c != constraints.end(); ++c) {
                // Project the mouse pointer onto the constraint; In case we don't snap then we will
                // return the projection onto the constraint, such that the constraint is always enforced
                Geom::Point result_p = (*c).projection(result.getPoint());
                if (c == constraints.begin() || (Geom::L2(result_p - p.getPoint()) < Geom::L2(result_closest - p.getPoint()))) {
                    result_closest = result_p;
                }
            }
            result.setPoint(result_closest);
        }
        return result;
    }

    // So we didn't snap, but we still need to return a point on one of the constraints
    // Find out which of the constraints yielded the closest projection of point p
    for (std::vector<Geom::Point>::iterator pp = projections.begin(); pp != projections.end(); ++pp) {
        if (pp != projections.begin()) {
            if (Geom::L2(*pp - p.getPoint()) < Geom::L2(no_snap.getPoint() - p.getPoint())) {
                no_snap.setPoint(*pp);
            }
        } else {
            no_snap.setPoint(projections.front());
        }
    }

    return no_snap;
}

Inkscape::SnappedPoint SnapManager::constrainedAngularSnap(Inkscape::SnapCandidatePoint const &p,
                                                            boost::optional<Geom::Point> const &p_ref,
                                                            Geom::Point const &o,
                                                            unsigned const snaps) const
{
    Inkscape::SnappedPoint sp;
    if (snaps > 0) { // 0 means no angular snapping
        // p is at an arbitrary angle. Now we should snap this angle to specific increments.
        // For this we'll calculate the closest two angles, one at each side of the current angle
        Geom::Line y_axis(Geom::Point(0, 0), Geom::Point(0, 1));
        Geom::Line p_line(o, p.getPoint());
        double angle = Geom::angle_between(y_axis, p_line);
        double angle_incr = M_PI / snaps;
        double angle_offset = 0;
        if (p_ref) {
            Geom::Line p_line_ref(o, *p_ref);
            angle_offset = Geom::angle_between(y_axis, p_line_ref);
        }
        double angle_ceil = round_to_upper_multiple_plus(angle, angle_incr, angle_offset);
        double angle_floor = round_to_lower_multiple_plus(angle, angle_incr, angle_offset);
        // We have two angles now. The constrained snapper will try each of them and return the closest

        // Now do the snapping...
        std::vector<Inkscape::Snapper::SnapConstraint> constraints;
        constraints.push_back(Inkscape::Snapper::SnapConstraint(Geom::Line(o, angle_ceil - M_PI/2)));
        constraints.push_back(Inkscape::Snapper::SnapConstraint(Geom::Line(o, angle_floor - M_PI/2)));
        sp = multipleConstrainedSnaps(p, constraints); // Constraints will always be applied, even if we didn't snap
        if (!sp.getSnapped()) { // If we haven't snapped then we only had the constraint applied;
            sp.setTarget(Inkscape::SNAPTARGET_CONSTRAINED_ANGLE);
        }
    } else {
        sp = freeSnap(p);
    }
    return sp;
}

void SnapManager::guideFreeSnap(Geom::Point &p, Geom::Point &origin_or_vector, bool origin, bool freeze_angle) const
{
    if (freeze_angle && origin) {
        g_warning("Dear developer, when snapping guides you shouldn't ask me to freeze the guide's vector when you haven't specified one");
        // You've supplied me with an origin instead of a vector
    }

    if (!snapprefs.getSnapEnabledGlobally() || snapprefs.getSnapPostponedGlobally() || !snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_GUIDE)) {
        return;
    }

    Inkscape::SnapCandidatePoint candidate(p, Inkscape::SNAPSOURCE_GUIDE);
    if (origin) {
        candidate.addOrigin(origin_or_vector);
    } else {
        candidate = Inkscape::SnapCandidatePoint(p, Inkscape::SNAPSOURCE_GUIDE_ORIGIN);
        candidate.addVector(Geom::rot90(origin_or_vector));
    }

    IntermSnapResults isr;
    SnapperList snappers = getSnappers();
    for (SnapperList::const_iterator i = snappers.begin(); i != snappers.end(); ++i) {
        (*i)->freeSnap(isr, candidate, Geom::OptRect(), NULL, NULL);
    }

    Inkscape::SnappedPoint const s = findBestSnap(candidate, isr, false);

    s.getPointIfSnapped(p);

    if (!freeze_angle && s.getSnapped()) {
        if (!Geom::are_near(s.getTangent(), Geom::Point(0,0))) { // If the tangent has been set ...
            origin_or_vector = Geom::rot90(s.getTangent()); // then use it to update the normal of the guide
            // PS: The tangent might not have been set if we snapped for example to a node
        }
    }
}

void SnapManager::guideConstrainedSnap(Geom::Point &p, SPGuide const &guideline) const
{
    if (!snapprefs.getSnapEnabledGlobally() || snapprefs.getSnapPostponedGlobally() || !snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_GUIDE)) {
        return;
    }

    Inkscape::SnapCandidatePoint candidate(p, Inkscape::SNAPSOURCE_GUIDE_ORIGIN, Inkscape::SNAPTARGET_UNDEFINED);

    IntermSnapResults isr;
    Inkscape::Snapper::SnapConstraint cl(guideline.point_on_line, Geom::rot90(guideline.normal_to_line));

    SnapperList snappers = getSnappers();
    for (SnapperList::const_iterator i = snappers.begin(); i != snappers.end(); ++i) {
        (*i)->constrainedSnap(isr, candidate, Geom::OptRect(), cl, NULL, NULL);
    }

    Inkscape::SnappedPoint const s = findBestSnap(candidate, isr, false);
    s.getPointIfSnapped(p);
}

Inkscape::SnappedPoint SnapManager::_snapTransformed(
    std::vector<Inkscape::SnapCandidatePoint> const &points,
    Geom::Point const &pointer,
    bool constrained,
    Inkscape::Snapper::SnapConstraint const &constraint,
    Transformation transformation_type,
    Geom::Point const &transformation,
    Geom::Point const &origin,
    Geom::Dim2 dim,
    bool uniform)
{
    /* We have a list of points, which we are proposing to transform in some way.  We need to see
    ** if any of these points, when transformed, snap to anything.  If they do, we return the
    ** appropriate transformation with `true'; otherwise we return the original scale with `false'.
    */

    if (points.size() == 0) {
        return Inkscape::SnappedPoint(pointer);
    }

    std::vector<Inkscape::SnapCandidatePoint> transformed_points;
    Geom::Rect bbox;

    long source_num = 0;
    for (std::vector<Inkscape::SnapCandidatePoint>::const_iterator i = points.begin(); i != points.end(); ++i) {

        /* Work out the transformed version of this point */
        Geom::Point transformed = _transformPoint(*i, transformation_type, transformation, origin, dim, uniform);

        // add the current transformed point to the box hulling all transformed points
        if (i == points.begin()) {
            bbox = Geom::Rect(transformed, transformed);
        } else {
            bbox.expandTo(transformed);
        }

        transformed_points.push_back(Inkscape::SnapCandidatePoint(transformed, (*i).getSourceType(), source_num, Inkscape::SNAPTARGET_UNDEFINED, Geom::OptRect()));
        source_num++;
    }

    /* The current best transformation */
    Geom::Point best_transformation = transformation;

    /* The current best metric for the best transformation; lower is better, Geom::infinity()
    ** means that we haven't snapped anything.
    */
    Inkscape::SnappedPoint best_snapped_point;
    g_assert(best_snapped_point.getAlwaysSnap() == false); // Check initialization of snapped point
    g_assert(best_snapped_point.getAtIntersection() == false);

    // Warnings for the devs
    if (constrained && transformation_type == SCALE && !uniform) {
        g_warning("Non-uniform constrained scaling is not supported!");
    }

    if (!constrained && transformation_type == ROTATE) {
        // We do not yet allow for simultaneous rotation and scaling
        g_warning("Unconstrained rotation is not supported!");
    }

    // We will try to snap a set of points, but we don't want to have a snap indicator displayed
    // for each of them. That's why it's temporarily disabled here, and re-enabled again after we
    // have finished calling the freeSnap() and constrainedSnap() methods
    bool _orig_snapindicator_status = _snapindicator;
    _snapindicator = false;

    std::vector<Inkscape::SnapCandidatePoint>::iterator j = transformed_points.begin();

    // std::cout << std::endl;
    bool first_free_snap = true;

    for (std::vector<Inkscape::SnapCandidatePoint>::const_iterator i = points.begin(); i != points.end(); ++i) {

        /* Snap it */
        Inkscape::SnappedPoint snapped_point;
        Inkscape::Snapper::SnapConstraint dedicated_constraint = constraint;
        Geom::Point const b = ((*i).getPoint() - origin); // vector to original point (not the transformed point! required for rotations!)

        if (constrained) {
            if (((transformation_type == SCALE || transformation_type == STRETCH) && uniform)) {
                // When uniformly scaling, each point will have its own unique constraint line,
                // running from the scaling origin to the original untransformed point. We will
                // calculate that line here
                dedicated_constraint = Inkscape::Snapper::SnapConstraint(origin, b);
            } else if (transformation_type == ROTATE) {
                Geom::Coord r = Geom::L2(b); // the radius of the circular constraint
                dedicated_constraint = Inkscape::Snapper::SnapConstraint(origin, b, r);
            } else if (transformation_type == STRETCH) { // when non-uniform stretching {
                Geom::Point cvec; cvec[dim] = 1.;
                dedicated_constraint = Inkscape::Snapper::SnapConstraint((*i).getPoint(), cvec);
            } else if (transformation_type == TRANSLATE) {
                // When doing a constrained translation, all points will move in the same direction, i.e.
                // either horizontally or vertically. The lines along which they move are therefore all
                // parallel, but might not be colinear. Therefore we will have to specify the point through
                // which the constraint-line runs here, for each point individually. (we could also have done this
                // earlier on, e.g. in seltrans.cpp but we're being lazy there and don't want to add an iteration loop)
                dedicated_constraint = Inkscape::Snapper::SnapConstraint((*i).getPoint(), constraint.getDirection());
            } // else: leave the original constraint, e.g. for skewing
            snapped_point = constrainedSnap(*j, dedicated_constraint, bbox);
        } else {
            bool const c1 = fabs(b[Geom::X]) < 1e-6;
            bool const c2 = fabs(b[Geom::Y]) < 1e-6;
            if (transformation_type == SCALE && (c1 || c2) && !(c1 && c2)) {
                // When scaling, a point aligned either horizontally or vertically with the origin can only
                // move in that specific direction; therefore it should only snap in that direction, otherwise
                // we will get snapped points with an invalid transformation
                Geom::Point cvec; cvec[c1] = 1.;
                dedicated_constraint = Inkscape::Snapper::SnapConstraint(origin, cvec);
                snapped_point = constrainedSnap(*j, dedicated_constraint, bbox);
            } else {
                // If we have a collection of SnapCandidatePoints, with mixed constrained snapping and free snapping
                // requirements, then freeSnap might never see the SnapCandidatePoint with source_num == 0. The freeSnap()
                // method in the object snapper depends on this, because only for source-num == 0 the target nodes will
                // be collected. Therefore we enforce that the first SnapCandidatePoint that is to be freeSnapped always
                // has source_num == 0;
                // TODO: This is a bit ugly so fix this; do we need sourcenum for anything else? if we don't then get rid
                // of it and explicitely communicate to the object snapper that this is a first point
                if (first_free_snap) {
                    (*j).setSourceNum(0);
                    first_free_snap = false;
                }
                snapped_point = freeSnap(*j, bbox);
            }
        }
        // std::cout << "dist = " << snapped_point.getSnapDistance() << std::endl;
        snapped_point.setPointerDistance(Geom::L2(pointer - (*i).getPoint()));

        // Allow the snapindicator to be displayed again
        _snapindicator = _orig_snapindicator_status;

        Geom::Point result;

        /*Find the transformation that describes where the snapped point has
        ** ended up, and also the metric for this transformation.
        */
        Geom::Point const a = snapped_point.getPoint() - origin; // vector to snapped point
        //Geom::Point const b = (*i - origin); // vector to original point

        switch (transformation_type) {
            case TRANSLATE:
                result = snapped_point.getPoint() - (*i).getPoint();
                /* Consider the case in which a box is almost aligned with a grid in both
                 * horizontal and vertical directions. The distance to the intersection of
                 * the grid lines will always be larger then the distance to a single grid
                 * line. If we prefer snapping to an intersection over to a single
                 * grid line, then we cannot use "metric = Geom::L2(result)". Therefore the
                 * snapped distance will be used as a metric. Please note that the snapped
                 * distance to an intersection is defined as the distance to the nearest line
                 *  of the intersection, and not to the intersection itself!
                 */
                // Only for translations, the relevant metric will be the real snapped distance,
                // so we don't have to do anything special here
                break;
            case SCALE:
            {
                result = Geom::Point(Geom::infinity(), Geom::infinity());
                // If this point *i is horizontally or vertically aligned with
                // the origin of the scaling, then it will scale purely in X or Y
                // We can therefore only calculate the scaling in this direction
                // and the scaling factor for the other direction should remain
                // untouched (unless scaling is uniform of course)
                for (int index = 0; index < 2; index++) {
                    if (fabs(b[index]) > 1e-6) { // if SCALING CAN occur in this direction
                        if (fabs(fabs(a[index]/b[index]) - fabs(transformation[index])) > 1e-12) { // if SNAPPING DID occur in this direction
                            result[index] = a[index] / b[index]; // then calculate it!
                        }
                        // we might have left result[1-index] = Geom::infinity()
                        // if scaling didn't occur in the other direction
                    }
                }
                if (uniform) {
                    if (fabs(result[0]) < fabs(result[1])) {
                        result[1] = result[0];
                    } else {
                        result[0] = result[1];
                    }
                }

                // Compare the resulting scaling with the desired scaling
                Geom::Point scale_metric = result - transformation; // One or both of its components might be Geom::infinity()
                scale_metric[0] = fabs(scale_metric[0]);
                scale_metric[1] = fabs(scale_metric[1]);
                if (scale_metric[0] == Geom::infinity() || scale_metric[1] == Geom::infinity()) {
                    snapped_point.setSnapDistance(std::min(scale_metric[0], scale_metric[1]));
                } else {
                    snapped_point.setSnapDistance(Geom::L2(scale_metric));
                }
                snapped_point.setSecondSnapDistance(Geom::infinity());
                break;
            }
            case STRETCH:
                result = Geom::Point(Geom::infinity(), Geom::infinity());
                if (fabs(b[dim]) > 1e-6) { // if STRETCHING will occur for this point
                    result[dim] = a[dim] / b[dim];
                    result[1-dim] = uniform ? result[dim] : 1;
                } else { // STRETCHING might occur for this point, but only when the stretching is uniform
                    if (uniform && fabs(b[1-dim]) > 1e-6) {
                       result[1-dim] = a[1-dim] / b[1-dim];
                       result[dim] = result[1-dim];
                    }
                }
                // Store the metric for this transformation as a virtual distance
                snapped_point.setSnapDistance(std::abs(result[dim] - transformation[dim]));
                snapped_point.setSecondSnapDistance(Geom::infinity());
                break;
            case SKEW:
                result[0] = (snapped_point.getPoint()[dim] - ((*i).getPoint())[dim]) / b[1 - dim]; // skew factor
                result[1] = transformation[1]; // scale factor
                // Store the metric for this transformation as a virtual distance
                snapped_point.setSnapDistance(std::abs(result[0] - transformation[0]));
                snapped_point.setSecondSnapDistance(Geom::infinity());
                break;
            case ROTATE:
                // a is vector to snapped point; b is vector to original point; now lets calculate angle between a and b
                result[0] = atan2(Geom::dot(Geom::rot90(b), a), Geom::dot(b, a));
                result[1] = result[1]; // how else should we store an angle in a point ;-)
                if (Geom::L2(b) < 1e-9) { // points too close to the rotation center will not move. Don't try to snap these
                    // as they will always yield a perfect snap result if they're already snapped beforehand (e.g.
                    // when the transformation center has been snapped to a grid intersection in the selector tool)
                    snapped_point.setSnapDistance(Geom::infinity());
                    // PS1: Apparently we don't have to do this for skewing, but why?
                    // PS2: We cannot easily filter these points upstream, e.g. in the grab() method (seltrans.cpp)
                    // because the rotation center will change when pressing shift, and grab() won't be recalled.
                    // Filtering could be done in handleRequest() (again in seltrans.cpp), by iterating through
                    // the snap candidates. But hey, we're iterating here anyway.
                } else {
                    snapped_point.setSnapDistance(std::abs(result[0] - transformation[0]));
                }
                snapped_point.setSecondSnapDistance(Geom::infinity());
                break;
            default:
                g_assert_not_reached();
        }

        if (snapped_point.getSnapped()) {
            // We snapped; keep track of the best snap
            if (best_snapped_point.isOtherSnapBetter(snapped_point, true)) {
                best_transformation = result;
                best_snapped_point = snapped_point;
            }
        } else {
            // So we didn't snap for this point
            if (!best_snapped_point.getSnapped()) {
                // ... and none of the points before snapped either
                // We might still need to apply a constraint though, if we tried a constrained snap. And
                // in case of a free snap we might have use for the transformed point, so let's return that
                // point, whether it's constrained or not
                if (best_snapped_point.isOtherSnapBetter(snapped_point, true) || points.size() == 1) {
                    // .. so we must keep track of the best non-snapped constrained point
                    best_transformation = result;
                    best_snapped_point = snapped_point;
                }
            }
        }

        ++j;
    }

    Geom::Coord best_metric;
    if (transformation_type == SCALE) {
        // When scaling, don't ever exit with one of scaling components uninitialized
        for (int index = 0; index < 2; index++) {
            if (fabs(best_transformation[index]) == Geom::infinity()) {
                if (uniform && fabs(best_transformation[1-index]) < Geom::infinity()) {
                    best_transformation[index] = best_transformation[1-index];
                } else {
                    best_transformation[index] = transformation[index];
                }
            }
        }
    }

    best_metric = best_snapped_point.getSnapDistance();
    best_snapped_point.setTransformation(best_transformation);
    // Using " < 1e6" instead of " < Geom::infinity()" for catching some rounding errors
    // These rounding errors might be caused by NRRects, see bug #1584301
    best_snapped_point.setSnapDistance(best_metric < 1e6 ? best_metric : Geom::infinity());

    if (_snapindicator) {
        if (best_snapped_point.getSnapped()) {
            _desktop->snapindicator->set_new_snaptarget(best_snapped_point);
        } else {
            _desktop->snapindicator->remove_snaptarget();
        }
    }
    return best_snapped_point;
}


Inkscape::SnappedPoint SnapManager::freeSnapTranslate(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                        Geom::Point const &pointer,
                                                        Geom::Point const &tr)
{
    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, false, Geom::Point(0,0), TRANSLATE, tr, Geom::Point(0,0), Geom::X, false);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}

Inkscape::SnappedPoint SnapManager::constrainedSnapTranslate(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                               Geom::Point const &pointer,
                                                               Inkscape::Snapper::SnapConstraint const &constraint,
                                                               Geom::Point const &tr)
{
    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, true, constraint, TRANSLATE, tr, Geom::Point(0,0), Geom::X, false);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}


Inkscape::SnappedPoint SnapManager::freeSnapScale(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                  Geom::Point const &pointer,
                                                  Geom::Scale const &s,
                                                  Geom::Point const &o)
{
    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, false, Geom::Point(0,0), SCALE, Geom::Point(s[Geom::X], s[Geom::Y]), o, Geom::X, false);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}


Inkscape::SnappedPoint SnapManager::constrainedSnapScale(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                         Geom::Point const &pointer,
                                                         Geom::Scale const &s,
                                                         Geom::Point const &o)
{
    // When constrained scaling, only uniform scaling is supported.
    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, true, Geom::Point(0,0), SCALE, Geom::Point(s[Geom::X], s[Geom::Y]), o, Geom::X, true);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}

Inkscape::SnappedPoint SnapManager::constrainedSnapStretch(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                            Geom::Point const &pointer,
                                                            Geom::Coord const &s,
                                                            Geom::Point const &o,
                                                            Geom::Dim2 d,
                                                            bool u)
{
    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, true, Geom::Point(0,0), STRETCH, Geom::Point(s, s), o, d, u);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}

Inkscape::SnappedPoint SnapManager::constrainedSnapSkew(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                 Geom::Point const &pointer,
                                                 Inkscape::Snapper::SnapConstraint const &constraint,
                                                 Geom::Point const &s,
                                                 Geom::Point const &o,
                                                 Geom::Dim2 d)
{
    // "s" contains skew factor in s[0], and scale factor in s[1]

    // Snapping the nodes of the bounding box of a selection that is being transformed, will only work if
    // the transformation of the bounding box is equal to the transformation of the individual nodes. This is
    // NOT the case for example when rotating or skewing. The bounding box itself cannot possibly rotate or skew,
    // so it's corners have a different transformation. The snappers cannot handle this, therefore snapping
    // of bounding boxes is not allowed here.
    if (!p.empty()) {
        g_assert(!(p.at(0).getSourceType() & Inkscape::SNAPSOURCE_BBOX_CATEGORY));
    }

    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, true, constraint, SKEW, s, o, d, false);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;
}

Inkscape::SnappedPoint SnapManager::constrainedSnapRotate(std::vector<Inkscape::SnapCandidatePoint> const &p,
                                                    Geom::Point const &pointer,
                                                    Geom::Coord const &angle,
                                                    Geom::Point const &o)
{
    // Snapping the nodes of the bounding box of a selection that is being transformed, will only work if
    // the transformation of the bounding box is equal to the transformation of the individual nodes. This is
    // NOT the case for example when rotating or skewing. The bounding box itself cannot possibly rotate or skew,
    // so it's corners have a different transformation. The snappers cannot handle this, therefore snapping
    // of bounding boxes is not allowed here.

    Inkscape::SnappedPoint result = _snapTransformed(p, pointer, true, Geom::Point(0,0), ROTATE, Geom::Point(angle, angle), o, Geom::X, false);

    if (p.size() == 1) {
        displaySnapsource(Inkscape::SnapCandidatePoint(result.getPoint(), p.at(0).getSourceType()));
    }

    return result;

}

Inkscape::SnappedPoint SnapManager::findBestSnap(Inkscape::SnapCandidatePoint const &p,
                                                 IntermSnapResults const &isr,
                                                 bool constrained,
                                                 bool allowOffScreen) const
{
    g_assert(_desktop != NULL);

    /*
    std::cout << "Type and number of snapped constraints: " << std::endl;
    std::cout << "  Points      : " << isr.points.size() << std::endl;
    std::cout << "  Grid lines  : " << isr.grid_lines.size()<< std::endl;
    std::cout << "  Guide lines : " << isr.guide_lines.size()<< std::endl;
    std::cout << "  Curves      : " << isr.curves.size()<< std::endl;
    */

    /*
    // Display all snap candidates on the canvas
    _desktop->snapindicator->remove_debugging_points();
    for (std::list<Inkscape::SnappedPoint>::const_iterator i = isr.points.begin(); i != isr.points.end(); i++) {
        _desktop->snapindicator->set_new_debugging_point((*i).getPoint());
    }
    for (std::list<Inkscape::SnappedCurve>::const_iterator i = isr.curves.begin(); i != isr.curves.end(); i++) {
        _desktop->snapindicator->set_new_debugging_point((*i).getPoint());
    }
    for (std::list<Inkscape::SnappedLine>::const_iterator i = isr.grid_lines.begin(); i != isr.grid_lines.end(); i++) {
        _desktop->snapindicator->set_new_debugging_point((*i).getPoint());
    }
    for (std::list<Inkscape::SnappedLine>::const_iterator i = isr.guide_lines.begin(); i != isr.guide_lines.end(); i++) {
        _desktop->snapindicator->set_new_debugging_point((*i).getPoint());
    }
    */

    // Store all snappoints
    std::list<Inkscape::SnappedPoint> sp_list;

    // search for the closest snapped point
    Inkscape::SnappedPoint closestPoint;
    if (getClosestSP(isr.points, closestPoint)) {
        sp_list.push_back(closestPoint);
    }

    // search for the closest snapped curve
    Inkscape::SnappedCurve closestCurve;
    // We might have collected the paths only to snap to their intersection, without the intention to snap to the paths themselves
    // Therefore we explicitly check whether the paths should be considered as snap targets themselves
    bool exclude_paths = !snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_PATH);
    if (getClosestCurve(isr.curves, closestCurve, exclude_paths)) {
        sp_list.push_back(Inkscape::SnappedPoint(closestCurve));
    }

    // search for the closest snapped grid line
    Inkscape::SnappedLine closestGridLine;
    if (getClosestSL(isr.grid_lines, closestGridLine)) {
        sp_list.push_back(Inkscape::SnappedPoint(closestGridLine));
    }

    // search for the closest snapped guide line
    Inkscape::SnappedLine closestGuideLine;
    if (getClosestSL(isr.guide_lines, closestGuideLine)) {
        sp_list.push_back(Inkscape::SnappedPoint(closestGuideLine));
    }

    // When freely snapping to a grid/guide/path, only one degree of freedom is eliminated
    // Therefore we will try get fully constrained by finding an intersection with another grid/guide/path

    // When doing a constrained snap however, we're already at an intersection of the constrained line and
    // the grid/guide/path we're snapping to. This snappoint is therefore fully constrained, so there's
    // no need to look for additional intersections
    if (!constrained) {
        if (snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_PATH_INTERSECTION)) {
            // search for the closest snapped intersection of curves
            Inkscape::SnappedPoint closestCurvesIntersection;
            if (getClosestIntersectionCS(isr.curves, p.getPoint(), closestCurvesIntersection, _desktop->dt2doc())) {
                closestCurvesIntersection.setSource(p.getSourceType());
                sp_list.push_back(closestCurvesIntersection);
            }
        }

        if (snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_PATH_GUIDE_INTERSECTION)) {
            // search for the closest snapped intersection of a guide with a curve
            Inkscape::SnappedPoint closestCurveGuideIntersection;
            if (getClosestIntersectionCL(isr.curves, isr.guide_lines, p.getPoint(), closestCurveGuideIntersection, _desktop->dt2doc())) {
                closestCurveGuideIntersection.setSource(p.getSourceType());
                sp_list.push_back(closestCurveGuideIntersection);
            }
        }

        // search for the closest snapped intersection of grid lines
        Inkscape::SnappedPoint closestGridPoint;
        if (getClosestIntersectionSL(isr.grid_lines, closestGridPoint)) {
            closestGridPoint.setSource(p.getSourceType());
            closestGridPoint.setTarget(Inkscape::SNAPTARGET_GRID_INTERSECTION);
            sp_list.push_back(closestGridPoint);
        }

        // search for the closest snapped intersection of guide lines
        Inkscape::SnappedPoint closestGuidePoint;
        if (getClosestIntersectionSL(isr.guide_lines, closestGuidePoint)) {
            closestGuidePoint.setSource(p.getSourceType());
            closestGuidePoint.setTarget(Inkscape::SNAPTARGET_GUIDE_INTERSECTION);
            sp_list.push_back(closestGuidePoint);
        }

        // search for the closest snapped intersection of grid with guide lines
        if (snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_GRID_GUIDE_INTERSECTION)) {
            Inkscape::SnappedPoint closestGridGuidePoint;
            if (getClosestIntersectionSL(isr.grid_lines, isr.guide_lines, closestGridGuidePoint)) {
                closestGridGuidePoint.setSource(p.getSourceType());
                closestGridGuidePoint.setTarget(Inkscape::SNAPTARGET_GRID_GUIDE_INTERSECTION);
                sp_list.push_back(closestGridGuidePoint);
            }
        }
    }

    // now let's see which snapped point gets a thumbs up
    Inkscape::SnappedPoint bestSnappedPoint(p.getPoint());
    // std::cout << "Finding the best snap..." << std::endl;
    for (std::list<Inkscape::SnappedPoint>::const_iterator i = sp_list.begin(); i != sp_list.end(); ++i) {
        // std::cout << "sp = " << (*i).getPoint() << " | source = " << (*i).getSource() << " | target = " << (*i).getTarget();
        bool onScreen = _desktop->get_display_area().contains((*i).getPoint());
        if (onScreen || allowOffScreen) { // Only snap to points which are not off the screen
            if ((*i).getSnapDistance() <= (*i).getTolerance()) { // Only snap to points within snapping range
                // if it's the first point, or if it is closer than the best snapped point so far
                if (i == sp_list.begin() || bestSnappedPoint.isOtherSnapBetter(*i, false)) {
                    // then prefer this point over the previous one
                    bestSnappedPoint = *i;
                }
            }
        }
        // std::cout << std::endl;
    }

    // Update the snap indicator, if requested
    if (_snapindicator) {
        if (bestSnappedPoint.getSnapped()) {
            _desktop->snapindicator->set_new_snaptarget(bestSnappedPoint);
        } else {
            _desktop->snapindicator->remove_snaptarget();
        }
    }

    // std::cout << "findBestSnap = " << bestSnappedPoint.getPoint() << " | dist = " << bestSnappedPoint.getSnapDistance() << std::endl;
    return bestSnappedPoint;
}

void SnapManager::setup(SPDesktop const *desktop,
                        bool snapindicator,
                        SPItem const *item_to_ignore,
                        std::vector<Inkscape::SnapCandidatePoint> *unselected_nodes,
                        SPGuide *guide_to_ignore)
{
    g_assert(desktop != NULL);
    if (_desktop != NULL) {
        g_warning("The snapmanager has been set up before, but unSetup() hasn't been called afterwards. It possibly held invalid pointers");
    }
    _items_to_ignore.clear();
    _items_to_ignore.push_back(item_to_ignore);
    _desktop = desktop;
    _snapindicator = snapindicator;
    _unselected_nodes = unselected_nodes;
    _guide_to_ignore = guide_to_ignore;
    _rotation_center_source_items = NULL;
}

void SnapManager::setup(SPDesktop const *desktop,
                        bool snapindicator,
                        std::vector<SPItem const *> &items_to_ignore,
                        std::vector<Inkscape::SnapCandidatePoint> *unselected_nodes,
                        SPGuide *guide_to_ignore)
{
    g_assert(desktop != NULL);
    if (_desktop != NULL) {
        g_warning("The snapmanager has been set up before, but unSetup() hasn't been called afterwards. It possibly held invalid pointers");
    }
    _items_to_ignore = items_to_ignore;
    _desktop = desktop;
    _snapindicator = snapindicator;
    _unselected_nodes = unselected_nodes;
    _guide_to_ignore = guide_to_ignore;
    _rotation_center_source_items = NULL;
}

/// Setup, taking the list of items to ignore from the desktop's selection.
void SnapManager::setupIgnoreSelection(SPDesktop const *desktop,
                                      bool snapindicator,
                                      std::vector<Inkscape::SnapCandidatePoint> *unselected_nodes,
                                      SPGuide *guide_to_ignore)
{
    g_assert(desktop != NULL);
    if (_desktop != NULL) {
        // Someone has been naughty here! This is dangerous
        g_warning("The snapmanager has been set up before, but unSetup() hasn't been called afterwards. It possibly held invalid pointers");
    }
    _desktop = desktop;
    _snapindicator = snapindicator;
    _unselected_nodes = unselected_nodes;
    _guide_to_ignore = guide_to_ignore;
    _rotation_center_source_items = NULL;
    _items_to_ignore.clear();

    Inkscape::Selection *sel = _desktop->selection;
    GSList const *items = sel->itemList();
    for (GSList *i = const_cast<GSList*>(items); i; i = i->next) {
        _items_to_ignore.push_back(static_cast<SPItem const *>(i->data));
    }
}

SPDocument *SnapManager::getDocument() const
{
    return _named_view->document;
}

Geom::Point SnapManager::_transformPoint(Inkscape::SnapCandidatePoint const &p,
                                        Transformation const transformation_type,
                                        Geom::Point const &transformation,
                                        Geom::Point const &origin,
                                        Geom::Dim2 const dim,
                                        bool const uniform) const
{
    /* Work out the transformed version of this point */
    Geom::Point transformed;
    switch (transformation_type) {
        case TRANSLATE:
            transformed = p.getPoint() + transformation;
            break;
        case SCALE:
            transformed = (p.getPoint() - origin) * Geom::Scale(transformation[Geom::X], transformation[Geom::Y]) + origin;
            break;
        case STRETCH:
        {
            Geom::Scale s(1, 1);
            if (uniform)
                s[Geom::X] = s[Geom::Y] = transformation[dim];
            else {
                s[dim] = transformation[dim];
                s[1 - dim] = 1;
            }
            transformed = ((p.getPoint() - origin) * s) + origin;
            break;
        }
        case SKEW:
            // Apply the skew factor
            transformed[dim] = (p.getPoint())[dim] + transformation[0] * ((p.getPoint())[1 - dim] - origin[1 - dim]);
            // While skewing, mirroring and scaling (by integer multiples) in the opposite direction is also allowed.
            // Apply that scale factor here
            transformed[1-dim] = (p.getPoint() - origin)[1 - dim] * transformation[1] + origin[1 - dim];
            break;
        case ROTATE:
            // for rotations: transformation[0] stores the angle in radians
            transformed = (p.getPoint() - origin) * Geom::Rotate(transformation[0]) + origin;
            break;
        default:
            g_assert_not_reached();
    }

    return transformed;
}

/**
 * Mark the location of the snap source (not the snap target!) on the canvas by drawing a symbol.
 *
 * @param point_type Category of points to which the source point belongs: node, guide or bounding box
 * @param p The transformed position of the source point, paired with an identifier of the type of the snap source.
 */
void SnapManager::displaySnapsource(Inkscape::SnapCandidatePoint const &p) const {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    if (prefs->getBool("/options/snapclosestonly/value")) {
        Inkscape::SnapSourceType t = p.getSourceType();
        bool p_is_a_node = t & Inkscape::SNAPSOURCE_NODE_CATEGORY;
        bool p_is_a_bbox = t & Inkscape::SNAPSOURCE_BBOX_CATEGORY;
        bool p_is_other = (t & Inkscape::SNAPSOURCE_OTHERS_CATEGORY) || (t & Inkscape::SNAPSOURCE_DATUMS_CATEGORY);

        g_assert(_desktop != NULL);
        if (snapprefs.getSnapEnabledGlobally() && (p_is_other || (p_is_a_node && snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_NODE_CATEGORY)) || (p_is_a_bbox && snapprefs.isTargetSnappable(Inkscape::SNAPTARGET_BBOX_CATEGORY)))) {
            _desktop->snapindicator->set_new_snapsource(p);
        } else {
            _desktop->snapindicator->remove_snapsource();
        }
    }
}
/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
