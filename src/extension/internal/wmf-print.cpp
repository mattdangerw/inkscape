/** @file
 * @brief Windows Metafile printing
 */
/* Authors:
 *   Ulf Erikson <ulferikson@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   David Mathog
 *
 * Copyright (C) 2006-2009 Authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */
/*
 * References:
 *  - How to Create & Play Enhanced Metafiles in Win32
 *      http://support.microsoft.com/kb/q145999/
 *  - INFO: Windows Metafile Functions & Aldus Placeable Metafiles
 *      http://support.microsoft.com/kb/q66949/
 *  - Metafile Functions
 *      http://msdn.microsoft.com/library/en-us/gdi/metafile_0whf.asp
 *  - Metafile Structures
 *      http://msdn.microsoft.com/library/en-us/gdi/metafile_5hkj.asp
 */


#ifdef HAVE_CONFIG_H
# include "config.h"
#endif


#include "2geom/sbasis-to-bezier.h"
#include "2geom/svg-elliptical-arc.h"

#include "2geom/path.h"
#include "2geom/pathvector.h"
#include "2geom/rect.h"
#include "2geom/bezier-curve.h"
#include "2geom/hvlinesegment.h"
#include "helper/geom.h"
#include "helper/geom-curves.h"
#include "sp-item.h"

#include "style.h"
#include "inkscape-version.h"
#include "sp-root.h"


#include "unit-constants.h"

#include "extension/system.h"
#include "extension/print.h"
#include "document.h"
#include "path-prefix.h"
#include "sp-pattern.h"
#include "sp-image.h"
#include "sp-gradient.h"
#include "sp-radial-gradient.h"
#include "sp-linear-gradient.h"

#include "splivarot.h"             // pieces for union on shapes
#include "2geom/svg-path-parser.h" // to get from SVG text to Geom::Path
#include "display/canvas-bpath.h"  // for SPWindRule

#include "wmf-print.h"


#include <string.h>
extern "C" {
#include "libunicode-convert/unicode-convert.h"
}


namespace Inkscape {
namespace Extension {
namespace Internal {

#define PXPERMETER 2835
#define MAXDISP 2.0 // This should be set in the output dialog.  This is ok for experimenting, no more than 2 pixel deviation.  Not actually used at present

enum drawmode {DRAW_PAINT, DRAW_PATTERN, DRAW_IMAGE, DRAW_LINEAR_GRADIENT, DRAW_RADIAL_GRADIENT};

struct FFNEXUS {
    char        *fontname;   //Font name
    FFNEXUS     *next;       //link to next nexus, NULL if this is the last
    double       f1;         //Vertical (rotating) offset factor (* font height)
    double       f2;         //Vertical (nonrotating) offset factor (* font height)
    double       f3;         //Horizontal (nonrotating) offset factor (* font height)
};

struct GRADVALUES{
    Geom::Point  p1;         // center   or start
    Geom::Point  p2;         // xhandle  or end
    Geom::Point  p3;         // yhandle  or unused
    double       r;          // radius   or unused
    void        *grad;       // to access the stops information
    int          mode;       // DRAW_LINEAR_GRADIENT or DRAW_RADIAL_GRADIENT, if GRADVALUES is valid, else any value
    U_COLORREF   bgc;        // document background color, this is as good a place as any to keep it
    float        rgb[3];     // also background color, but as 0-1 float.
};

/* globals */
static double       PX2WORLD = 1200.0/90.0;     // inkscape is 90 dpi, WMF file is 1200
static bool         FixPPTCharPos, FixPPTDashLine, FixPPTGrad2Polys, FixPPTPatternAsHatch;
static FFNEXUS     *wmf_short_fflist = NULL;  //only those fonts so far encountered
static FFNEXUS     *wmf_long_fflist  = NULL;   //all the fonts described in ...\share\extensions\fontfix.conf
static WMFTRACK    *wt               = NULL;
static WMFHANDLES  *wht              = NULL;
static GRADVALUES   gv;

void PrintWmf::read_system_fflist(void){  //this is not called by any other source files
    FFNEXUS        *temp=NULL;
    FFNEXUS        *ptr=NULL;
    std::fstream    fffile;
    std::string     instr;
    char            fontname[128];
    double          f1,f2,f3;
    std::string     path_to_ffconf;

    if(wmf_long_fflist)return;
    char *oldlocale = g_strdup(setlocale(LC_NUMERIC, NULL));
    setlocale(LC_NUMERIC, "C");

    path_to_ffconf=INKSCAPE_EXTENSIONDIR;
#ifdef WIN32
    path_to_ffconf.append("\\fontfix.conf"); //Windows path syntax
#else
    path_to_ffconf.append("/fontfix.conf"); //Unix/linx path syntax
#endif
    //open the input
    fffile.open(path_to_ffconf.c_str(), std::ios::in);
    if(!fffile.is_open()){
        g_error("Unable to open file: %s\n", path_to_ffconf.c_str());
    }
    while (std::getline(fffile,instr)){
        if(instr[0]=='#')continue;
        // not a comment, get the 4 values from the line
        int elements=sscanf(instr.c_str(),"%lf %lf %lf %127[^\n]",&f1,&f2,&f3, &fontname[0]);
        if(elements!=4){
            g_error("Expected \"f1 f2 f3 Fontname\" but did not find it in file: %s\n", path_to_ffconf.c_str());
        }
        temp=(FFNEXUS *) calloc(1,sizeof(FFNEXUS)); //This will never be freed
        temp->f1=f1;
        temp->f2=f2;
        temp->f3=f3;
        temp->fontname=strdup(fontname); //This will never be freed
        temp->next=NULL;  //just to be explicit, it is already 0
        if(ptr){
            ptr->next=temp;
            ptr=temp;
        }
        else {
            wmf_long_fflist=ptr=temp;
        }
    }
    fffile.close();

    setlocale(LC_NUMERIC, oldlocale);
    g_free(oldlocale);
}

/* Looks for the fontname in the long list.  If it does not find it, it adds the default values
to the short list with this fontname.  If it does find it, then it adds the specified values.
*/
void  PrintWmf::search_long_fflist(const char *fontname, double *f1, double *f2, double *f3){  //this is not called by any other source files
    FFNEXUS *ptr=NULL;
    FFNEXUS *tmp=wmf_long_fflist;
    if(!wmf_long_fflist){
        g_error("Programming error search_long_fflist called before read_system_fflist\n");
    }
    ptr=wmf_long_fflist;
    while(ptr){
        if(!strcmp(ptr->fontname,fontname)){ tmp=ptr; break; }
        ptr=ptr->next;
    }
    //tmp points at either the found name, or the default, the first entry in wmf_long_fflist
    if(!wmf_short_fflist){
        ptr=wmf_short_fflist=(FFNEXUS *) malloc(sizeof(FFNEXUS));
    }
    else {
        ptr=wmf_short_fflist;
        while(ptr->next){ ptr=ptr->next; }
        ptr->next=(FFNEXUS *) malloc(sizeof(FFNEXUS));
        ptr=ptr->next;
    }
    ptr->fontname=strdup(tmp->fontname);
    *f1 = ptr->f1 = tmp->f1;
    *f2 = ptr->f2 = tmp->f2;
    *f3 = ptr->f3 = tmp->f3;
    ptr->next=NULL;
}

/* Looks for the fontname in the short list.  If it does not find it, it looks in the wmf_long_fflist.
Either way it returns the f1, f2, f3 parameters for the font, even if these are for the default.
*/
void  PrintWmf::search_short_fflist(const char *fontname, double *f1, double *f2, double *f3){  //this is not called by any other source files
    FFNEXUS *ptr=NULL;
    static FFNEXUS *last=NULL;

    if(!wmf_long_fflist){
        g_error("Programming error search_short_fflist called before read_system_fflist\n");
    }
    // This speeds things up a lot - if the same font is called twice in a row, pull it out immediately
    if(last && !strcmp(last->fontname,fontname)){ ptr=last;         }
    else {                                        ptr=wmf_short_fflist; }  // wmf_short_fflist may still be NULL
    while(ptr){
        if(!strcmp(ptr->fontname,fontname)){ *f1=ptr->f1; *f2=ptr->f2; *f3=ptr->f3; last=ptr; return; }
        ptr=ptr->next;
    }
    //reach this point only if there is no match
    search_long_fflist(fontname, f1, f2, f3);
}

void PrintWmf::smuggle_adxky_out(const char *string, int16_t **adx, double *ky, int *rtl, int *ndx, float scale){
    float       fdx;
    int         i;
    int16_t   *ladx;
    const char *cptr=&string[strlen(string)+1]; // this works because of the first fake terminator

    *adx = NULL;
    *ky  = 0.0;       // set a default value
    sscanf(cptr,"%7d",ndx);
    if(!*ndx)return;  // this could happen with an empty string
    cptr += 7;
    ladx = (int16_t *) malloc(*ndx * sizeof(int16_t) );
    if(!ladx)g_error("Out of memory");
    *adx=ladx;
    for(i=0; i<*ndx; i++,cptr+=7, ladx++){
        sscanf(cptr,"%7f",&fdx);
        *ladx=(int16_t) round(fdx * scale);
    }
    cptr++; // skip 2nd fake terminator
    sscanf(cptr,"%7f",&fdx);
    *ky=fdx;
    cptr += 7;  // advance over ky and its space
    sscanf(cptr,"%07d",rtl);
}

/* convert an  0RGB color to EMF U_COLORREF.
inverse of sethexcolor() in wmf-inout.cpp
*/
U_COLORREF  PrintWmf::gethexcolor(uint32_t color){
    U_COLORREF out;
    out =   U_RGB( 
                (color >> 16) & 0xFF,
                (color >>  8) & 0xFF,
                (color >>  0) & 0xFF
            );
    return(out);
}


/* Translate inkscape weights to WMF weights.
*/
uint32_t PrintWmf::transweight(const unsigned int inkweight){
    if(inkweight == SP_CSS_FONT_WEIGHT_400)return(U_FW_NORMAL);
    if(inkweight == SP_CSS_FONT_WEIGHT_100)return(U_FW_THIN);
    if(inkweight == SP_CSS_FONT_WEIGHT_200)return(U_FW_EXTRALIGHT);
    if(inkweight == SP_CSS_FONT_WEIGHT_300)return(U_FW_LIGHT);
    // 400 is tested first, as it is the most common case
    if(inkweight == SP_CSS_FONT_WEIGHT_500)return(U_FW_MEDIUM);
    if(inkweight == SP_CSS_FONT_WEIGHT_600)return(U_FW_SEMIBOLD);
    if(inkweight == SP_CSS_FONT_WEIGHT_700)return(U_FW_BOLD);
    if(inkweight == SP_CSS_FONT_WEIGHT_800)return(U_FW_EXTRABOLD);
    if(inkweight == SP_CSS_FONT_WEIGHT_900)return(U_FW_HEAVY);
    return(U_FW_NORMAL);
}

PrintWmf::PrintWmf (void)
{
    // all of the class variables are initialized elsewhere, many in PrintWmf::Begin,
}


PrintWmf::~PrintWmf (void)
{

    /* restore default signal handling for SIGPIPE */
#if !defined(_WIN32) && !defined(__WIN32__)
    (void) signal(SIGPIPE, SIG_DFL);
#endif
    return;
}


unsigned int PrintWmf::setup (Inkscape::Extension::Print * /*mod*/)
{
    return TRUE;
}


unsigned int PrintWmf::begin (Inkscape::Extension::Print *mod, SPDocument *doc)
{
    char           *rec;
    gchar const    *utf8_fn = mod->get_param_string("destination");

    FixPPTCharPos        = mod->get_param_bool("FixPPTCharPos");
    FixPPTDashLine       = mod->get_param_bool("FixPPTDashLine");
    FixPPTGrad2Polys     = mod->get_param_bool("FixPPTGrad2Polys");
    FixPPTPatternAsHatch = mod->get_param_bool("FixPPTPatternAsHatch");

    (void) wmf_start(utf8_fn, 1000000, 250000, &wt);  // Initialize the wt structure
    (void) wmf_htable_create(128, 128, &wht);         // Initialize the wht structure

    // WMF header the only things that can be set are the page size in inches (w,h) and the dpi
    // width and height in px
    _width  = doc->getWidth();
    _height = doc->getHeight();

    // initialize a few global variables
    hbrush = hpen = 0;
    htextalignment = U_TA_BASELINE | U_TA_LEFT;
    use_stroke = use_fill = simple_shape = usebk = false;

    Inkscape::XML::Node *nv = sp_repr_lookup_name (doc->rroot, "sodipodi:namedview");
    if(nv){
        const char *p1 = nv->attribute("pagecolor");
        char *p2;
        uint32_t lc = strtoul( &p1[1], &p2, 16 );  // it looks like "#ABC123"
        if(*p2)lc=0;
        gv.bgc = gethexcolor(lc);
        gv.rgb[0] = (float) U_RGBAGetR(gv.bgc)/255.0;
        gv.rgb[1] = (float) U_RGBAGetG(gv.bgc)/255.0;
        gv.rgb[2] = (float) U_RGBAGetB(gv.bgc)/255.0;
    }

    bool pageBoundingBox;
    pageBoundingBox = mod->get_param_bool("pageBoundingBox");

    Geom::Rect d;
    if (pageBoundingBox) {
        d = Geom::Rect::from_xywh(0, 0, _width, _height);
    } else {
        SPItem* doc_item = doc->getRoot();
        Geom::OptRect bbox = doc_item->desktopVisualBounds();
        if (bbox) d = *bbox;
    }

    d *= Geom::Scale(IN_PER_PX);  // 90 dpi inside inkscape, wmf file will be 1200 dpi

    /* -1/1200 in next two lines so that WMF read in will write out again at exactly the same size */
    float dwInchesX = d.width()  - 1.0/1200.0;
    float dwInchesY = d.height() - 1.0/1200.0;
    int   dwPxX     = round(dwInchesX * 1200.0);  
    int   dwPxY     = round(dwInchesY * 1200.0); 
#if 0
    float dwInchesX = d.width();
    float dwInchesY = d.height();
    int   dwPxX     = round(d.width()  * 1200.0);  
    int   dwPxY     = round(d.height() * 1200.0); 
#endif

    PU_PAIRF ps = U_PAIRF_set(dwInchesX, dwInchesY);
    rec = U_WMRHEADER_set(ps,1200); // Example: drawing is A4 horizontal,  1200 dpi
    if(!rec){
        g_error("Fatal programming error in PrintWmf::begin at WMRSETMAPMODE");
    }
    (void) wmf_header_append((PU_METARECORD)rec, wt, 1);
    free(ps);

    rec = U_WMRSETWINDOWEXT_set(point16_set( dwPxX, dwPxY));
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at WMRSETWINDOWEXT");
    }
    
    rec = U_WMRSETWINDOWORG_set(point16_set(0,0));
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at WMRSETWINDOWORG");
    }

    rec = U_WMRSETMAPMODE_set(U_MM_ANISOTROPIC);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at WMRSETMAPMODE");
    }
    
    /* set some parameters, else the program that reads the WMF may default to other values */

    rec = U_WMRSETBKMODE_set(U_TRANSPARENT);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at U_WMRSETBKMODE");
    }

    hpolyfillmode=U_WINDING;
    rec = U_WMRSETPOLYFILLMODE_set(U_WINDING);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at U_WMRSETPOLYFILLMODE");
    }

    // Text alignment:  (only changed if RTL text is encountered )
    //   - (x,y) coordinates received by this filter are those of the point where the text
    //     actually starts, and already takes into account the text object's alignment;
    //   - for this reason, the WMF text alignment must always be TA_BASELINE|TA_LEFT.
    rec = U_WMRSETTEXTALIGN_set(U_TA_BASELINE | U_TA_LEFT);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at U_WMRSETTEXTALIGN_set");
    }

    htextcolor_rgb[0] = htextcolor_rgb[1] = htextcolor_rgb[2] = 0.0; 
    rec = U_WMRSETTEXTCOLOR_set(U_RGB(0,0,0));
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at U_WMRSETTEXTCOLOR_set");
    }

    rec = U_WMRSETROP2_set(U_R2_COPYPEN);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at U_WMRSETROP2");
    }
    
    hmiterlimit=5;
    rec = wmiterlimit_set(5);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at wmiterlimit_set");
    }

    
    // create a pen as object 0.  We never use it (except by mistake).  Its purpose it to make all of the other object indices >=1
    U_PEN up = U_PEN_set(U_PS_SOLID, 1, colorref_set(0,0,0));
    uint32_t   Pen;
    rec = wcreatepenindirect_set(&Pen, wht, up);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at wcreatepenindirect_set");
    }
    
    // create a null pen.  If no specific pen is set, this is used
    up = U_PEN_set(U_PS_NULL, 1, colorref_set(0,0,0));
    rec = wcreatepenindirect_set(&hpen_null, wht, up);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at wcreatepenindirect_set");
    }
    destroy_pen(); // make this pen active

    // create a null brush.  If no specific brush is set, this is used
    U_WLOGBRUSH lb = U_WLOGBRUSH_set(U_BS_NULL, U_RGB(0, 0, 0), U_HS_HORIZONTAL);
    rec = wcreatebrushindirect_set(&hbrush_null, wht, lb);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::begin at wcreatebrushindirect_set");
    }
    destroy_brush(); // make this brush active

    return 0;
}


unsigned int PrintWmf::finish (Inkscape::Extension::Print * /*mod*/)
{
    char *rec;
    if (!wt) return 0;

    // get rid of null brush
    rec = wdeleteobject_set(&hbrush_null, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::finish at wdeleteobject_set null brush");
    }
    
    // get rid of null pen
    rec = wdeleteobject_set(&hpen_null, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::finish at wdeleteobject_set null pen");
    }
    
    // get rid of object 0, which was a pen that was used to shift the other object indices to >=1.
        hpen=0;
    rec = wdeleteobject_set(&hpen, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::finish at wdeleteobject_set filler object");
    }

    rec = U_WMREOF_set();  // generate the EOF record
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::finish");
    }
    (void) wmf_finish(wt); // Finalize and write out the WMF
    wmf_free(&wt);              // clean up
    wmf_htable_free(&wht);          // clean up

    return 0;
}


unsigned int PrintWmf::comment ( Inkscape::Extension::Print * /*module*/, const char * /*comment*/)
{
    if (!wt) return 0;

    // earlier versions had flush of fill here, but it never executed and was removed

    return 0;
}

// Extract hatchType, hatchColor from a name like
// *MFhatch<hatchType>_<hatchColor>[_<bkcolor>]  (WMF or EMF hatches are the same)
// Where the first one is a number and the second (and third) a color in hex.
// hatchType, hatchColor, bkColor have been set with defaults before this is called.
//
void PrintWmf::hatch_classify(char *name, int *hatchType, U_COLORREF *hatchColor, U_COLORREF *bkColor){
    int      val;
    uint32_t hcolor=0;
    uint32_t bcolor=0;

    // name should be EMFhatch or WMFhatch but *MFhatch will be accepted
    if(0!=strncmp(&name[1],"MFhatch",7)){ return; } // not anything we can parse
    name += 8; // WMFhatch already detected
    val   = 0;
    while(*name && isdigit(*name)){ 
        val = 10*val + *name - '0';
        name++;
    }
    *hatchType = val;
    if(*name != '_' || val > U_HS_DITHEREDBKCLR){ // wrong syntax, cannot classify
        *hatchType = -1;
    }
    else {
        name++;
        if(2 != sscanf(name,"%X_%X", &hcolor, &bcolor)){ // not a pattern with background
            if(1 != sscanf(name,"%X", &hcolor)){ *hatchType = -1; } // not a pattern, cannot classify
            *hatchColor = gethexcolor(hcolor);
        }
        else {
            *hatchColor = gethexcolor(hcolor);
            *bkColor    = gethexcolor(bcolor);
            usebk       = true;
        }
    }
    /* Everything > U_HS_SOLIDCLR is solid, just specify the color in the brush rather than messing around with background or textcolor */
    if(*hatchType > U_HS_SOLIDCLR)*hatchType = U_HS_SOLIDCLR;
}

//
//  Recurse down from a brush pattern, try to figure out what it is. 
//  If an image is found set a pointer to the epixbuf, else set that to NULL
//  If a pattern is found with a name like [EW]MFhatch3_3F7FFF return hatchType=3, hatchColor=3F7FFF (as a uint32_t),
//    otherwise hatchType is set to -1 and hatchColor is not defined.
//

void PrintWmf::brush_classify(SPObject *parent, int depth, GdkPixbuf **epixbuf, int *hatchType, U_COLORREF *hatchColor, U_COLORREF *bkColor){
    if(depth==0){
        *epixbuf    = NULL;
        *hatchType  = -1;
        *hatchColor = U_RGB(0,0,0);
        *bkColor    = U_RGB(255,255,255);
    }
    depth++;
    // first look along the pattern chain, if there is one 
    if(SP_IS_PATTERN(parent)){
        for (SPPattern *pat_i = SP_PATTERN(parent); pat_i != NULL; pat_i = pat_i->ref ? pat_i->ref->getObject() : NULL) {
            if(SP_IS_IMAGE(pat_i)){
                *epixbuf = ((SPImage *)pat_i)->pixbuf;
                return;
            }
            char temp[32];  // large enough
            temp[31]='\0';
            strncpy(temp,pat_i->getAttribute("id"),31);  // Some names may be longer than [EW]MFhatch#_###### 
            hatch_classify(temp, hatchType, hatchColor, bkColor);
            if(*hatchType != -1)return;

            // still looking?  Look at this pattern's children, if there are any
            SPObject *child = pat_i->firstChild();
            while(child && !(*epixbuf) && (*hatchType == -1)){
                brush_classify(child, depth, epixbuf, hatchType, hatchColor, bkColor);
                child = child->getNext();
            }
        }
    }
    else if(SP_IS_IMAGE(parent)){
        *epixbuf = ((SPImage *)parent)->pixbuf;
        return;
    }
    else { // some inkscape rearrangements pass through nodes between pattern and image which are not classified as either.
        SPObject *child = parent->firstChild();
        while(child && !(*epixbuf) && (*hatchType == -1)){
            brush_classify(child, depth, epixbuf, hatchType, hatchColor, bkColor);
            child = child->getNext();
        }
    }
}

//swap R/B in 4 byte pixel
void PrintWmf::swapRBinRGBA(char *px, int pixels){
    char tmp;
    for(int i=0;i<pixels*4;px+=4,i+=4){
        tmp=px[2];
        px[2]=px[0];
        px[0]=tmp;
    }
}

/* opacity weighting of two colors as float.  v1 is the color, op is its opacity, v2 is the background color */
inline float opweight(float v1, float v2, float op){
    return v1*op + v2*(1.0-op);
}

U_COLORREF PrintWmf::avg_stop_color(SPGradient *gr){
    U_COLORREF cr;
    int last = gr->vector.stops.size() -1;
    if(last>=1){
        float rgbs[3];
        float rgbe[3];
        float ops,ope;

        ops = gr->vector.stops[0   ].opacity;
        ope = gr->vector.stops[last].opacity;
        sp_color_get_rgb_floatv(&gr->vector.stops[0   ].color, rgbs);
        sp_color_get_rgb_floatv(&gr->vector.stops[last].color, rgbe);

        /* Replace opacity at start & stop with that fraction background color, then average those two for final color. */
        cr =    U_RGB(
                    255*(( opweight(rgbs[0],gv.rgb[0],ops)   +   opweight(rgbe[0],gv.rgb[0],ope) )/2.0),
                    255*(( opweight(rgbs[1],gv.rgb[1],ops)   +   opweight(rgbe[1],gv.rgb[1],ope) )/2.0),
                    255*(( opweight(rgbs[2],gv.rgb[2],ops)   +   opweight(rgbe[2],gv.rgb[2],ope) )/2.0)
                );
    }
    else {
        cr = U_RGB(0, 0, 0);  // The default fill
    }
    return cr;
}

int PrintWmf::hold_gradient(void *gr, int mode){
    gv.mode = mode;
    gv.grad = gr;
    if(mode==DRAW_RADIAL_GRADIENT){
        SPRadialGradient *rg = (SPRadialGradient *) gr;
        gv.r  = rg->r.computed;                                 // radius, but of what???
        gv.p1 = Geom::Point(rg->cx.computed, rg->cy.computed);  // center
        gv.p2 = Geom::Point(gv.r, 0) + gv.p1;                   // xhandle
        gv.p3 = Geom::Point(0, -gv.r) + gv.p1;                  // yhandle
        if (rg->gradientTransform_set) {
            gv.p1 = gv.p1 * rg->gradientTransform;
            gv.p2 = gv.p2 * rg->gradientTransform;
            gv.p3 = gv.p3 * rg->gradientTransform;
        }
    }
    else if(mode==DRAW_LINEAR_GRADIENT){
        SPLinearGradient *lg = (SPLinearGradient *) gr;
        gv.r = 0;                                               // unused
        gv.p1 = Geom::Point (lg->x1.computed, lg->y1.computed); // start
        gv.p2 = Geom::Point (lg->x2.computed, lg->y2.computed); // end
        gv.p3 = Geom::Point (0, 0);                             // unused
        if (lg->gradientTransform_set) {
            gv.p1 = gv.p1 * lg->gradientTransform;
            gv.p2 = gv.p2 * lg->gradientTransform;
        }
    }
    else {
        g_error("Fatal programming error, hold_gradient() in wmf-print.cpp called with invalid draw mode");
    }
    return 1;
}

// fcolor is defined when gradients are being expanded, it is the color of one stripe or ring.
int PrintWmf::create_brush(SPStyle const *style, PU_COLORREF fcolor)
{
    float         rgb[3];
    char         *rec;
    U_WLOGBRUSH   lb;
    uint32_t      brush, fmode;
    enum drawmode fill_mode;
    GdkPixbuf    *pixbuf;
    uint32_t      brushStyle;
    int           hatchType;
    U_COLORREF    hatchColor;
    U_COLORREF    bkColor;
    uint32_t      width  = 0; // quiets a harmless compiler warning, initialization not otherwise required.
    uint32_t      height = 0;

    if (!wt) return 0;

    // set a default fill in case we can't figure out a better way to do it
    fmode      = U_ALTERNATE;
    fill_mode  = DRAW_PAINT;
    brushStyle = U_BS_SOLID;
    hatchType  = U_HS_SOLIDCLR;
    bkColor    = U_RGB(0, 0, 0);
    if(fcolor){ hatchColor = *fcolor;        }
    else {      hatchColor = U_RGB(0, 0, 0); }

    if (!fcolor && style) {
        if(style->fill.isColor()){
            fill_mode = DRAW_PAINT;
            float opacity = SP_SCALE24_TO_FLOAT(style->fill_opacity.value);
            if (opacity <= 0.0)opacity = 0.0;  // basically the same as no fill

            sp_color_get_rgb_floatv( &style->fill.value.color, rgb );
            hatchColor = U_RGB(255*rgb[0], 255*rgb[1], 255*rgb[2]);

            fmode = style->fill_rule.computed == 0 ? U_WINDING : (style->fill_rule.computed == 2 ? U_ALTERNATE : U_ALTERNATE);
        }
        else if(SP_IS_PATTERN(SP_STYLE_FILL_SERVER(style))){ // must be paint-server
            SPPaintServer *paintserver = style->fill.value.href->getObject();
            SPPattern *pat = SP_PATTERN (paintserver);
            double dwidth  = pattern_width(pat);
            double dheight = pattern_height(pat);
            width  = dwidth;
            height = dheight;
            brush_classify(pat,0,&pixbuf,&hatchType,&hatchColor,&bkColor);
            if(pixbuf){ fill_mode = DRAW_IMAGE;  }
            else {  // pattern
                fill_mode = DRAW_PATTERN;
                if(hatchType == -1){  // Not a standard hatch, so force it to something
                    hatchType  = U_HS_CROSS;
                    hatchColor = U_RGB(0xFF,0xC3,0xC3);
                }
            }
            if(FixPPTPatternAsHatch){
                if(hatchType == -1){  // image or unclassified 
                    fill_mode  = DRAW_PATTERN;
                    hatchType  = U_HS_DIAGCROSS;
                    hatchColor = U_RGB(0xFF,0xC3,0xC3);
                } 
            }
            brushStyle = U_BS_HATCHED;
        }
        else if(SP_IS_GRADIENT(SP_STYLE_FILL_SERVER(style))){ // must be a gradient
            // currently we do not do anything with gradients, the code below just sets the color to the average of the stops
            SPPaintServer *paintserver = style->fill.value.href->getObject();
            SPLinearGradient *lg = NULL;
            SPRadialGradient *rg = NULL;

            if (SP_IS_LINEARGRADIENT (paintserver)) {
                lg = SP_LINEARGRADIENT(paintserver);
                SP_GRADIENT(lg)->ensureVector(); // when exporting from commandline, vector is not built
                fill_mode = DRAW_LINEAR_GRADIENT;
            }
            else if (SP_IS_RADIALGRADIENT (paintserver)) {
                rg = SP_RADIALGRADIENT(paintserver);
                SP_GRADIENT(rg)->ensureVector(); // when exporting from commandline, vector is not built
                fill_mode = DRAW_RADIAL_GRADIENT;
            }
            else {
                // default fill
            }

            if(rg){
                if(FixPPTGrad2Polys){  return hold_gradient(rg, fill_mode); }
                else {                 hatchColor = avg_stop_color(rg);  }
            }
            else if(lg){
                if(FixPPTGrad2Polys){  return hold_gradient(lg, fill_mode); }
                else {                 hatchColor = avg_stop_color(lg);   }
            }
        }
    } 
    else { // if (!style)
        // default fill
    }

    switch(fill_mode){
        case DRAW_LINEAR_GRADIENT: // fill with average color unless gradients are converted to slices
        case DRAW_RADIAL_GRADIENT: // ditto
        case DRAW_PAINT:
        case DRAW_PATTERN:
            // SVG text has no background attribute, so OPAQUE mode ALWAYS cancels after the next draw, otherwise it would mess up future text output.
            if(usebk){
                rec = U_WMRSETBKCOLOR_set(bkColor);
                if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                    g_error("Fatal programming error in PrintWmf::create_brush at U_WMRSETBKCOLOR_set");
                }
                rec = U_WMRSETBKMODE_set(U_OPAQUE);
                if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                    g_error("Fatal programming error in PrintWmf::create_brush at U_WMRSETBKMODE_set");
                }
            }
            lb   = U_WLOGBRUSH_set(brushStyle, hatchColor, hatchType);
            rec = wcreatebrushindirect_set(&brush, wht, lb);
            if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                g_error("Fatal programming error in PrintWmf::create_brush at createbrushindirect_set");
            }
            break;
        case DRAW_IMAGE:
            char                *px;
            char                *rgba_px;
            uint32_t             cbPx;
            uint32_t             colortype;
            PU_RGBQUAD           ct;
            int                  numCt;
            U_BITMAPINFOHEADER   Bmih;
            PU_BITMAPINFO        Bmi;
            rgba_px = (char *) gdk_pixbuf_get_pixels(pixbuf); // Do NOT free this!!!
            colortype = U_BCBM_COLOR32;
            (void) RGBA_to_DIB(&px, &cbPx, &ct, &numCt,  rgba_px,  width, height, width*4, colortype, 0, 1);
            // Not sure why the next swap is needed because the preceding does it, and the code is identical
            // to that in stretchdibits_set, which does not need this.
            swapRBinRGBA(px, width*height);
            Bmih = bitmapinfoheader_set(width, height, 1, colortype, U_BI_RGB, 0, PXPERMETER, PXPERMETER, numCt, 0);
            Bmi = bitmapinfo_set(Bmih, ct);
            rec = wcreatedibpatternbrush_srcdib_set(&brush, wht, U_DIB_RGB_COLORS, Bmi, cbPx, px);
            if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                g_error("Fatal programming error in PrintWmf::create_brush at createdibpatternbrushpt_set");
            }
            free(px);
            free(Bmi); // ct will be NULL because of colortype
            break;
    }

    hbrush = brush;  // need this later for destroy_brush
    rec = wselectobject_set(brush, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::create_brush at wselectobject_set");
    }

    if(fmode != hpolyfillmode){
        hpolyfillmode=fmode;
        rec = U_WMRSETPOLYFILLMODE_set(fmode); 
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::create_brush at U_WMRSETPOLYFILLMODE_set");
        }
    }

    return 0;
}


void PrintWmf::destroy_brush()
{
    char *rec;
    // WMF lets any object be deleted whenever, and the chips fall where they may...
    if (hbrush){
        rec = wdeleteobject_set(&hbrush, wht);
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::destroy_brush");
        }
        hbrush = 0;
    }
    
    // (re)select the null brush
    
    rec = wselectobject_set(hbrush_null, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::destroy_brush");
    }    
}


int PrintWmf::create_pen(SPStyle const *style, const Geom::Affine &transform)
{
    char                *rec       = NULL;
    uint32_t             pen;
    uint32_t             penstyle;
    U_COLORREF           penColor;
    U_PEN                up;
    int                  modstyle;
  
    if (!wt) return 0;

    // set a default stroke  in case we can't figure out a better way to do it
    penstyle = U_PS_SOLID;
    modstyle = 0;
    penColor = U_RGB(0, 0, 0);
    uint32_t linewidth = 1;

    if (style) {  // override some or all of the preceding 
        float rgb[3];

        // WMF does not support hatched, bitmap, or gradient pens, just set the color.
        sp_color_get_rgb_floatv( &style->stroke.value.color, rgb );
        penColor = U_RGB(255*rgb[0], 255*rgb[1], 255*rgb[2]);

        using Geom::X;
        using Geom::Y;

        Geom::Point zero(0, 0);
        Geom::Point one(1, 1);
        Geom::Point p0(zero * transform);
        Geom::Point p1(one * transform);
        Geom::Point p(p1 - p0);

        double scale = sqrt( (p[X]*p[X]) + (p[Y]*p[Y]) ) / sqrt(2);

        if(!style->stroke_width.computed){return 0;}  //if width is 0 do not (reset) the pen, it should already be NULL_PEN
        linewidth = MAX( 1, (uint32_t) round(scale * style->stroke_width.computed * PX2WORLD) );

        // most WMF readers will ignore linecap and linejoin, but set them anyway.  Inkscape itself can read them back in.
 
        if (     style->stroke_linecap.computed == 0) {  modstyle |= U_PS_ENDCAP_FLAT;   }
        else if (style->stroke_linecap.computed == 1) {  modstyle |= U_PS_ENDCAP_ROUND;  }
        else {                                           modstyle |= U_PS_ENDCAP_SQUARE; }

        if (style->stroke_linejoin.computed == 0) {
            float miterlimit = style->stroke_miterlimit.value;  // This is a ratio.
            if (miterlimit < 1)miterlimit = 1;

            // most WMF readers will ignore miterlimit, but set it anyway.  Inkscape itself can read it back in
            if((uint32_t)miterlimit != hmiterlimit){
                hmiterlimit = (uint32_t)miterlimit;
                rec = wmiterlimit_set((uint32_t) miterlimit);
                if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                    g_error("Fatal programming error in PrintWmf::create_pen at wmiterlimit_set");
                }
            }
                                                         modstyle |= U_PS_JOIN_MITER;
        }
        else if (style->stroke_linejoin.computed == 1) { modstyle |= U_PS_JOIN_ROUND; }
        else {                                           modstyle |= U_PS_JOIN_BEVEL; }

        if (style->stroke_dash.n_dash   &&
            style->stroke_dash.dash       )
        {
            if(!FixPPTDashLine){ // if this is set code elsewhere will break dots/dashes into many smaller lines.
                penstyle = U_PS_DASH;// userstyle not supported apparently, for now map all Inkscape dot/dash to just dash
            }
        }

    }

    up  = U_PEN_set(penstyle | modstyle, linewidth, penColor);
    rec = wcreatepenindirect_set(&pen, wht, up);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::create_pen at wcreatepenindirect_set");
    }

    rec = wselectobject_set(pen, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::create_pen at wselectobject_set");
    }
    hpen = pen;  // need this later for destroy_pen

    return 0;
}

//  delete the defined pen object
void PrintWmf::destroy_pen()
{
    char *rec = NULL;
    // WMF lets any object be deleted whenever, and the chips fall where they may...
    if (hpen){
        rec = wdeleteobject_set(&hpen, wht);
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::destroy_pen");
        }
        hpen = 0;
    }

    // (re)select the null pen
    
    rec = wselectobject_set(hpen_null, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::destroy_pen");
    }
}



unsigned int PrintWmf::bind(Inkscape::Extension::Print * /*mod*/, Geom::Affine const &transform, float /*opacity*/)
{   
    if (!m_tr_stack.empty()) {
        Geom::Affine tr_top = m_tr_stack.top();
        m_tr_stack.push(transform * tr_top);
    } else {
        m_tr_stack.push(transform);
    }

    return 1;
}

unsigned int PrintWmf::release(Inkscape::Extension::Print * /*mod*/)
{
    m_tr_stack.pop();
    return 1;
}

#define clrweight(a,b,t) ((1-t)*((double) a) + (t)*((double) b))
inline U_COLORREF PrintWmf::weight_opacity(U_COLORREF c1){
    float opa = c1.Reserved/255.0;
    U_COLORREF result = U_RGB(
        255*opweight((float)c1.Red  /255.0, gv.rgb[0], opa),
        255*opweight((float)c1.Green/255.0, gv.rgb[1], opa),
        255*opweight((float)c1.Blue /255.0, gv.rgb[2], opa)
    );
    return result;
}


// return the color between c1 and c2, c1 for t=0, c2 for t=1.0
U_COLORREF PrintWmf::weight_colors(U_COLORREF c1, U_COLORREF c2, double t){
    U_COLORREF result;
    result.Red      = clrweight(c1.Red,      c2.Red,      t);
    result.Green    = clrweight(c1.Green,    c2.Green,    t);
    result.Blue     = clrweight(c1.Blue,     c2.Blue,     t);
    result.Reserved = clrweight(c1.Reserved, c2.Reserved, t);

    // now handle the opacity, mix the RGB with background at the weighted opacity
    
    if(result.Reserved != 255)result = weight_opacity(result);

    return result;
}

/*  convert from center ellipse to SVGEllipticalArc ellipse
   
    From:
    http://www.w3.org/TR/SVG/implnote.html#ArcConversionEndpointToCenter
    A point (x,y) on the arc can be found by:
   
    {x,y} = {cx,cy} + {cosF,-sinF,sinF,cosF} x {rxcosT,rysinT}
   
    where 
        {cx,cy} is the center of the ellipse
        F       is the rotation angle of the X axis of the ellipse from the true X axis
        T       is the rotation angle around the ellipse
        {,,,}   is the rotation matrix
        rx,ry   are the radii of the ellipse's axes

    For SVG parameterization need two points.  
    Arbitrarily we can use T=0 and T=pi
    Since the sweep is 180 the flags are always 0:

    F is in RADIANS, but the SVGEllipticalArc needs degrees!

*/
Geom::PathVector PrintWmf::center_ellipse_as_SVG_PathV(Geom::Point ctr, double rx, double ry, double F){
    using Geom::X;
    using Geom::Y;
    double x1,y1,x2,y2;
    Geom::Path SVGep;
 
    x1 = ctr[X]  +  cos(F) * rx * cos(0)      +   sin(-F) * ry * sin(0);
    y1 = ctr[Y]  +  sin(F) * rx * cos(0)      +   cos(F)  * ry * sin(0);
    x2 = ctr[X]  +  cos(F) * rx * cos(M_PI)   +   sin(-F) * ry * sin(M_PI);
    y2 = ctr[Y]  +  sin(F) * rx * cos(M_PI)   +   cos(F)  * ry * sin(M_PI);

    char text[256];
    sprintf(text," M %f,%f A %f %f %f 0 0 %f %f A %f %f %f 0 0 %f %f z",x1,y1,  rx,ry,F*360./(2.*M_PI),x2,y2,   rx,ry,F*360./(2.*M_PI),x1,y1);
    std::vector<Geom::Path> outres =  Geom::parse_svg_path(text);
    return outres;
}


/*  rx2,ry2 must be larger than rx1,ry1!
    angle is in RADIANS
*/
Geom::PathVector PrintWmf::center_elliptical_ring_as_SVG_PathV(Geom::Point ctr, double rx1, double ry1, double rx2, double ry2, double F){
    using Geom::X;
    using Geom::Y;
    double x11,y11,x12,y12;
    double x21,y21,x22,y22;
    double degrot = F*360./(2.*M_PI);
 
    x11 = ctr[X]  +  cos(F) * rx1 * cos(0)      +   sin(-F) * ry1 * sin(0);
    y11 = ctr[Y]  +  sin(F) * rx1 * cos(0)      +   cos(F)  * ry1 * sin(0);
    x12 = ctr[X]  +  cos(F) * rx1 * cos(M_PI)   +   sin(-F) * ry1 * sin(M_PI);
    y12 = ctr[Y]  +  sin(F) * rx1 * cos(M_PI)   +   cos(F)  * ry1 * sin(M_PI);

    x21 = ctr[X]  +  cos(F) * rx2 * cos(0)      +   sin(-F) * ry2 * sin(0);
    y21 = ctr[Y]  +  sin(F) * rx2 * cos(0)      +   cos(F)  * ry2 * sin(0);
    x22 = ctr[X]  +  cos(F) * rx2 * cos(M_PI)   +   sin(-F) * ry2 * sin(M_PI);
    y22 = ctr[Y]  +  sin(F) * rx2 * cos(M_PI)   +   cos(F)  * ry2 * sin(M_PI);

    char text[512];
    sprintf(text," M %f,%f A %f %f %f 0 1 %f %f A %f %f %f 0 1 %f %f z M %f,%f  A %f %f %f 0 0 %f %f A %f %f %f 0 0 %f %f z",
        x11,y11,  rx1,ry1,degrot,x12,y12,   rx1,ry1,degrot,x11,y11,
        x21,y21,  rx2,ry2,degrot,x22,y22,   rx2,ry2,degrot,x21,y21
    );
    std::vector<Geom::Path> outres =  Geom::parse_svg_path(text);

    return outres;
}

/* Elliptical hole in a large square extending from -50k to +50k */
Geom::PathVector PrintWmf::center_elliptical_hole_as_SVG_PathV(Geom::Point ctr, double rx, double ry, double F){
    using Geom::X;
    using Geom::Y;
    double x1,y1,x2,y2;
    Geom::Path SVGep;
 
    x1 = ctr[X]  +  cos(F) * rx * cos(0)      +   sin(-F) * ry * sin(0);
    y1 = ctr[Y]  +  sin(F) * rx * cos(0)      +   cos(F)  * ry * sin(0);
    x2 = ctr[X]  +  cos(F) * rx * cos(M_PI)   +   sin(-F) * ry * sin(M_PI);
    y2 = ctr[Y]  +  sin(F) * rx * cos(M_PI)   +   cos(F)  * ry * sin(M_PI);

    char text[256];
    sprintf(text," M %f,%f A %f %f %f 0 0 %f %f A %f %f %f 0 0 %f %f z M 50000,50000 50000,-50000 -50000,-50000 -50000,50000 z",
        x1,y1,  rx,ry,F*360./(2.*M_PI),x2,y2,   rx,ry,F*360./(2.*M_PI),x1,y1);
    std::vector<Geom::Path> outres =  Geom::parse_svg_path(text);
    return outres;
}

/* rectangular cutter. 
ctr    "center" of rectangle (might not actually be in the center with respect to leading/trailing edges
pos    vector from center to leading edge
neg    vector from center to trailing edge
width  vector to side edge 
*/
Geom::PathVector PrintWmf::rect_cutter(Geom::Point ctr, Geom::Point pos, Geom::Point neg, Geom::Point width){
    std::vector<Geom::Path> outres;
    Geom::Path cutter;
    cutter.start(                       ctr + pos - width);
    cutter.appendNew<Geom::LineSegment>(ctr + pos + width);
    cutter.appendNew<Geom::LineSegment>(ctr + neg + width);
    cutter.appendNew<Geom::LineSegment>(ctr + neg - width);
    cutter.close();
    outres.push_back(cutter);
    return outres;
}

/*  Convert from SPWindRule to livarot's FillRule
    This is similar to what sp_selected_path_boolop() does
*/
FillRule PrintWmf::SPWR_to_LVFR(SPWindRule wr){
    FillRule fr;
    if(wr ==  SP_WIND_RULE_EVENODD){ fr = fill_oddEven; }
    else {                           fr = fill_nonZero; }
    return fr;
}


unsigned int PrintWmf::fill(
    Inkscape::Extension::Print * /*mod*/,
    Geom::PathVector const &pathv, Geom::Affine const & /*transform*/, SPStyle const *style,
    Geom::OptRect const &/*pbox*/, Geom::OptRect const &/*dbox*/, Geom::OptRect const &/*bbox*/)
{
    using Geom::X;
    using Geom::Y;

    Geom::Affine tf = m_tr_stack.top();

    use_fill   = true;
    use_stroke = false;

    fill_transform = tf;

    if (create_brush(style, NULL)){ 
        /*
            Handle gradients.  Uses modified livarot as 2geom boolops is currently broken.
            Can handle gradients with multiple stops.

            The overlap is needed to avoid antialiasing artifacts when edges are not strictly aligned on pixel boundaries.
            There is an inevitable loss of accuracy saving through an WMF file because of the integer coordinate system.
            Keep the overlap quite large so that loss of accuracy does not remove an overlap.
        */
        destroy_pen();  //this sets the NULL_PEN, otherwise gradient slices may display with boundaries, see longer explanation below
        Geom::Path cutter;
        float      rgb[3];
        U_COLORREF wc,c1,c2;
        FillRule   frb = SPWR_to_LVFR( (SPWindRule) style->fill_rule.computed);
        double     doff,doff_base,doff_range;
        double     divisions= 128.0;
        int        nstops;
        int        istop =     1;
        float      opa;                     // opacity at stop

        SPRadialGradient *tg = (SPRadialGradient *) (gv.grad);  // linear/radial are the same here
        nstops = tg->vector.stops.size();
        sp_color_get_rgb_floatv(&tg->vector.stops[0].color, rgb);
        opa    = tg->vector.stops[0].opacity;
        c1     = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );
        sp_color_get_rgb_floatv(&tg->vector.stops[nstops-1].color, rgb);
        opa    = tg->vector.stops[nstops-1].opacity;
        c2     = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );

        doff       = 0.0;
        doff_base  = 0.0;
        doff_range = tg->vector.stops[1].offset;              // next or last stop

        if(gv.mode==DRAW_RADIAL_GRADIENT){
            Geom::Point xv = gv.p2 - gv.p1;           // X'  vector
            Geom::Point yv = gv.p3 - gv.p1;           // Y'  vector
            Geom::Point xuv = Geom::unit_vector(xv);  // X' unit vector
            double rx = hypot(xv[X],xv[Y]);
            double ry = hypot(yv[X],yv[Y]);
            double range    = fmax(rx,ry);            // length along the gradient
            double step     = range/divisions;        // adequate approximation for gradient
            double overlap  = step/4.0;               // overlap slices slightly
            double start;
            double stop;
            Geom::PathVector pathvc, pathvr;

            /*  radial gradient might stop part way through the shape, fill with outer color from there to "infinity".
                Do this first so that outer colored ring will overlay it.
            */
            pathvc = center_elliptical_hole_as_SVG_PathV(gv.p1, rx*(1.0 - overlap/range), ry*(1.0 - overlap/range), asin(xuv[Y]));
            pathvr = sp_pathvector_boolop(pathvc, pathv, bool_op_inters, (FillRule) fill_oddEven, frb);
            wc = weight_opacity(c2); 
            (void) create_brush(style, &wc);
            print_pathv(pathvr, fill_transform);

            sp_color_get_rgb_floatv(&tg->vector.stops[istop].color, rgb);
            opa = tg->vector.stops[istop].opacity;
            c2 = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );

            for(start = 0.0; start < range; start += step, doff += 1./divisions){
                stop = start + step + overlap;
                if(stop > range)stop=range;
                wc = weight_colors(c1, c2, (doff - doff_base)/(doff_range-doff_base) );
                (void) create_brush(style, &wc);

                pathvc = center_elliptical_ring_as_SVG_PathV(gv.p1, rx*start/range, ry*start/range, rx*stop/range, ry*stop/range, asin(xuv[Y]));

                pathvr = sp_pathvector_boolop(pathvc, pathv, bool_op_inters, (FillRule) fill_nonZero, frb);
                print_pathv(pathvr, fill_transform);  // show the intersection

                if(doff >= doff_range - doff_base){
                    istop++;
                    if(istop >= nstops)continue; // could happen on a rounding error
                    doff_base  = doff_range;
                    doff_range = tg->vector.stops[istop].offset;  // next or last stop
                    c1=c2;
                    sp_color_get_rgb_floatv(&tg->vector.stops[istop].color, rgb);
                    opa = tg->vector.stops[istop].opacity;
                    c2 = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );
                }
            }
        }
        else if(gv.mode == DRAW_LINEAR_GRADIENT){
            Geom::Point uv  = Geom::unit_vector(gv.p2 - gv.p1);  // unit vector
            Geom::Point puv = uv.cw();                           // perp. to unit vector
            double range    = Geom::distance(gv.p1,gv.p2);       // length along the gradient
            double step     = range/divisions;                   // adequate approximation for gradient
            double overlap  = step/4.0;                          // overlap slices slightly
            double start;
            double stop;
            Geom::PathVector pathvc, pathvr;

            /* before lower end of gradient, overlap first slice position */
            wc = weight_opacity(c1); 
            (void) create_brush(style, &wc);
            pathvc = rect_cutter(gv.p1, uv*(overlap), uv*(-50000.0), puv*50000.0);
            pathvr = sp_pathvector_boolop(pathvc, pathv, bool_op_inters, (FillRule) fill_nonZero, frb);
            print_pathv(pathvr, fill_transform);

            /* after high end of gradient, overlap last slice poosition */
            wc = weight_opacity(c2); 
            (void) create_brush(style, &wc);
            pathvc = rect_cutter(gv.p2, uv*(-overlap), uv*(50000.0), puv*50000.0);
            pathvr = sp_pathvector_boolop(pathvc, pathv, bool_op_inters, (FillRule) fill_nonZero, frb);
            print_pathv(pathvr, fill_transform);

            sp_color_get_rgb_floatv(&tg->vector.stops[istop].color, rgb);
            opa = tg->vector.stops[istop].opacity;
            c2 = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );

            for(start = 0.0; start < range; start += step, doff += 1./divisions){
                stop = start + step + overlap;
                if(stop > range)stop=range;
                pathvc = rect_cutter(gv.p1, uv*start, uv*stop, puv*50000.0);

                wc = weight_colors(c1, c2, (doff - doff_base)/(doff_range-doff_base) );
                (void) create_brush(style, &wc);
                Geom::PathVector pathvr = sp_pathvector_boolop(pathvc, pathv, bool_op_inters, (FillRule) fill_nonZero, frb);
                print_pathv(pathvr, fill_transform);  // show the intersection

                if(doff >= doff_range - doff_base){
                    istop++;
                    if(istop >= nstops)continue; // could happen on a rounding error
                    doff_base  = doff_range;
                    doff_range = tg->vector.stops[istop].offset;  // next or last stop
                    c1=c2;
                    sp_color_get_rgb_floatv(&tg->vector.stops[istop].color, rgb);
                    opa = tg->vector.stops[istop].opacity;
                    c2 = U_RGBA( 255*rgb[0], 255*rgb[1], 255*rgb[2], 255*opa );
                }
            }
        }
        else {
            g_error("Fatal programming error in PrintWmf::fill, invalid gradient type detected");
        }
        use_fill = false;  // gradients handled, be sure stroke does not use stroke and fill
    }
    else {
        /*
            Inkscape was not calling create_pen for objects with no border. 
            This was because it never called stroke() (next method).
            PPT, and presumably others, pick whatever they want for the border if it is not specified, so no border can
            become a visible border.
            To avoid this force the pen to NULL_PEN if we can determine that no pen will be needed after the fill.
        */
        if (style->stroke.noneSet || style->stroke_width.computed == 0.0){
            destroy_pen();  //this sets the NULL_PEN
        }

        /*  postpone fill in case stroke also required AND all stroke paths closed
            Dashes converted to line segments will "open" a closed path.
        */
        bool all_closed = true;
        for (Geom::PathVector::const_iterator pit = pathv.begin(); pit != pathv.end(); ++pit){
            for (Geom::Path::const_iterator cit = pit->begin(); cit != pit->end_open(); ++cit){
                if (pit->end_default() != pit->end_closed()) { all_closed=false; }
            }
        }
        if (
            (style->stroke.isNone() || style->stroke.noneSet || style->stroke_width.computed == 0.0) ||
            (style->stroke_dash.n_dash   &&  style->stroke_dash.dash  && FixPPTDashLine)             ||
            !all_closed
        ){
            print_pathv(pathv, fill_transform);  // do any fills. side effect: clears fill_pathv
            use_fill = false;
        }
    }

    return 0;
}


unsigned int PrintWmf::stroke (
    Inkscape::Extension::Print * /*mod*/,
    Geom::PathVector const &pathv, const Geom::Affine &/*transform*/, const SPStyle *style,
    Geom::OptRect const &/*pbox*/, Geom::OptRect const &/*dbox*/, Geom::OptRect const &/*bbox*/)
{
    
    char *rec = NULL;
    Geom::Affine tf = m_tr_stack.top();

    use_stroke = true;
    //  use_fill was set in ::fill, if it is needed, if not, the null brush is used, it should be already set

    if (create_pen(style, tf))return 0;
    
    if (style->stroke_dash.n_dash   &&  style->stroke_dash.dash  && FixPPTDashLine  ){
        // convert the path, gets its complete length, and then make a new path with parameter length instead of t
        Geom::Piecewise<Geom::D2<Geom::SBasis> > tmp_pathpw;  // pathv-> sbasis
        Geom::Piecewise<Geom::D2<Geom::SBasis> > tmp_pathpw2; // sbasis using arc length parameter
        Geom::Piecewise<Geom::D2<Geom::SBasis> > tmp_pathpw3; // new (discontinuous) path, composed of dots/dashes
        Geom::Piecewise<Geom::D2<Geom::SBasis> > first_frag;  // first fragment, will be appended at end
        int n_dash = style->stroke_dash.n_dash;
        int i=0; //dash index
        double tlength;                                       // length of tmp_pathpw
        double slength=0.0;                                   // start of gragment
        double elength;                                       // end of gragment
        for (unsigned int i=0; i < pathv.size(); i++) {
            tmp_pathpw.concat(pathv[i].toPwSb());
        }
        tlength = length(tmp_pathpw,0.1);
        tmp_pathpw2 = arc_length_parametrization(tmp_pathpw);

        // go around the dash array repeatedly until the entire path is consumed (but not beyond).
        while(slength < tlength){
            elength = slength + style->stroke_dash.dash[i++];
            if(elength > tlength)elength = tlength;
            Geom::Piecewise<Geom::D2<Geom::SBasis> > fragment(portion(tmp_pathpw2, slength, elength));
            if(slength){  tmp_pathpw3.concat(fragment); }
            else {        first_frag = fragment;        }
            slength = elength;
            slength += style->stroke_dash.dash[i++];  // the gap
            if(i>=n_dash)i=0;
        }
        tmp_pathpw3.concat(first_frag); // may merge line around start point
        Geom::PathVector out_pathv = Geom::path_from_piecewise(tmp_pathpw3, 0.01); 
        print_pathv(out_pathv, tf);
    }
    else {
        print_pathv(pathv, tf);
    }

    use_stroke = false;
    use_fill   = false;

    if(usebk){  // OPAQUE was set, revert to TRANSPARENT
        usebk = false;
        rec = U_WMRSETBKMODE_set(U_TRANSPARENT);
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::stroke at U_WMRSETBKMODE_set");
        }
    }

    return 0;
}


// Draws simple_shapes, those with closed WMR_* primitives, like polygons, rectangles and ellipses.
// These use whatever the current pen/brush are and need not be followed by a FILLPATH or STROKEPATH.
// For other paths it sets a few flags and returns.
bool PrintWmf::print_simple_shape(Geom::PathVector const &pathv, const Geom::Affine &transform)
{

    Geom::PathVector pv = pathv_to_linear( pathv * transform, MAXDISP );
    
    int nodes  = 0;
    int moves  = 0;
    int lines  = 0;
    int curves = 0;
    char *rec  = NULL;

    for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
    {
        moves++;
        nodes++;
        
        for (Geom::Path::const_iterator cit = pit->begin(); cit != pit->end_open(); ++cit)
        {
            nodes++;
            
            if ( is_straight_curve(*cit) ) { lines++;  }
            else if (&*cit) {                curves++; }
        }
    }

    if (!nodes)
        return false;
    
    U_POINT16 *lpPoints = new U_POINT16[moves + lines + curves*3];
    int i = 0;

    /**  For all Subpaths in the <path> */

    for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
    {
        using Geom::X;
        using Geom::Y;

        Geom::Point p0 = pit->initialPoint();

        p0[X] = (p0[X] * PX2WORLD);
        p0[Y] = (p0[Y] * PX2WORLD);
        
        int32_t const x0 = (int32_t) round(p0[X]);
        int32_t const y0 = (int32_t) round(p0[Y]);

        lpPoints[i].x = x0;
        lpPoints[i].y = y0;
        i = i + 1;

        /**  For all segments in the subpath */

        for (Geom::Path::const_iterator cit = pit->begin(); cit != pit->end_open(); ++cit)
        {
            if ( is_straight_curve(*cit) )
            {
                //Geom::Point p0 = cit->initialPoint();
                Geom::Point p1 = cit->finalPoint();

                //p0[X] = (p0[X] * PX2WORLD);
                p1[X] = (p1[X] * PX2WORLD);
                //p0[Y] = (p0[Y] * PX2WORLD);
                p1[Y] = (p1[Y] * PX2WORLD);

                //int32_t const x0 = (int32_t) round(p0[X]);
                //int32_t const y0 = (int32_t) round(p0[Y]);
                int32_t const x1 = (int32_t) round(p1[X]);
                int32_t const y1 = (int32_t) round(p1[Y]);

                lpPoints[i].x = x1;
                lpPoints[i].y = y1;
                i = i + 1;
            }
            else if (Geom::CubicBezier const *cubic = dynamic_cast<Geom::CubicBezier const*>(&*cit))
            {
                std::vector<Geom::Point> points = cubic->points();
                //Geom::Point p0 = points[0];
                Geom::Point p1 = points[1];
                Geom::Point p2 = points[2];
                Geom::Point p3 = points[3];

                //p0[X] = (p0[X] * PX2WORLD);
                p1[X] = (p1[X] * PX2WORLD);
                p2[X] = (p2[X] * PX2WORLD);
                p3[X] = (p3[X] * PX2WORLD);
                //p0[Y] = (p0[Y] * PX2WORLD);
                p1[Y] = (p1[Y] * PX2WORLD);
                p2[Y] = (p2[Y] * PX2WORLD);
                p3[Y] = (p3[Y] * PX2WORLD);
                
                //int32_t const x0 = (int32_t) round(p0[X]);
                //int32_t const y0 = (int32_t) round(p0[Y]);
                int32_t const x1 = (int32_t) round(p1[X]);
                int32_t const y1 = (int32_t) round(p1[Y]);
                int32_t const x2 = (int32_t) round(p2[X]);
                int32_t const y2 = (int32_t) round(p2[Y]);
                int32_t const x3 = (int32_t) round(p3[X]);
                int32_t const y3 = (int32_t) round(p3[Y]);

                lpPoints[i].x = x1;
                lpPoints[i].y = y1;
                lpPoints[i+1].x = x2;
                lpPoints[i+1].y = y2;
                lpPoints[i+2].x = x3;
                lpPoints[i+2].y = y3;
                i = i + 3;
            }
        }
    }

    bool done = false;
    bool closed = (lpPoints[0].x == lpPoints[i-1].x) && (lpPoints[0].y == lpPoints[i-1].y);
    bool polygon = false;
    bool rectangle = false;
    bool ellipse = false;
    
    if (moves == 1 && moves+lines == nodes && closed) {
        polygon = true;
//        if (nodes==5) {                             // disable due to LP Bug 407394
//            if (lpPoints[0].x == lpPoints[3].x && lpPoints[1].x == lpPoints[2].x &&
//                lpPoints[0].y == lpPoints[1].y && lpPoints[2].y == lpPoints[3].y)
//            {
//                rectangle = true;
//            }
//        }
    }
    else if (moves == 1 && nodes == 5 && moves+curves == nodes && closed) {
//        if (lpPoints[0].x == lpPoints[1].x && lpPoints[1].x == lpPoints[11].x &&
//            lpPoints[5].x == lpPoints[6].x && lpPoints[6].x == lpPoints[7].x &&
//            lpPoints[2].x == lpPoints[10].x && lpPoints[3].x == lpPoints[9].x && lpPoints[4].x == lpPoints[8].x &&
//            lpPoints[2].y == lpPoints[3].y && lpPoints[3].y == lpPoints[4].y &&
//            lpPoints[8].y == lpPoints[9].y && lpPoints[9].y == lpPoints[10].y &&
//            lpPoints[5].y == lpPoints[1].y && lpPoints[6].y == lpPoints[0].y && lpPoints[7].y == lpPoints[11].y)
//        {                                           // disable due to LP Bug 407394
//            ellipse = true;
//        }
    }

    if (polygon || ellipse) {
        // pens and brushes already set by caller, do not touch them

        if (polygon) {
            if (rectangle){
                U_RECT16 rcl = U_RECT16_set((U_POINT16) {lpPoints[0].x, lpPoints[0].y}, (U_POINT16) {lpPoints[2].x, lpPoints[2].y});
                rec = U_WMRRECTANGLE_set(rcl);
            }
            else {
                rec = U_WMRPOLYGON_set(nodes, lpPoints);
            }
        }
        else if (ellipse) {
            U_RECT16 rcl = U_RECT16_set((U_POINT16) {lpPoints[6].x, lpPoints[3].y}, (U_POINT16) {lpPoints[0].x, lpPoints[9].y});
            rec = U_WMRELLIPSE_set(rcl);
        }
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::print_simple_shape at retangle/ellipse/polygon");
        }
        
        done = true;

    }

    delete[] lpPoints;
    
    return done;
}

/** Some parts based on win32.cpp by Lauris Kaplinski <lauris@kaplinski.com>.  Was a part of Inkscape
    in the past (or will be in the future?)  Not in current trunk. (4/19/2012)
   
    Limitations of this code:
    1.  Images lose their rotation, one corner stays in the same place.
    2.  Transparency is lost on export.  (A limitation of the WMF format.)
    3.  Probably messes up if row stride != w*4
    4.  There is still a small memory leak somewhere, possibly in a pixbuf created in a routine
        that calls this one and passes px, but never removes the rest of the pixbuf.  The first time
        this is called it leaked 5M (in one test) and each subsequent call leaked around 200K more.
        If this routine is reduced to 
            if(1)return(0);
        and called for a single 1280 x 1024 image then the program leaks 11M per call, or roughly the
        size of two bitmaps.
*/

unsigned int PrintWmf::image(
    Inkscape::Extension::Print * /* module */,  /** not used */
    unsigned char *rgba_px,   /** array of pixel values, Gdk::Pixbuf bitmap format */
    unsigned int w,      /** width of bitmap */
    unsigned int h,      /** height of bitmap */
    unsigned int rs,     /** row stride (normally w*4) */
    Geom::Affine const &tf_ignore,  /** WRONG affine transform, use the one from m_tr_stack */
    SPStyle const *style)  /** provides indirect link to image object */
{
    double x1,y1,dw,dh;
    char *rec = NULL;
    Geom::Affine tf = m_tr_stack.top();

    rec = U_WMRSETSTRETCHBLTMODE_set(U_COLORONCOLOR);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::image at EMRHEADER");
    }

    x1= atof(style->object->getAttribute("x"));
    y1= atof(style->object->getAttribute("y"));
    dw= atof(style->object->getAttribute("width"));
    dh= atof(style->object->getAttribute("height"));
    Geom::Point pLL(x1,y1);
    Geom::Point pLL2 = pLL * tf;  //location of LL corner in Inkscape coordinates

    char                *px;
    uint32_t             cbPx;
    uint32_t             colortype;
    PU_RGBQUAD           ct;
    int                  numCt;
    U_BITMAPINFOHEADER   Bmih;
    PU_BITMAPINFO        Bmi;
    colortype = U_BCBM_COLOR32;
    (void) RGBA_to_DIB(&px, &cbPx, &ct, &numCt,  (char *) rgba_px,  w, h, w*4, colortype, 0, 1);
    Bmih = bitmapinfoheader_set(w, h, 1, colortype, U_BI_RGB, 0, PXPERMETER, PXPERMETER, numCt, 0);
    Bmi = bitmapinfo_set(Bmih, ct);

    U_POINT16 Dest  = point16_set(round(pLL2[Geom::X] * PX2WORLD), round(pLL2[Geom::Y] * PX2WORLD));
    U_POINT16 cDest = point16_set(round(dw * PX2WORLD), round(dh * PX2WORLD));
    U_POINT16 Src   = point16_set(0,0);
    U_POINT16 cSrc  = point16_set(w,h);
    rec = U_WMRSTRETCHDIB_set(
            Dest,                //! Destination UL corner in logical units
            cDest,               //! Destination W & H in logical units
            Src,                 //! Source UL corner in logical units
            cSrc,                //! Source W & H in logical units
            U_DIB_RGB_COLORS,    //! DIBColors Enumeration
            U_SRCCOPY,           //! RasterOPeration Enumeration
            Bmi,                 //! (Optional) bitmapbuffer (U_BITMAPINFO section)
            h*rs,                //! size in bytes of px          
            px                   //! (Optional) bitmapbuffer (U_BITMAPINFO section)
    );
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::image at U_WMRSTRETCHDIB_set");
    }
    free(px);
    free(Bmi);
    if(numCt)free(ct);
    return 0;
}

// may also be called with a simple_shape or an empty path, whereupon it just returns without doing anything
unsigned int PrintWmf::print_pathv(Geom::PathVector const &pathv, const Geom::Affine &transform)
{
    char       *rec = NULL;
    PU_POINT16  pt16hold, pt16ptr;
    uint16_t   *n16hold;
    uint16_t   *n16ptr;

    simple_shape = print_simple_shape(pathv, transform);
    if (!simple_shape && !pathv.empty()){
        // WMF does not have beziers, need to convert to ONLY  linears with something like this:
        Geom::PathVector pv = pathv_to_linear( pathv * transform, MAXDISP );

        /**  For all Subpaths in the <path> */

        /* If the path consists entirely of closed subpaths use one polypolygon.
            Otherwise use a mix of  polygon or polyline separately on each path.
            If the polyline turns out to be single line segments, use a series of MOVETO/LINETO instead,
            because WMF has no POLYPOLYLINE.
            The former allows path delimited donuts and the like, which
            cannot be represented in WMF with polygon or polyline because there is no external way to combine paths
            as there is in EMF or SVG.
            For polygons specify the last point the same as the first.  The WMF/EMF manuals say that the 
            reading program SHOULD close the path, which allows a conforming program not to, potentially rendering
            a closed path as an open one. */ 
        int nPolys=0;
        int totPoints = 0;
        for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
        {
            totPoints += 1 + pit->size_default();  // big array, will hold all points, for all polygons.  Size_default ignores first point in each path.
            if (pit->end_default() == pit->end_closed()) {  nPolys++;        }
            else {                                          nPolys=0; break; }
        }

        if(nPolys > 1){ // a single polypolygon, a single polygon falls through to the else
            pt16hold = pt16ptr = (PU_POINT16) malloc(totPoints * sizeof(U_POINT16));
            if(!pt16ptr)return(false);

            n16hold = n16ptr = (uint16_t *) malloc(nPolys * sizeof(uint16_t));
            if(!n16ptr){ free(pt16hold); return(false); }

            for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
            {
                using Geom::X;
                using Geom::Y;


                *n16ptr++ = pit->size_default();  // points in the subpath

                /**  For each segment in the subpath */

                Geom::Point p1 = pit->initialPoint(); // This point is special, it isn't in the interator

                p1[X] = (p1[X] * PX2WORLD);
                p1[Y] = (p1[Y] * PX2WORLD);              
                *pt16ptr++ = point16_set((int32_t) round(p1[X]), (int32_t) round(p1[Y]));

                for (Geom::Path::const_iterator cit = pit->begin(); cit != pit->end_open(); ++cit)
                {
                    Geom::Point p1 = cit->finalPoint();

                    p1[X] = (p1[X] * PX2WORLD);
                    p1[Y] = (p1[Y] * PX2WORLD);              
                    *pt16ptr++ = point16_set((int32_t) round(p1[X]), (int32_t) round(p1[Y]));
                }

            }
            rec = U_WMRPOLYPOLYGON_set(nPolys, n16hold,pt16hold);
            if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                g_error("Fatal programming error in PrintWmf::print_pathv at U_WMRPOLYPOLYGON_set");
            }
            free(pt16hold);
            free(n16hold);
        }
        else { // one or more polyline or polygons (but not all polygons, that would be the preceding case)
            for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
            {
                using Geom::X;
                using Geom::Y;
                
                /* Malformatted Polylines with a sequence like M L M M L have been seen, the 2nd M does nothing 
                   and that point must not go into the output. */
                if(!(pit->size_default())){ continue; }
                /*  Figure out how many points there are, make an array big enough to hold them, and store
                    all the points.  This is the same for open or closed path.  This gives the upper bound for
                    the number of points.  The actual number used is calculated on the fly.
                */
                int nPoints = 1 + pit->size_default();
                
                pt16hold = pt16ptr = (PU_POINT16) malloc(nPoints * sizeof(U_POINT16));
                if(!pt16ptr)break;

                /**  For each segment in the subpath */

                Geom::Point p1 = pit->initialPoint(); // This point is special, it isn't in the interator

                p1[X] = (p1[X] * PX2WORLD);
                p1[Y] = (p1[Y] * PX2WORLD);              
                *pt16ptr++ = point16_set((int32_t) round(p1[X]), (int32_t) round(p1[Y]));
                nPoints = 1;

                for (Geom::Path::const_iterator cit = pit->begin(); cit != pit->end_default(); ++cit, nPoints++)
                {
                    Geom::Point p1 = cit->finalPoint();

                    p1[X] = (p1[X] * PX2WORLD);
                    p1[Y] = (p1[Y] * PX2WORLD);              
                    *pt16ptr++ = point16_set((int32_t) round(p1[X]), (int32_t) round(p1[Y]));
                }

                if (pit->end_default() == pit->end_closed()) {
                    rec = U_WMRPOLYGON_set(nPoints,  pt16hold); 
                    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                        g_error("Fatal programming error in PrintWmf::print_pathv at U_WMRPOLYGON_set");
                    }
                }
                else if(nPoints>2) {
                    rec = U_WMRPOLYLINE_set(nPoints, pt16hold);
                    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                        g_error("Fatal programming error in PrintWmf::print_pathv at U_POLYLINE_set");
                    }
                }
                else if(nPoints == 2) {
                    rec = U_WMRMOVETO_set(pt16hold[0]);
                    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                        g_error("Fatal programming error in PrintWmf::print_pathv at U_WMRMOVETO_set");
                    }
                    rec = U_WMRLINETO_set(pt16hold[1]);
                    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
                        g_error("Fatal programming error in PrintWmf::print_pathv at U_WMRLINETO_set");
                    }
                }
                free(pt16hold);
            }
        }
    }

    // WMF has no fill or stroke commands, the draw does it with active pen/brush

    // clean out brush and pen, but only after all parts of the draw complete
    if (use_fill){    destroy_brush(); }
    if (use_stroke){  destroy_pen();   }

    return TRUE;
}


bool PrintWmf::textToPath(Inkscape::Extension::Print * ext)
{
    return ext->get_param_bool("textToPath");
}

unsigned int PrintWmf::text(Inkscape::Extension::Print * /*mod*/, char const *text, Geom::Point const &p,
                    SPStyle const *const style)
{
    if (!wt) return 0;

    char *rec = NULL;
    int ccount,newfont;
    int fix90n=0;
    uint32_t hfont = 0;
    Geom::Affine tf = m_tr_stack.top();
    double rot = -1800.0*std::atan2(tf[1], tf[0])/M_PI;    // 0.1 degree rotation,  - sign for MM_TEXT
    double rotb = -std::atan2(tf[1], tf[0]);  // rotation for baseline offset for superscript/subscript, used below
    double dx,dy;
    double f1,f2,f3;
    double ky;

    // the dx array is smuggled in like: text<nul>w1 w2 w3 ...wn<nul><nul>, where the widths are floats 7 characters wide, including the space
    int ndx, rtl;
    int16_t *adx;
    smuggle_adxky_out(text, &adx, &ky, &rtl, &ndx, PX2WORLD * std::min(tf.expansionX(),tf.expansionY())); // side effect: free() adx
    
    uint32_t textalignment;
    if(rtl > 0){ textalignment = U_TA_BASELINE | U_TA_LEFT;                    }
    else {       textalignment = U_TA_BASELINE | U_TA_RIGHT | U_TA_RTLREADING; }
    if(textalignment != htextalignment){
        htextalignment = textalignment;
        rec = U_WMRSETTEXTALIGN_set(textalignment);
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::text at U_WMRSETTEXTALIGN_set");
        }
    }

    char *text2 = strdup(text);  // because U_Utf8ToUtf16le calls iconv which does not like a const char *
    uint16_t *unicode_text = U_Utf8ToUtf16le( text2, 0, NULL );
    free(text2);
    //translates Unicode  as Utf16le to NonUnicode, if possible.  If any translate, all will, and all to
    //the same font, because of code in Layout::print
    UnicodeToNon(unicode_text, &ccount, &newfont);
    // The preceding hopefully handled conversions to symbol, wingdings or zapf dingbats.  Now slam everything
    // else down into latin1, which is all WMF can handle.  If the language isn't English expect terrible results.
    char *latin1_text = U_Utf16leToLatin1( unicode_text, 0, NULL );
    free(unicode_text);

    //PPT gets funky with text within +-1 degree of a multiple of 90, but only for SOME fonts.Snap those to the central value
    //Some funky ones:  Arial, Times New Roman
    //Some not funky ones: Symbol and Verdana.
    //Without a huge table we cannot catch them all, so just the most common problem ones.
    if(FixPPTCharPos){
        switch(newfont){
            case CVTSYM:
                search_short_fflist("Convert To Symbol", &f1, &f2, &f3);
                break;
            case CVTZDG:
                search_short_fflist("Convert To Zapf Dingbats", &f1, &f2, &f3);
                break;
            case CVTWDG:
                search_short_fflist("Convert To Wingdings", &f1, &f2, &f3);
                break;
            default:  //also CVTNON
                search_short_fflist(style->text->font_family.value, &f1, &f2, &f3);
                break;
        }
        if(f2 || f3){
            int irem = ((int) round(rot)) % 900 ;
            if(irem <=9 && irem >= -9){
                fix90n=1; //assume vertical
                rot  = (double) (((int) round(rot)) - irem);
                rotb =  rot*M_PI/1800.0;
                if( abs(rot) == 900.0 ){ fix90n = 2; }
            }
        }
    }

    /* 
        Note that text font sizes are stored into the WMF as fairly small integers and that limits their precision.  
        The WMF output files produced here have been designed so that the integer valued pt sizes
        land right on an integer value in the WMF file, so those are exact.  However, something like 18.1 pt will be
        somewhat off, so that when it is read back in it becomes 18.11 pt.  (For instance.)   
    */
    int textheight = round(-style->font_size.computed * PX2WORLD * std::min(tf.expansionX(),tf.expansionY()));
    if (!hfont) {

        // Get font face name.  Use changed font name if unicode mapped to one
        // of the special fonts.
        char *facename;
        if(!newfont){ facename = U_Utf8ToLatin1(style->text->font_family.value, 0, NULL); }
        else {        facename = U_Utf8ToLatin1(FontName(newfont), 0, NULL);              }

        // Scale the text to the minimum stretch. (It tends to stay within bounding rectangles even if
        // it was streteched asymmetrically.)  Few applications support text from WMF which is scaled
        // differently by height/width, so leave lfWidth alone.  

        PU_FONT puf = U_FONT_set(
            textheight, 
            0,        
            round(rot),
            round(rot),
            transweight(style->font_weight.computed),
            (style->font_style.computed == SP_CSS_FONT_STYLE_ITALIC),
            style->text_decoration_line.underline,
            style->text_decoration_line.line_through,
            U_DEFAULT_CHARSET,
            U_OUT_DEFAULT_PRECIS,
            U_CLIP_DEFAULT_PRECIS,
            U_DEFAULT_QUALITY,
            U_DEFAULT_PITCH | U_FF_DONTCARE,
            facename);
	free(facename);
       
        rec  = wcreatefontindirect_set( &hfont, wht, puf); 
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::text at wcreatefontindirect_set");
        }
        free(puf);
    }
    
    rec = wselectobject_set(hfont, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::text at wselectobject_set");
    }

    float rgb[3];
    sp_color_get_rgb_floatv( &style->fill.value.color, rgb );
    // only change the text color when it needs to be changed 
    if(memcmp(htextcolor_rgb,rgb,3*sizeof(float))){
        memcpy(htextcolor_rgb,rgb,3*sizeof(float));
        rec = U_WMRSETTEXTCOLOR_set(U_RGB(255*rgb[0], 255*rgb[1], 255*rgb[2]));
        if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
            g_error("Fatal programming error in PrintWmf::text at U_WMRSETTEXTCOLOR_set");
        }
    }

 
    // Text alignment:
    //   - (x,y) coordinates received by this filter are those of the point where the text
    //     actually starts, and already takes into account the text object's alignment;
    //   - for this reason, the WMF text alignment must always be TA_BASELINE|TA_LEFT.
    //     this is set at the beginning of the file and never changed

    // Transparent text background, never changes, set at the beginning of the file

    Geom::Point p2 = p * tf;

    //Handle super/subscripts and vertical kerning
/*  Previously used this, but vertical kerning was not supported
    p2[Geom::X] -= style->baseline_shift.computed * std::sin( rotb );
    p2[Geom::Y] -= style->baseline_shift.computed * std::cos( rotb );
*/
    p2[Geom::X] += ky * std::sin( rotb );
    p2[Geom::Y] += ky * std::cos( rotb );

    //Conditionally handle compensation for PPT WMF import bug (affects PPT 2003-2010, at least)
    if(FixPPTCharPos){
        if(fix90n==1){ //vertical
            dx= 0.0;
            dy= f3 * style->font_size.computed * std::cos( rotb );
        }
        else if(fix90n==2){ //horizontal
            dx= f2 * style->font_size.computed * std::sin( rotb );
            dy= 0.0;
        }
        else {
            dx= f1 * style->font_size.computed * std::sin( rotb );
            dy= f1 * style->font_size.computed * std::cos( rotb );
        }
        p2[Geom::X] += dx;
        p2[Geom::Y] += dy;
    }

    p2[Geom::X] = (p2[Geom::X] * PX2WORLD);
    p2[Geom::Y] = (p2[Geom::Y] * PX2WORLD);

    int32_t const xpos = (int32_t) round(p2[Geom::X]);
    int32_t const ypos = (int32_t) round(p2[Geom::Y]);

    // The number of characters in the string is a bit fuzzy.  ndx, the number of entries in adx is 
    // the number of VISIBLE characters, since some may combine from the UTF (8 originally,
    // now 16) encoding.  Conversely strlen() or wchar16len() would give the absolute number of
    // encoding characters.  Unclear if emrtext wants the former or the latter but for now assume the former.
    
//    This is currently being smuggled in from caller as part of text, works
//    MUCH better than the fallback hack below
//    uint32_t *adx = dx_set(textheight,  U_FW_NORMAL, slen);  // dx is needed, this makes one up
    if(rtl>0){
        rec = U_WMREXTTEXTOUT_set((U_POINT16) {xpos, ypos}, ndx, U_ETO_NONE, latin1_text, adx, U_RCL16_DEF);
    }
    else {  // RTL text, U_TA_RTLREADING should be enough, but set this one too just in case
        rec = U_WMREXTTEXTOUT_set((U_POINT16) {xpos, ypos}, ndx, U_ETO_RTLREADING, latin1_text, adx, U_RCL16_DEF);
    }
    free(latin1_text);
    free(adx);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::text at U_WMREXTTEXTOUTW_set");
    }

    rec = wdeleteobject_set(&hfont, wht);
    if(!rec || wmf_append((PU_METARECORD)rec, wt, U_REC_FREE)){
        g_error("Fatal programming error in PrintWmf::text at wdeleteobject_set");
    }
    
    return 0;
}

void PrintWmf::init (void)
{
    read_system_fflist();

    /* WMF print */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
        "<name>Windows Metafile Print</name>\n"
        "<id>org.inkscape.print.wmf</id>\n"
        "<param name=\"destination\" type=\"string\"></param>\n"
        "<param name=\"textToPath\" type=\"boolean\">true</param>\n"
        "<param name=\"pageBoundingBox\" type=\"boolean\">true</param>\n"
        "<param name=\"FixPPTCharPos\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTDashLine\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTGrad2Polys\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTPatternAsHatch\" type=\"boolean\">false</param>\n"
        "<print/>\n"
        "</inkscape-extension>", new PrintWmf());

    return;
}

}  /* namespace Internal */
}  /* namespace Extension */
}  /* namespace Inkscape */


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
