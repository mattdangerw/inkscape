#ifndef SEEN_SP_RECT_H
#define SEEN_SP_RECT_H

/*
 * SVG <rect> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Abhishek Sharma
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include "svg/svg-length.h"
#include "sp-shape.h"
#include <2geom/forward.h>

G_BEGIN_DECLS

#define SP_TYPE_RECT            (sp_rect_get_type ())
#define SP_RECT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), SP_TYPE_RECT, SPRect))
#define SP_RECT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), SP_TYPE_RECT, SPRectClass))
#define SP_IS_RECT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SP_TYPE_RECT))
#define SP_IS_RECT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), SP_TYPE_RECT))

class CRect;

class SPRect : public SPShape {
public:
	CRect* crect;

	SVGLength x;
	SVGLength y;
	SVGLength width;
	SVGLength height;
	SVGLength rx;
	SVGLength ry;
};

struct SPRectClass {
	SPShapeClass parent_class;
};


class CRect : public CShape {
public:
	CRect(SPRect* sprect);
	virtual ~CRect();

	virtual void build(SPDocument* doc, Inkscape::XML::Node* repr);

	void set(unsigned key, gchar const *value);
	void update(SPCtx* ctx, unsigned int flags);

	virtual Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags);
	virtual gchar* description();

	void set_shape();
	virtual Geom::Affine set_transform(Geom::Affine const& xform);

	void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs);
	void convert_to_guides();

protected:
	SPRect* sprect;
};


/* Standard GType function */
GType sp_rect_get_type (void) G_GNUC_CONST;

void sp_rect_position_set (SPRect * rect, gdouble x, gdouble y, gdouble width, gdouble height);

/* If SET if FALSE, VALUE is just ignored */
void sp_rect_set_rx(SPRect * rect, gboolean set, gdouble value);
void sp_rect_set_ry(SPRect * rect, gboolean set, gdouble value);

void sp_rect_set_visible_rx (SPRect *rect, gdouble rx);
void sp_rect_set_visible_ry (SPRect *rect, gdouble ry);
gdouble sp_rect_get_visible_rx (SPRect *rect);
gdouble sp_rect_get_visible_ry (SPRect *rect);
Geom::Rect sp_rect_get_rect (SPRect *rect);

void sp_rect_set_visible_width (SPRect *rect, gdouble rx);
void sp_rect_set_visible_height (SPRect *rect, gdouble ry);
gdouble sp_rect_get_visible_width (SPRect *rect);
gdouble sp_rect_get_visible_height (SPRect *rect);

void sp_rect_compensate_rxry (SPRect *rect, Geom::Affine xform);

G_END_DECLS

#endif // SEEN_SP_RECT_H

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
