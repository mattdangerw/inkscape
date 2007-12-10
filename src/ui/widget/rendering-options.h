/**
 * \brief Rendering Options Widget - A container for selecting rendering options
 *
 * Author:
 *   Kees Cook <kees@outflux.net>
 *
 * Copyright (C) 2007 Kees Cook
 * Copyright (C) 2004 Bryce Harrington
 *
 * Released under GNU GPL.  Read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_WIDGET_RENDERING_OPTIONS_H
#define INKSCAPE_UI_WIDGET_RENDERING_OPTIONS_H

#include "labelled.h"
#include "scalar.h"

namespace Inkscape {
namespace UI {
namespace Widget {

class RenderingOptions : public Gtk::VBox
{
public:
    RenderingOptions();

    bool as_bitmap();   // should we render as a bitmap?
    double bitmap_dpi();   // at what DPI should we render the bitmap?

protected:
    // Radio buttons to select desired rendering
    Gtk::RadioButton    *_radio_cairo;
    Gtk::RadioButton    *_radio_bitmap;
    Labelled    _widget_cairo;
    Labelled    _widget_bitmap;
    Scalar      _dpi; // DPI of bitmap to render
};

} // namespace Widget
} // namespace UI
} // namespace Inkscape

#endif // INKSCAPE_UI_WIDGET_RENDERING_OPTIONS_H

/* 
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
