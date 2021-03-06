/**
 * @file
 * Pixel art tracing settings dialog - implementation.
 */
/* Authors:
 *   Bob Jamison <rjamison@titan.com>
 *   Stéphane Gimenez <dev@gim.name>
 *   Vinícius dos Santos Oliveira <vini.ipsmaker@gmail.com>
 *   Other dudes from The Inkscape Organization
 *
 * Copyright (C) 2004-2013 Authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "pixelartdialog.h"
#include <gtkmm/radiobutton.h>
#include <gtkmm/stock.h>
#include <gtkmm/messagedialog.h>

#include <gtk/gtk.h> //for GTK_RESPONSE* types
#include <glibmm/i18n.h>

#include "ui/widget/spinbutton.h"
#include "ui/widget/frame.h"

#include "desktop.h"
#include "desktop-tracker.h"
#include "message-stack.h"
#include "selection.h"
#include "preferences.h"

#include "sp-image.h"
#include "display/cairo-utils.h"
#include "libdepixelize/kopftracer2011.h"
#include <algorithm>
#include "document.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "svg/svg.h"
#include "svg/svg-color.h"
#include "color.h"
#include "svg/css-ostringstream.h"
#include "document-undo.h"

#ifdef HAVE_OPENMP
#include <omp.h>
#endif // HAVE_OPENMP

namespace Inkscape {
namespace UI {
namespace Dialog {

template<class T>
T move(T &obj)
{
#ifdef LIBDEPIXELIZE_ENABLE_CPP11
    return std::move(obj);
#else
    T ret;
    std::swap(obj, ret);
    return ret;
#endif // LIBDEPIXELIZE_ENABLE_CPP11
}

/**
 * A dialog for adjusting pixel art -> vector tracing parameters
 */
class PixelArtDialogImpl : public PixelArtDialog
{
public:
    PixelArtDialogImpl();

    ~PixelArtDialogImpl();

private:
    void setDesktop(SPDesktop *desktop);
    void setTargetDesktop(SPDesktop *desktop);

    //############ Events

    void responseCallback(int response_id);

    //############ UI Logic

    Tracer::Kopf2011::Options options();

    void vectorize();
    void processLibdepixelize(SPImage *img);
    void setDefaults();
    void updatePreview();

    bool ignorePreview;
    bool pendingPreview;

    //############ UI

    Gtk::HBox             buttonsHBox;

    Gtk::Button           *mainOkButton;
    Gtk::Button           *mainCancelButton;
    Gtk::Button           *mainResetButton;

    Gtk::VBox         heuristicsVBox;
    UI::Widget::Frame heuristicsFrame;

    Gtk::HBox                        curvesMultiplierHBox;
    Gtk::Label                       curvesMultiplierLabel;
    Inkscape::UI::Widget::SpinButton curvesMultiplierSpinner;

    Gtk::HBox                        islandsWeightHBox;
    Gtk::Label                       islandsWeightLabel;
    Inkscape::UI::Widget::SpinButton islandsWeightSpinner;

    Gtk::HBox                        sparsePixelsMultiplierHBox;
    Gtk::Label                       sparsePixelsMultiplierLabel;
    Inkscape::UI::Widget::SpinButton sparsePixelsMultiplierSpinner;

    Gtk::HBox                        sparsePixelsRadiusHBox;
    Gtk::Label                       sparsePixelsRadiusLabel;
    Inkscape::UI::Widget::SpinButton sparsePixelsRadiusSpinner;

    Gtk::VBox             outputVBox;
    UI::Widget::Frame     outputFrame;
    Gtk::RadioButtonGroup outputGroup;

    Gtk::RadioButton voronoiRadioButton;
    Gtk::RadioButton noOptimizeRadioButton;
#if LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
    Gtk::RadioButton optimizeRadioButton;
#endif // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH

    SPDesktop *desktop;
    DesktopTracker deskTrack;
    sigc::connection desktopChangeConn;
};

void PixelArtDialogImpl::setDesktop(SPDesktop *desktop)
{
    Panel::setDesktop(desktop);
    deskTrack.setBase(desktop);
}

void PixelArtDialogImpl::setTargetDesktop(SPDesktop *desktop)
{
    this->desktop = desktop;
}

PixelArtDialogImpl::PixelArtDialogImpl() :
    PixelArtDialog(),
    ignorePreview(false),
    pendingPreview(false)
{

    Gtk::Box *contents = _getContents();

    // Heuristics
    {
        curvesMultiplierLabel.set_label(_("_Curves (multiplier)"));
        curvesMultiplierLabel.set_use_underline(true);
        curvesMultiplierLabel.set_mnemonic_widget(curvesMultiplierSpinner);
        curvesMultiplierLabel.set_tooltip_text(_("Favors connections that are part of a long curve"));
        curvesMultiplierSpinner.set_increments(0.125, 0);
        curvesMultiplierSpinner.set_digits(3);
        curvesMultiplierSpinner.set_range(-10, 10);
        curvesMultiplierSpinner.get_adjustment()->signal_value_changed()
            .connect(sigc::mem_fun(*this, &PixelArtDialogImpl::updatePreview));

        curvesMultiplierHBox.pack_start(curvesMultiplierLabel, false, false);
        curvesMultiplierHBox.pack_end(curvesMultiplierSpinner, false, false);
        heuristicsVBox.pack_start(curvesMultiplierHBox, false, false);

        islandsWeightLabel.set_label(_("_Islands (weight)"));
        islandsWeightLabel.set_use_underline(true);
        islandsWeightLabel.set_mnemonic_widget(islandsWeightSpinner);
        islandsWeightLabel.set_tooltip_text(_("Avoid single disconnected pixels"));

        islandsWeightSpinner.set_tooltip_text(_("A constant vote value"));
        islandsWeightSpinner.set_increments(1, 0);
        islandsWeightSpinner.set_range(-20, 20);
        islandsWeightSpinner.get_adjustment()->signal_value_changed()
            .connect(sigc::mem_fun(*this, &PixelArtDialogImpl::updatePreview));

        islandsWeightHBox.pack_start(islandsWeightLabel, false, false);
        islandsWeightHBox.pack_end(islandsWeightSpinner, false, false);
        heuristicsVBox.pack_start(islandsWeightHBox, false, false);

        sparsePixelsRadiusLabel.set_label(_("Sparse pixels (window _radius)"));
        sparsePixelsRadiusLabel.set_use_underline(true);
        sparsePixelsRadiusLabel.set_mnemonic_widget(sparsePixelsRadiusSpinner);

        sparsePixelsRadiusSpinner.set_increments(1, 0);
        sparsePixelsRadiusSpinner.set_range(2, 8);
        sparsePixelsRadiusSpinner.get_adjustment()->signal_value_changed()
            .connect(sigc::mem_fun(*this, &PixelArtDialogImpl::updatePreview));

        sparsePixelsRadiusSpinner.set_tooltip_text(_("The radius of the window analyzed"));
        sparsePixelsMultiplierLabel.set_label(_("Sparse pixels (_multiplier)"));
        sparsePixelsMultiplierLabel.set_use_underline(true);
        sparsePixelsMultiplierLabel.set_mnemonic_widget(sparsePixelsMultiplierSpinner);

        sparsePixelsMultiplierSpinner.set_increments(0.125, 0);
        sparsePixelsMultiplierSpinner.set_digits(3);
        sparsePixelsMultiplierSpinner.set_range(-10, 10);
        sparsePixelsMultiplierSpinner.get_adjustment()->signal_value_changed()
            .connect(sigc::mem_fun(*this, &PixelArtDialogImpl::updatePreview));

        {
            char const *str = _("Favors connections that are part of foreground color");
            sparsePixelsRadiusLabel.set_tooltip_text(str);
            sparsePixelsMultiplierLabel.set_tooltip_text(str);
        }

        {
            char const *str = _("The heuristic computed vote will be multiplied by this value");
            curvesMultiplierSpinner.set_tooltip_text(str);
            sparsePixelsMultiplierSpinner.set_tooltip_text(str);
        }

        sparsePixelsRadiusHBox.pack_start(sparsePixelsRadiusLabel, false, false);
        sparsePixelsRadiusHBox.pack_end(sparsePixelsRadiusSpinner, false, false);
        heuristicsVBox.pack_start(sparsePixelsRadiusHBox, false, false);

        sparsePixelsMultiplierHBox.pack_start(sparsePixelsMultiplierLabel, false, false);
        sparsePixelsMultiplierHBox.pack_end(sparsePixelsMultiplierSpinner, false, false);
        heuristicsVBox.pack_start(sparsePixelsMultiplierHBox, false, false);

        heuristicsFrame.set_label(_("Heuristics"));
        heuristicsFrame.add(heuristicsVBox);
        contents->pack_start(heuristicsFrame, false, false);
    }

    // Output
    {
        voronoiRadioButton.set_label(_("_Voronoi diagram"));
        voronoiRadioButton.set_tooltip_text(_("Output composed of straight lines"));
        voronoiRadioButton.set_use_underline(true);
        outputGroup = voronoiRadioButton.get_group();

        outputVBox.pack_start(voronoiRadioButton, false, false);

        noOptimizeRadioButton.set_label(_("Convert to _B-spline curves"));
        noOptimizeRadioButton.set_tooltip_text(_("Preserve staircasing artifacts"));
        noOptimizeRadioButton.set_use_underline(true);
        noOptimizeRadioButton.set_group(outputGroup);

        outputVBox.pack_start(noOptimizeRadioButton, false, false);

#if LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
        optimizeRadioButton.set_label(_("_Smooth curves"));
        optimizeRadioButton.set_tooltip_text(_("The Kopf-Lischinski algorithm"));
        optimizeRadioButton.set_use_underline(true);
        optimizeRadioButton.set_group(outputGroup);

        outputVBox.pack_start(optimizeRadioButton, false, false);
#endif // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH

        outputFrame.set_label(_("Output"));
        outputFrame.add(outputVBox);
        contents->pack_start(outputFrame, true, false);
    }

    // Buttons
    {
        mainResetButton = addResponseButton(_("Reset"), GTK_RESPONSE_HELP, true);
        mainResetButton ->set_tooltip_text(_("Reset all settings to defaults"));

        //## The OK button
        mainCancelButton = addResponseButton(Gtk::Stock::STOP, GTK_RESPONSE_CANCEL);
        if (mainCancelButton) {
            mainCancelButton->set_tooltip_text(_("Abort a trace in progress"));
            mainCancelButton->set_sensitive(false);
        }
        mainOkButton = addResponseButton(Gtk::Stock::OK, GTK_RESPONSE_OK);
        mainOkButton->set_tooltip_text(_("Execute the trace"));

        contents->pack_start(buttonsHBox);
    }

    setDefaults();

    show_all_children();

    desktopChangeConn = deskTrack.connectDesktopChanged( sigc::mem_fun(*this, &PixelArtDialogImpl::setTargetDesktop) );
    deskTrack.connect(GTK_WIDGET(gobj()));

    signalResponse().connect(sigc::mem_fun(*this, &PixelArtDialogImpl::responseCallback));
}

void PixelArtDialogImpl::responseCallback(int response_id)
{
    if (response_id == GTK_RESPONSE_OK) {
        vectorize();
    } else if (response_id == GTK_RESPONSE_CANCEL) {
        // TODO
    } else if (response_id == GTK_RESPONSE_HELP) {
        setDefaults();
    } else {
        hide();
        return;
    }
}

Tracer::Kopf2011::Options PixelArtDialogImpl::options()
{
    Tracer::Kopf2011::Options options;

    options.curvesMultiplier = curvesMultiplierSpinner.get_value();
    options.islandsWeight = islandsWeightSpinner.get_value_as_int();
    options.sparsePixelsMultiplier = sparsePixelsMultiplierSpinner.get_value();
    options.sparsePixelsRadius = sparsePixelsRadiusSpinner.get_value_as_int();
#if LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
    options.optimize = optimizeRadioButton.get_active();
#else // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
    options.optimize = false;
#endif // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH

    options.nthreads = Inkscape::Preferences::get()
        ->getIntLimited("/options/threading/numthreads",
#ifdef HAVE_OPENMP
                        omp_get_num_procs(),
#else
                        1,
#endif // HAVE_OPENMP
                        1, 256);

    return options;
}

void PixelArtDialogImpl::vectorize()
{
    Inkscape::MessageStack *msgStack = desktop->messageStack();

    if ( !desktop->selection ) {
        char *msg = _("Select an <b>image</b> to trace");
        msgStack->flash(Inkscape::ERROR_MESSAGE, msg);
        return;
    }

    bool found = false;

    for ( GSList const *list = desktop->selection->itemList() ; list
              ; list = list->next ) {
        if ( !SP_IS_IMAGE(list->data) )
            continue;

        found = true;

        processLibdepixelize(SP_IMAGE(list->data));
    }

    if ( !found ) {
        char *msg = _("Select an <b>image</b> to trace");
        msgStack->flash(Inkscape::ERROR_MESSAGE, msg);
        return;
    }

    DocumentUndo::done(desktop->doc(), SP_VERB_SELECTION_PIXEL_ART,
                       _("Trace pixel art"));

    // Flush pending updates
    desktop->doc()->ensureUpToDate();
}

void PixelArtDialogImpl::processLibdepixelize(SPImage *img)
{
    Tracer::Splines out;

    Glib::RefPtr<Gdk::Pixbuf> pixbuf
        = Glib::wrap(img->pixbuf->getPixbufRaw(), true);

    if ( pixbuf->get_width() > 256 || pixbuf->get_height() > 256 ) {
        char *msg = _("Image looks too big. Process may take a while and is"
                      " wise to save your document before continue."
                      "\n\nContinue the procedure (without saving)?");
        Gtk::MessageDialog dialog(msg, false, Gtk::MESSAGE_WARNING,
                                  Gtk::BUTTONS_OK_CANCEL, true);

        if ( dialog.run() != Gtk::RESPONSE_OK )
            return;
    }

    if ( voronoiRadioButton.get_active() ) {
        out = Tracer::Kopf2011::to_voronoi(pixbuf, options());
    } else {
        out = Tracer::Kopf2011::to_splines(pixbuf, options());
    }

    Inkscape::XML::Document *xml_doc = desktop->doc()->getReprDoc();
    Inkscape::XML::Node *group = xml_doc->createElement("svg:g");

    for ( Tracer::Splines::iterator it = out.begin(), end = out.end()
              ; it != end ; ++it ) {
        Inkscape::XML::Node *repr = xml_doc->createElement("svg:path");

        {
            SPCSSAttr *css = sp_repr_css_attr_new();

            {
                gchar b[64];
                sp_svg_write_color(b, sizeof(b),
                                   SP_RGBA32_U_COMPOSE(unsigned(it->rgba[0]),
                                                       unsigned(it->rgba[1]),
                                                       unsigned(it->rgba[2]),
                                                       unsigned(it->rgba[3])));

                sp_repr_css_set_property(css, "fill", b);
            }

            {
                Inkscape::CSSOStringStream osalpha;
                osalpha << float(it->rgba[3]) / 255.;
                sp_repr_css_set_property(css, "fill-opacity",
                                         osalpha.str().c_str());
            }

            sp_repr_css_set(repr, css, "style");
            sp_repr_css_attr_unref(css);
        }

        gchar *str = sp_svg_write_path(Dialog::move(it->pathVector));
        repr->setAttribute("d", str);
        g_free(str);

        group->appendChild(repr);

        Inkscape::GC::release(repr);
    }

    {
        group->setAttribute("transform",
                            (std::string("translate(")
                             + sp_svg_length_write_with_units(img->x)
                             + ' ' + sp_svg_length_write_with_units(img->y)
                             + ')').c_str());
    }

    desktop->currentLayer()->appendChildRepr(group);

    Inkscape::GC::release(group);
}

void PixelArtDialogImpl::setDefaults()
{
    ignorePreview = true;

    curvesMultiplierSpinner.set_value(Tracer::Kopf2011::Options
                                      ::CURVES_MULTIPLIER);

    islandsWeightSpinner.set_value(Tracer::Kopf2011::Options::ISLANDS_WEIGHT);

    sparsePixelsRadiusSpinner.set_value(Tracer::Kopf2011::Options
                                        ::SPARSE_PIXELS_RADIUS);

    sparsePixelsMultiplierSpinner.set_value(Tracer::Kopf2011::Options
                                            ::SPARSE_PIXELS_MULTIPLIER);

#if LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
    optimizeRadioButton.set_active();
#else // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH
    noOptimizeRadioButton.set_active();
#endif // LIBDEPIXELIZE_INKSCAPE_ENABLE_SMOOTH

    ignorePreview = false;

    if ( pendingPreview )
        updatePreview();
}

void PixelArtDialogImpl::updatePreview()
{
    if ( ignorePreview ) {
        pendingPreview = true;
        return;
    }

    // TODO: update preview
    pendingPreview = false;
}

/**
 * Factory method.  Use this to create a new PixelArtDialog
 */
PixelArtDialog &PixelArtDialog::getInstance()
{
    PixelArtDialog *dialog = new PixelArtDialogImpl();
    return *dialog;
}

PixelArtDialogImpl::~PixelArtDialogImpl()
{
    desktopChangeConn.disconnect();
}


} //namespace Dialog
} //namespace UI
} //namespace Inkscape

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
