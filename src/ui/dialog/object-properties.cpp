/**
 * @file Object properties dialog.
 */
/* 
 * Inkscape, an Open Source vector graphics editor
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright (C) 2012 Kris De Gussem <Kris.DeGussem@gmail.com>
 * c++ version based on former C-version (GPL v2) with authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Johan Engelen <goejendaagh@zonnet.nl>
 *   Abhishek Sharma
 */

#include "object-properties.h"
#include "widgets/sp-attribute-widget.h"
#include "../../desktop-handles.h"
#include "../../document.h"
#include "../../document-undo.h"
#include "verbs.h"
#include "inkscape.h"
#include "selection.h"
#include "desktop.h"
#include "sp-item.h"
#include "sp-image.h"
#include "xml/repr.h"
#include <glibmm/i18n.h>

#if WITH_GTKMM_3_0
# include <gtkmm/grid.h>
#else
# include <gtkmm/table.h>
#endif


namespace Inkscape {
namespace UI {
namespace Dialog {

ObjectProperties::ObjectProperties (void) :
    UI::Widget::Panel ("", "/dialogs/object/", SP_VERB_DIALOG_ITEM),
    blocked (false),
    CurrentItem(NULL),
#if WITH_GTKMM_3_0
    TopTable(Gtk::manage(new Gtk::Grid())),
#else
    TopTable(Gtk::manage(new Gtk::Table(4, 4))),
#endif
    LabelID(_("_ID:"), 1),
    LabelLabel(_("_Label:"), 1),
    LabelTitle(_("_Title:"),1),
    LabelImageRendering(_("_Image Rendering:"),1),
    LabelDescription(_("_Description:"),1),
    FrameDescription("", FALSE),
    HBoxCheck(FALSE, 0),
#if WITH_GTKMM_3_0
    CheckTable(Gtk::manage(new Gtk::Grid())),
#else
    CheckTable(Gtk::manage(new Gtk::Table(1, 2, true))),
#endif
    CBHide(_("_Hide"), 1),
    CBLock(_("L_ock"), 1),
    BSet (_("_Set"), 1),
    LabelInteractivity(_("_Interactivity"), 1),
    attrTable(Gtk::manage(new SPAttributeTable())),
    desktop(NULL),
    deskTrack(),
    selectChangedConn(),
    subselChangedConn()
{
    //initialize labels for the table at the bottom of the dialog
    int_attrs.push_back("onclick");
    int_attrs.push_back("onmouseover");
    int_attrs.push_back("onmouseout");
    int_attrs.push_back("onmousedown");
    int_attrs.push_back("onmouseup");
    int_attrs.push_back("onmousemove");
    int_attrs.push_back("onfocusin");
    int_attrs.push_back("onfocusout");
    int_attrs.push_back("onload");
	
    int_labels.push_back("onclick:");
    int_labels.push_back("onmouseover:");
    int_labels.push_back("onmouseout:");
    int_labels.push_back("onmousedown:");
    int_labels.push_back("onmouseup:");
    int_labels.push_back("onmousemove:");
    int_labels.push_back("onfocusin:");
    int_labels.push_back("onfocusout:");
    int_labels.push_back("onload:");
    
    desktopChangeConn = deskTrack.connectDesktopChanged( sigc::mem_fun(*this, &ObjectProperties::setTargetDesktop) );
    deskTrack.connect(GTK_WIDGET(gobj()));

#if WITH_GTKMM_3_0
    CheckTable->set_row_homogeneous();
    CheckTable->set_column_homogeneous(true);
#endif
    
    MakeWidget();
}

ObjectProperties::~ObjectProperties (void)
{
    subselChangedConn.disconnect();
    selectChangedConn.disconnect();
    desktopChangeConn.disconnect();
    deskTrack.disconnect();
}

void ObjectProperties::MakeWidget(void)
{
    Gtk::Box *contents = _getContents();
    contents->set_spacing(0);
    
    TopTable->set_border_width(4);

#if WITH_GTKMM_3_0
    TopTable->set_row_spacing(4);
    TopTable->set_column_spacing(0);
#else
    TopTable->set_row_spacings(4);
    TopTable->set_col_spacings(0);
#endif

    contents->pack_start (*TopTable, false, false, 0);

    /* Create the label for the object id */
    LabelID.set_label (LabelID.get_label() + " ");
    LabelID.set_alignment (1, 0.5);

#if WITH_GTKMM_3_0
    LabelID.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(LabelID, 0, 0, 1, 1);
#else
    TopTable->attach(LabelID, 0, 1, 0, 1,
                      Gtk::SHRINK | Gtk::FILL,
                      Gtk::AttachOptions(), 0, 0 );
#endif

    /* Create the entry box for the object id */
    EntryID.set_tooltip_text (_("The id= attribute (only letters, digits, and the characters .-_: allowed)"));
    EntryID.set_max_length (64);

#if WITH_GTKMM_3_0
    EntryID.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(EntryID, 1, 0, 1, 1);
#else
    TopTable->attach(EntryID, 1, 2, 0, 1,
                     Gtk::EXPAND | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    LabelID.set_mnemonic_widget (EntryID);

    // pressing enter in the id field is the same as clicking Set:
    EntryID.signal_activate().connect(sigc::mem_fun(this, &ObjectProperties::label_changed));
    // focus is in the id field initially:
    EntryID.grab_focus();

    /* Create the label for the object label */
    LabelLabel.set_label (LabelLabel.get_label() + " ");
    LabelLabel.set_alignment (1, 0.5);

#if WITH_GTKMM_3_0
    LabelLabel.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(LabelLabel, 0, 1, 1, 1);
#else
    TopTable->attach(LabelLabel, 0, 1, 1, 2,
                     Gtk::SHRINK | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    /* Create the entry box for the object label */
    EntryLabel.set_tooltip_text (_("A freeform label for the object"));
    EntryLabel.set_max_length (256);

#if WITH_GTKMM_3_0
    EntryLabel.set_hexpand();
    EntryLabel.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(EntryLabel, 1, 1, 1, 1);
#else
    TopTable->attach(EntryLabel, 1, 2, 1, 2,
                     Gtk::EXPAND | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    LabelLabel.set_mnemonic_widget (EntryLabel);

    // pressing enter in the label field is the same as clicking Set:
    EntryLabel.signal_activate().connect(sigc::mem_fun(this, &ObjectProperties::label_changed));

    /* Create the label for the object title */
    LabelTitle.set_label (LabelTitle.get_label() + " ");
    LabelTitle.set_alignment (1, 0.5);

#if WITH_GTKMM_3_0
    LabelTitle.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(LabelTitle, 0, 2, 1, 1);
#else
    TopTable->attach(LabelTitle, 0, 1, 2, 3,
                     Gtk::SHRINK | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    /* Create the entry box for the object title */
    EntryTitle.set_sensitive (FALSE);
    EntryTitle.set_max_length (256);

#if WITH_GTKMM_3_0
    EntryTitle.set_hexpand();
    EntryTitle.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(EntryTitle, 1, 2, 1, 1);
#else
    TopTable->attach(EntryTitle, 1, 2, 2, 3,
                     Gtk::EXPAND | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    LabelTitle.set_mnemonic_widget (EntryTitle);
    // pressing enter in the label field is the same as clicking Set:
    EntryTitle.signal_activate().connect(sigc::mem_fun(this, &ObjectProperties::label_changed));

    /* Create the frame for the object description */
    FrameDescription.set_label_widget (LabelDescription);
    FrameDescription.set_padding (0,0,0,0);
    contents->pack_start (FrameDescription, true, true, 0);

    /* Create the text view box for the object description */
    FrameTextDescription.set_border_width(4);
    FrameTextDescription.set_sensitive (FALSE);
    FrameDescription.add (FrameTextDescription);
    FrameTextDescription.set_shadow_type (Gtk::SHADOW_IN);

    TextViewDescription.set_wrap_mode(Gtk::WRAP_WORD);
    TextViewDescription.get_buffer()->set_text("");
    FrameTextDescription.add (TextViewDescription);
    TextViewDescription.add_mnemonic_label(LabelDescription);

    /* Image rendering */
    /* Create the label for the object ImageRendering */
    LabelImageRendering.set_label (LabelImageRendering.get_label() + " ");
    LabelImageRendering.set_alignment (1, 0.5);

#if WITH_GTKMM_3_0
    LabelImageRendering.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(LabelImageRendering, 0, 3, 1, 1);
#else
    TopTable->attach(LabelImageRendering, 0, 1, 3, 4,
                      Gtk::SHRINK | Gtk::FILL,
                      Gtk::AttachOptions(), 0, 0 );
#endif

    /* Create the combo box text for the 'image-rendering' property  */
    ComboBoxTextImageRendering.append( "auto" );
    ComboBoxTextImageRendering.append( "optimizeQuality" );
    ComboBoxTextImageRendering.append( "optimizeSpeed" );
    ComboBoxTextImageRendering.set_tooltip_text (_("The 'image-rendering' property can influence how a bitmap is up-scaled:\n\t'auto' no preference;\n\t'optimizeQuality' smooth;\n\t'optimizeSpeed' blocky.\nNote that this behaviour is not defined in the SVG 1.1 specification and not all browsers follow this interpretation."));

#if WITH_GTKMM_3_0
    ComboBoxTextImageRendering.set_valign(Gtk::ALIGN_CENTER);
    TopTable->attach(ComboBoxTextImageRendering, 1, 3, 1, 1);
#else
    TopTable->attach(ComboBoxTextImageRendering, 1, 2, 3, 4,
                     Gtk::EXPAND | Gtk::FILL,
                     Gtk::AttachOptions(), 0, 0 );
#endif

    LabelImageRendering.set_mnemonic_widget (ComboBoxTextImageRendering);

    ComboBoxTextImageRendering.signal_changed().connect(sigc::mem_fun(this, &ObjectProperties::image_rendering_changed));

    /* Check boxes */
    contents->pack_start (HBoxCheck, FALSE, FALSE, 0);
    CheckTable->set_border_width(4);
    HBoxCheck.pack_start(*CheckTable, true, true, 0);

    /* Hide */
    CBHide.set_tooltip_text (_("Check to make the object invisible"));

#if WITH_GTKMM_3_0
    CBHide.set_hexpand();
    CBHide.set_valign(Gtk::ALIGN_CENTER);
    CheckTable->attach(CBHide, 0, 0, 1, 1);
#else
    CheckTable->attach(CBHide, 0, 1, 0, 1,
                       Gtk::EXPAND | Gtk::FILL,
                       Gtk::AttachOptions(), 0, 0 );
#endif

    CBHide.signal_toggled().connect(sigc::mem_fun(this, &ObjectProperties::hidden_toggled));

    /* Lock */
    // TRANSLATORS: "Lock" is a verb here
    CBLock.set_tooltip_text (_("Check to make the object insensitive (not selectable by mouse)"));

#if WITH_GTKMM_3_0
    CBLock.set_hexpand();
    CBLock.set_valign(Gtk::ALIGN_CENTER);
    CheckTable->attach(CBLock, 1, 0, 1, 1);
#else
    CheckTable->attach(CBLock, 1, 2, 0, 1,
                       Gtk::EXPAND | Gtk::FILL,
                       Gtk::AttachOptions(), 0, 0 );
#endif

    CBLock.signal_toggled().connect(sigc::mem_fun(this, &ObjectProperties::sensitivity_toggled));


    /* Button for setting the object's id, label, title and description. */
#if WITH_GTKMM_3_0
    BSet.set_hexpand();
    BSet.set_valign(Gtk::ALIGN_CENTER);
    CheckTable->attach(BSet, 2, 0, 1, 1);
#else
    CheckTable->attach(BSet, 2, 3, 0, 1,
                       Gtk::EXPAND | Gtk::FILL,
                       Gtk::AttachOptions(), 0, 0 );
#endif

    BSet.signal_clicked().connect(sigc::mem_fun(this, &ObjectProperties::label_changed));

    /* Create the frame for interactivity options */
    EInteractivity.set_label_widget (LabelInteractivity);
    contents->pack_start (EInteractivity, FALSE, FALSE, 0);
    show_all ();
    widget_setup();
}

void ObjectProperties::widget_setup(void)
{
    if (blocked || !desktop)
    {
        return;
    }
    if (SP_ACTIVE_DESKTOP != desktop)
    {
        return;
    }

    Inkscape::Selection *selection = sp_desktop_selection (SP_ACTIVE_DESKTOP);
    Gtk::Box *contents = _getContents();

    if (!selection->singleItem()) {
        contents->set_sensitive (false);
        CurrentItem = NULL;
        //no selection anymore or multiple objects selected, means that we need
        //to close the connections to the previously selected object
        attrTable->clear();
        return;
    } else {
        contents->set_sensitive (true);
    }
    
    SPItem *item = selection->singleItem();
    if (CurrentItem == item)
    {
        //otherwise we would end up wasting resources through the modify selection
        //callback when moving an object (endlessly setting the labels and recreating attrTable)
        return;
    }
    blocked = true;
    
    CBLock.set_active (item->isLocked());           /* Sensitive */
    CBHide.set_active (item->isExplicitlyHidden()); /* Hidden */
    
    if (item->cloned) {
        /* ID */
        EntryID.set_text ("");
        EntryID.set_sensitive (FALSE);
        LabelID.set_text (_("Ref"));

        /* Label */
        EntryLabel.set_text ("");
        EntryLabel.set_sensitive (FALSE);
        LabelLabel.set_text (_("Ref"));

    } else {
        SPObject *obj = static_cast<SPObject*>(item);

        /* ID */
        EntryID.set_text (obj->getId());
        EntryID.set_sensitive (TRUE);
        LabelID.set_markup_with_mnemonic (_("_ID:"));

        /* Label */
        EntryLabel.set_text(obj->defaultLabel());
        EntryLabel.set_sensitive (TRUE);

        /* Title */
        gchar *title = obj->title();
        if (title) {
            EntryTitle.set_text(title);
            g_free(title);
        }
        else {
            EntryTitle.set_text("");
        }
        EntryTitle.set_sensitive(TRUE);

        /* Image Rendering */
        if( SP_IS_IMAGE( item ) ) {
            ComboBoxTextImageRendering.show();
            LabelImageRendering.show();
            char const *str = obj->getStyleProperty( "image-rendering", "auto" );
            if( strcmp( str, "auto" ) == 0 ) {
                ComboBoxTextImageRendering.set_active(0);
            } else if( strcmp( str, "optimizeQuality" ) == 0 ) {
                ComboBoxTextImageRendering.set_active(1);
            }  else {
                ComboBoxTextImageRendering.set_active(2);
            }
        } else {
            ComboBoxTextImageRendering.hide();
            ComboBoxTextImageRendering.unset_active();
            LabelImageRendering.hide();
        }

        /* Description */
        gchar *desc = obj->desc();
        if (desc) {
            TextViewDescription.get_buffer()->set_text(desc);
            g_free(desc);
        } else {
            TextViewDescription.get_buffer()->set_text("");
        }
        FrameTextDescription.set_sensitive(TRUE);
        
        if (CurrentItem == NULL)
        {
            attrTable->set_object(obj, int_labels, int_attrs, (GtkWidget*)EInteractivity.gobj());
        }
        else
        {
            attrTable->change_object(obj);
        }
        attrTable->show_all();
    }
    CurrentItem = item;
    blocked = false;
}

void ObjectProperties::label_changed(void)
{
    if (blocked)
    {
        return;
    }
    
    SPItem *item = sp_desktop_selection(SP_ACTIVE_DESKTOP)->singleItem();
    g_return_if_fail (item != NULL);

    blocked = true;

    /* Retrieve the label widget for the object's id */
    gchar *id = g_strdup(EntryID.get_text().c_str());
    g_strcanon (id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.:", '_');
    if (!strcmp (id, item->getId())) {
        LabelID.set_markup_with_mnemonic(_("_ID:"));
    } else if (!*id || !isalnum (*id)) {
        LabelID.set_text (_("Id invalid! "));
    } else if (SP_ACTIVE_DOCUMENT->getObjectById(id) != NULL) {
        LabelID.set_text (_("Id exists! "));
    } else {
        SPException ex;
        LabelID.set_markup_with_mnemonic(_("_ID:"));
        SP_EXCEPTION_INIT (&ex);
        item->setAttribute("id", id, &ex);
        DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM, _("Set object ID"));
    }
    g_free (id);

    /* Retrieve the label widget for the object's label */
    Glib::ustring label = EntryLabel.get_text();

    /* Give feedback on success of setting the drawing object's label
     * using the widget's label text
     */
    SPObject *obj = static_cast<SPObject*>(item);
    if (label.compare (obj->defaultLabel())) {
        obj->setLabel(label.c_str());
        DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM,
                _("Set object label"));
    }

    /* Retrieve the title */
    if (obj->setTitle(EntryTitle.get_text().c_str()))
        DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM,
                _("Set object title"));

    /* Retrieve the description */
    Gtk::TextBuffer::iterator start, end;
    TextViewDescription.get_buffer()->get_bounds(start, end);
    Glib::ustring desc = TextViewDescription.get_buffer()->get_text(start, end, TRUE);
    if (obj->setDesc(desc.c_str()))
        DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM,
                _("Set object description"));
    
    blocked = false;
}

void ObjectProperties::image_rendering_changed(void)
{
    if (blocked)
    {
        return;
    }
    
    SPItem *item = sp_desktop_selection(SP_ACTIVE_DESKTOP)->singleItem();
    g_return_if_fail (item != NULL);

    blocked = true;

    Glib::ustring scale = ComboBoxTextImageRendering.get_active_text();

    // We should unset if the parent computed value is auto and the desired value is auto.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "image-rendering", scale.c_str());
    Inkscape::XML::Node *image_node = item->getRepr();
    if( image_node ) {
        sp_repr_css_change(image_node, css, "style");
    }
    sp_repr_css_attr_unref( css );
        
    blocked = false;
}

void ObjectProperties::sensitivity_toggled (void)
{
    if (blocked)
    {
        return;
    }

    SPItem *item = sp_desktop_selection(SP_ACTIVE_DESKTOP)->singleItem();
    g_return_if_fail (item != NULL);

    blocked = true;
    item->setLocked(CBLock.get_active());
    DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM,
               CBLock.get_active()? _("Lock object") : _("Unlock object"));
    blocked = false;
}

void ObjectProperties::hidden_toggled(void)
{
    if (blocked)
    {
        return;
    }

    SPItem *item = sp_desktop_selection(SP_ACTIVE_DESKTOP)->singleItem();
    g_return_if_fail (item != NULL);

    blocked = true;
    item->setExplicitlyHidden(CBHide.get_active());
    DocumentUndo::done(SP_ACTIVE_DOCUMENT, SP_VERB_DIALOG_ITEM,
               CBHide.get_active()? _("Hide object") : _("Unhide object"));
    blocked = false;
}

void ObjectProperties::setDesktop(SPDesktop *desktop)
{
    Panel::setDesktop(desktop);
    deskTrack.setBase(desktop);
}

void ObjectProperties::setTargetDesktop(SPDesktop *desktop)
{
    if (this->desktop != desktop) {
        if (this->desktop) {
            subselChangedConn.disconnect();
            selectChangedConn.disconnect();
        }
        this->desktop = desktop;
        if (desktop && desktop->selection) {
            selectChangedConn = desktop->selection->connectChanged(sigc::hide(sigc::mem_fun(*this, &ObjectProperties::widget_setup)));
            subselChangedConn = desktop->connectToolSubselectionChanged(sigc::hide(sigc::mem_fun(*this, &ObjectProperties::widget_setup)));
        }
        widget_setup();
    }
}
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
