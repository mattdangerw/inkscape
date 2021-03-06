/*
 * SVG <desc> implementation
 *
 * Authors:
 *   Jeff Schiller <codedread@gmail.com>
 *
 * Copyright (C) 2008 Jeff Schiller
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "sp-desc.h"
#include "xml/repr.h"

#include "sp-factory.h"

namespace {
	SPObject* createDesc() {
		return new SPDesc();
	}

	bool descRegistered = SPFactory::instance().registerObject("svg:desc", createDesc);
}

SPDesc::SPDesc() : SPObject() {
}

SPDesc::~SPDesc() {
}

/**
 * Writes it's settings to an incoming repr object, if any.
 */
Inkscape::XML::Node* SPDesc::write(Inkscape::XML::Document* doc, Inkscape::XML::Node* repr, guint flags) {
    if (!repr) {
        repr = this->getRepr()->duplicate(doc);
    }

    SPObject::write(doc, repr, flags);

    return repr;
}
