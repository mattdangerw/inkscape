#ifndef __SP_USE_H__
#define __SP_USE_H__

/*
 * SVG <use> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include <stddef.h>
#include <sigc++/sigc++.h>
#include "svg/svg-length.h"
#include "sp-item.h"

#define SP_USE(obj) (dynamic_cast<SPUse*>((SPObject*)obj))
#define SP_IS_USE(obj) (dynamic_cast<const SPUse*>((SPObject*)obj) != NULL)

class SPUseReference;

class SPUse : public SPItem {
public:
	SPUse();
	virtual ~SPUse();

    // item built from the original's repr (the visible clone)
    // relative to the SPUse itself, it is treated as a child, similar to a grouped item relative to its group
    SPItem *child;

    // SVG attrs
    SVGLength x;
    SVGLength y;
    SVGLength width;
    SVGLength height;
    gchar *href;

    // the reference to the original object
    SPUseReference *ref;

    // a sigc connection for delete notifications
    sigc::connection _delete_connection;
    sigc::connection _changed_connection;

    // a sigc connection for transformed signal, used to do move compensation
    sigc::connection _transformed_connection;

	virtual void build(SPDocument* doc, Inkscape::XML::Node* repr);
	virtual void release();
	virtual void set(unsigned key, gchar const *value);
	virtual Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, guint flags);
	virtual void update(SPCtx* ctx, unsigned int flags);
	virtual void modified(unsigned int flags);

	virtual Geom::OptRect bbox(Geom::Affine const &transform, SPItem::BBoxType bboxtype) const;
    virtual const char* displayName() const;
	virtual gchar* description() const;
	virtual void print(SPPrintContext *ctx);
	virtual Inkscape::DrawingItem* show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags);
	virtual void hide(unsigned int key);
	virtual void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const;

	SPItem *root();
	SPItem const *root() const;

	SPItem *unlink();
	SPItem *get_original();
	Geom::Affine get_parent_transform();
	Geom::Affine get_root_transform();

private:
    void href_changed();
    void move_compensate(Geom::Affine const *mp);
    void delete_self();
};

#endif

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
