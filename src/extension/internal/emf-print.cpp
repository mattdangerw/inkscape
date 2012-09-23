/** @file
 * @brief Enhanced Metafile printing
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

#include "emf-print.h"

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

#include <string.h>
extern "C" {
#include "libunicode-convert/unicode-convert.h"
}


namespace Inkscape {
namespace Extension {
namespace Internal {

#define PXPERMETER 2835


enum drawmode {DRAW_PAINT, DRAW_PATTERN, DRAW_IMAGE, DRAW_LINEAR_GRADIENT, DRAW_RADIAL_GRADIENT};

struct FFNEXUS {
   char    *fontname; //Font name
   FFNEXUS *next;     //link to next nexus, NULL if this is the last
   double   f1;       //Vertical (rotating) offset factor (* font height)
   double   f2;       //Vertical (nonrotating) offset factor (* font height)
   double   f3;       //Horizontal (nonrotating) offset factor (* font height)
  };

struct GRADVALUES{
   Geom::Point p1;      // center   or start
   Geom::Point p2;      // xhandle  or end
   Geom::Point p3;      // yhandle  or unused
   double       r;      // radius   or unused
   void        *grad;   // to access the stops information
   int          mode;   // DRAW_LINEAR_GRADIENT or DRAW_RADIAL_GRADIENT, if GRADVALUES is valid, else any value
   U_COLORREF   bgc;    // document background color, this is as good a place as any to keep it
   float        rgb[3]; // also background color, but as 0-1 float.
  };

/* globals */
static double PX2WORLD = 20.0f;
static U_XFORM     worldTransform;
static bool FixPPTCharPos, FixPPTDashLine, FixPPTGrad2Polys, FixPPTPatternAsHatch;
static FFNEXUS    *short_fflist = NULL;  //only those fonts so far encountered
static FFNEXUS    *long_fflist  = NULL;   //all the fonts described in ...\share\extensions\fontfix.conf
static EMFTRACK   *et           = NULL;
static EMFHANDLES *eht          = NULL;
static GRADVALUES  gv;

void read_system_fflist(void){  //this is not called by any other source files
FFNEXUS *temp=NULL;
FFNEXUS *ptr=NULL;
std::fstream fffile;
std::string instr;
char fontname[128];
double f1,f2,f3;
std::string path_to_ffconf;

  if(long_fflist)return;
  path_to_ffconf=INKSCAPE_EXTENSIONDIR;
#ifdef WIN32
  path_to_ffconf.append("\\fontfix.conf"); //Windows path syntax
#else
  path_to_ffconf.append("/fontfix.conf"); //Unix/linx path syntax
#endif
  //open the input
  fffile.open(path_to_ffconf.c_str(), std::ios::in);
  if(!fffile.is_open()){
    g_message("Unable to open file: %s\n", path_to_ffconf.c_str());
    throw "boom";
  }
  while (std::getline(fffile,instr)){
    if(instr[0]=='#')continue;
    // not a comment, get the 4 values from the line
    int elements=sscanf(instr.c_str(),"%lf %lf %lf %[^\n]",&f1,&f2,&f3, &fontname[0]);
    if(elements!=4){
      g_message("Expected \"f1 f2 f3 Fontname\" but did not find it in file: %s\n", path_to_ffconf.c_str());
      throw "boom";
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
      long_fflist=ptr=temp;
    }
  }
  fffile.close();
}

/* Looks for the fontname in the long list.  If it does not find it, it adds the default values
to the short list with this fontname.  If it does find it, then it adds the specified values.
*/
void search_long_fflist(const char *fontname, double *f1, double *f2, double *f3){  //this is not called by any other source files
FFNEXUS *ptr=NULL;
FFNEXUS *tmp=long_fflist;
  if(!long_fflist){
      g_message("Programming error search_long_fflist called before read_system_fflist\n");
      throw "boom";
  }
  ptr=long_fflist;
  while(ptr){
    if(!strcmp(ptr->fontname,fontname)){ tmp=ptr; break; }
    ptr=ptr->next;
  }
  //tmp points at either the found name, or the default, the first entry in long_fflist
  if(!short_fflist){
    ptr=short_fflist=(FFNEXUS *) malloc(sizeof(FFNEXUS));
  }
  else {
    ptr=short_fflist;
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

/* Looks for the fontname in the short list.  If it does not find it, it looks in the long_fflist.
Either way it returns the f1, f2, f3 parameters for the font, even if these are for the default.
*/
void search_short_fflist(const char *fontname, double *f1, double *f2, double *f3){  //this is not called by any other source files
FFNEXUS *ptr=NULL;
static FFNEXUS *last=NULL;
  if(!long_fflist){
      g_message("Programming error search_short_fflist called before read_system_fflist\n");
      throw "boom";
  }
  // This speeds things up a lot - if the same font is called twice in a row, pull it out immediately
  if(last && !strcmp(last->fontname,fontname)){ ptr=last;         }
  else {                                        ptr=short_fflist; }  // short_fflist may still be NULL
  while(ptr){
    if(!strcmp(ptr->fontname,fontname)){ *f1=ptr->f1; *f2=ptr->f2; *f3=ptr->f3; last=ptr; return; }
    ptr=ptr->next;
  }
  //reach this point only if there is no match
  search_long_fflist(fontname, f1, f2, f3);
}

void smuggle_adx_out(const char *string, uint32_t **adx, int *ndx, float scale){
    float       fdx;
    int         i;
    uint32_t   *ladx;
    const char *cptr=&string[strlen(string)+1];

    *adx=NULL;
    sscanf(cptr,"%7d",ndx);
    if(!*ndx)return;  // this could happen with an empty string
    cptr += 7;
    ladx = (uint32_t *) malloc(*ndx * sizeof(uint32_t) );
    *adx=ladx;
    for(i=0; i<*ndx; i++,cptr+=7, ladx++){
      sscanf(cptr,"%7f",&fdx);
      *ladx=(uint32_t) round(fdx * scale);
    }
}

/* convert an  0RGB color to EMF U_COLORREF.
inverse of sethexcolor() in emf-inout.cpp
*/
U_COLORREF  gethexcolor(uint32_t color){

    U_COLORREF out;
    out = U_RGB( 
            (color >> 16) & 0xFF,
            (color >>  8) & 0xFF,
            (color >>  0) & 0xFF
          );
    return(out);
}


/* Translate inkscape weights to EMF weights.
*/
uint32_t transweight(const unsigned int inkweight){
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

PrintEmf::PrintEmf (void):
    _width(0),
    _height(0),
    hbrush(0),
    hbrushOld(0),
    hpen(0),
    use_stroke(false),
    use_fill(false),
    simple_shape(false)
{
}


PrintEmf::~PrintEmf (void)
{

    /* restore default signal handling for SIGPIPE */
#if !defined(_WIN32) && !defined(__WIN32__)
    (void) signal(SIGPIPE, SIG_DFL);
#endif
    return;
}


unsigned int PrintEmf::setup (Inkscape::Extension::Print * /*mod*/)
{
    return TRUE;
}


unsigned int PrintEmf::begin (Inkscape::Extension::Print *mod, SPDocument *doc)
{
    U_SIZEL szlDev, szlMm;
    U_RECTL rclBounds, rclFrame;
    char *rec;

    gchar const *utf8_fn = mod->get_param_string("destination");
    FixPPTCharPos = mod->get_param_bool("FixPPTCharPos");
    FixPPTDashLine = mod->get_param_bool("FixPPTDashLine");
    FixPPTGrad2Polys = mod->get_param_bool("FixPPTGrad2Polys");
    FixPPTPatternAsHatch = mod->get_param_bool("FixPPTPatternAsHatch");

    (void) emf_start(utf8_fn, 1000000, 250000, &et);  // Initialize the et structure
    (void) htable_create(128, 128, &eht);             // Initialize the eht structure

    char *ansi_uri = (char *) utf8_fn;

    // width and height in px
    _width  = doc->getWidth();
    _height = doc->getHeight();
    
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

    d *= Geom::Scale(IN_PER_PX);

    float dwInchesX = d.width();
    float dwInchesY = d.height();

    // dwInchesX x dwInchesY in micrometer units, dpi=90 -> 3543.3 dpm
    (void) drawing_size((int) ceil(dwInchesX*25.4), (int) ceil(dwInchesY*25.4), 3.543307, &rclBounds, &rclFrame);

    // set up the device as A4 horizontal, 47.244094 dpmm (1200 dpi)
    int MMX = 216;
    int MMY = 279;
    (void) device_size(MMX, MMY, 47.244094, &szlDev, &szlMm); // Drawing: A4 horizontal,  42744 dpm (1200 dpi)
    int PixelsX = szlDev.cx;
    int PixelsY = szlDev.cy;

    // set up the description:  (version string)0(file)00 
    char buff[1024];
    memset(buff,0, sizeof(buff));
    char *p1 = strrchr(ansi_uri, '\\');
    char *p2 = strrchr(ansi_uri, '/');
    char *p = MAX(p1, p2);
    if (p)
        p++;
    else
        p = ansi_uri;
    snprintf(buff, sizeof(buff)-1, "Inkscape %s (%s)\1%s\1", Inkscape::version_string, __DATE__,p);
    uint16_t *Description = U_Utf8ToUtf16le(buff, 0, NULL); 
    int cbDesc = 2 + wchar16len(Description);      // also count the final terminator
    (void) U_Utf16leEdit(Description, '\1', '\0'); // swap the temporary \1 characters for nulls
    
    // construct the EMRHEADER record and append it to the EMF in memory
    rec = U_EMRHEADER_set( rclBounds,  rclFrame,  NULL, cbDesc, Description, szlDev, szlMm, 0);
    free(Description);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::begin at EMRHEADER";
    }


    // Simplest mapping mode, supply all coordinates in pixels
    rec = U_EMRSETMAPMODE_set(U_MM_TEXT);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::begin at EMRSETMAPMODE";
    }
    

    //  Correct for dpi in EMF vs dpi in Inkscape (always 90?)
    //  Also correct for the scaling in PX2WORLD, which is set to 20.  Doesn't hurt for high resolution,
    //  helps prevent rounding errors for low resolution EMF.  Low resolution EMF is possible if there
    //  are no print devices and the screen resolution is low.

    worldTransform.eM11 = ((float)PixelsX * 25.4f)/((float)MMX*90.0f*PX2WORLD);
    worldTransform.eM12 = 0.0f;
    worldTransform.eM21 = 0.0f;
    worldTransform.eM22 = ((float)PixelsY * 25.4f)/((float)MMY*90.0f*PX2WORLD);
    worldTransform.eDx = 0;
    worldTransform.eDy = 0;

    rec = U_EMRMODIFYWORLDTRANSFORM_set(worldTransform, U_MWT_LEFTMULTIPLY);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::begin at EMRMODIFYWORLDTRANSFORM";
    }


    if (1) {
        snprintf(buff, sizeof(buff)-1, "Screen=%dx%dpx, %dx%dmm", PixelsX, PixelsY, MMX, MMY);
        rec = textcomment_set(buff);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
           throw "Fatal programming error in PrintEmf::begin at textcomment_set 1";
        }

        snprintf(buff, sizeof(buff)-1, "Drawing=%.1lfx%.1lfpx, %.1lfx%.1lfmm", _width, _height, dwInchesX * MM_PER_IN, dwInchesY * MM_PER_IN);
        rec = textcomment_set(buff);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
           throw "Fatal programming error in PrintEmf::begin at textcomment_set 1";
        }
    }

    return 0;
}


unsigned int PrintEmf::finish (Inkscape::Extension::Print * /*mod*/)
{
// std::cout << "finish " << std::endl;
    char *rec;
    if (!et) return 0;

    
    // earlier versions had flush of fill here, but it never executed and was removed

    rec = U_EMREOF_set(0,NULL,et);  // generate the EOF record
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::finish";
    }
    (void) emf_finish(et, eht); // Finalize and write out the EMF
    emf_free(&et);              // clean up
    htable_free(&eht);          // clean up

// std::cout << "end finish" << std::endl;
    return 0;
}


unsigned int PrintEmf::comment (Inkscape::Extension::Print * /*module*/,
                        const char * /*comment*/)
{
// std::cout << "comment " << std::endl;
    if (!et) return 0;

    // earlier versions had flush of fill here, but it never executed and was removed

// std::cout << "end comment" << std::endl;
    return 0;
}

// Extracth hatchType, hatchColor from a name like
// EMFhatch<hatchType>_<hatchColor>
// Where the first one is a number and the second a color in hex.
// hatchType and hatchColor have been set with defaults before this is called.
//
void hatch_classify(char *name, int *hatchType, U_COLORREF *hatchColor){
   int val;
   uint32_t hcolor=0;
   if(0!=strncmp(name,"EMFhatch",8)){ return; } // not anything we can parse
   name+=8; // EMFhatch already detected
   val = 0;
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
      if(1 != sscanf(name,"%X",&hcolor)){ *hatchType = -1; } // again wrong syntax, cannot classify
      *hatchColor = gethexcolor(hcolor);
   }
   if(*hatchType > U_HS_SOLIDCLR)*hatchType = U_HS_SOLIDCLR;
}

//
//  Recurse down from a brush pattern, try to figure out what it is. 
//  If an image is found set a pointer to the epixbuf, else set that to NULL
//  If a pattern is found with a name like EMFhatch3_3F7FFF return hatchType=3, hatchColor=3F7FFF (as a uint32_t),
//    otherwise hatchType is set to -1 and hatchColor is not defined.
//

void brush_classify(SPObject *parent, int depth, GdkPixbuf **epixbuf, int *hatchType, U_COLORREF *hatchColor){
   if(depth==0){
      *epixbuf    = NULL;
      *hatchType  = -1;
      *hatchColor = U_RGB(0,0,0);
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
         strncpy(temp,pat_i->getAttribute("id"),31);  // Some names may be longer than EMFhatch#_###### 
         hatch_classify(temp,hatchType,hatchColor);
         if(*hatchType != -1)return;

         // still looking?  Look at this pattern's children, if there are any
         SPObject *child = pat_i->firstChild();
         while(child && !(*epixbuf) && (*hatchType == -1)){
            brush_classify(child, depth, epixbuf, hatchType, hatchColor);
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
          brush_classify(child, depth, epixbuf, hatchType, hatchColor);
          child = child->getNext();
       }
   }
}

//swap R/B in 4 byte pixel
void swapRBinRGBA(char *px, int pixels){
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

U_COLORREF avg_stop_color(SPGradient *gr){
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
      cr = U_RGB(
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

int hold_gradient(void *gr, int mode){
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
      throw "Fatal programming error, hold_gradient() in emf-print.cpp called with invalid draw mode";
   }
   return 1;
}

// fcolor is defined when gradients are being expanded, it is the color of one stripe or ring.
int PrintEmf::create_brush(SPStyle const *style, PU_COLORREF fcolor)
{
// std::cout << "create_brush " << std::endl;
    float         rgb[3];
    char         *rec;
    U_LOGBRUSH    lb;
    uint32_t      brush, fmode;
    enum drawmode fill_mode;
    GdkPixbuf    *pixbuf;
    uint32_t      brushStyle;
    int           hatchType;
    U_COLORREF    hatchColor;
    uint32_t      width  = 0; // quiets a harmless compiler warning, initialization not otherwise required.
    uint32_t      height = 0;

    if (!et) return 0;

    // set a default fill in case we can't figure out a better way to do it
    fmode      = U_ALTERNATE;
    fill_mode  = DRAW_PAINT;
    brushStyle = U_BS_SOLID;
    hatchType  = U_HS_SOLIDCLR;
    if(fcolor){ hatchColor = *fcolor;        }
    else {      hatchColor = U_RGB(0, 0, 0); }

    if (!fcolor && style) {
        if(style->fill.isColor()){
           fill_mode = DRAW_PAINT;
           float opacity = SP_SCALE24_TO_FLOAT(style->fill_opacity.value);
           if (opacity <= 0.0) return 1;  // opacity isn't used here beyond this

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
           brush_classify(pat,0,&pixbuf,&hatchType,&hatchColor);
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

    lb   = logbrush_set(brushStyle, hatchColor, hatchType);

    switch(fill_mode){
       case DRAW_LINEAR_GRADIENT: // fill with average color unless gradients are converted to slices
       case DRAW_RADIAL_GRADIENT: // ditto
       case DRAW_PAINT:
       case DRAW_PATTERN:
          rec = createbrushindirect_set(&brush, eht, lb);
          if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
             throw "Fatal programming error in PrintEmf::create_brush at createbrushindirect_set";
          }
          hbrush = brush;  // need this later for destroy_brush

          rec = selectobject_set(brush, eht);
          if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
             throw "Fatal programming error in PrintEmf::create_brush at selectobject_set";
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
          rec = createdibpatternbrushpt_set(&brush, eht, U_DIB_RGB_COLORS, Bmi, cbPx, px);
          if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
             throw "Fatal programming error in PrintEmf::create_brush at createdibpatternbrushpt_set";
          }
          free(px);
          free(Bmi); // ct will be NULL because of colortype

          rec = selectobject_set(brush, eht);
          if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
             throw "Fatal programming error in PrintEmf::create_brush at selectobject_set";
          }
          break;
    }
    rec = U_EMRSETPOLYFILLMODE_set(fmode); 
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::create_brush at U_EMRSETPOLYdrawmode_set";
    }
// std::cout << "end create_brush " << std::endl;
    return 0;
}


void PrintEmf::destroy_brush()
{
// std::cout << "destroy_brush " << std::endl;
    char *rec;
    // before an object may be safely deleted it must no longer be selected
    // select in a stock object to deselect this one, the stock object should
    // never be used because we always select in a new one before drawing anythingrestore previous brush, necessary??? Would using a default stock object not work?
    rec = selectobject_set(U_NULL_BRUSH, eht);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::destroy_brush at selectobject_set";
    }
    if (hbrush){
       rec = deleteobject_set(&hbrush, eht);
       if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
         throw "Fatal programming error in PrintEmf::destroy_brush";
       }
       hbrush = 0;
    }
// std::cout << "end destroy_brush" << std::endl;
}


int PrintEmf::create_pen(SPStyle const *style, const Geom::Affine &transform)
{
    U_EXTLOGPEN         *elp;
    U_NUM_STYLEENTRY     n_dash    = 0;
    U_STYLEENTRY        *dash      = NULL;
    char                *rec       = NULL;
    int                  linestyle = U_PS_SOLID;
    int                  linecap   = 0;
    int                  linejoin  = 0;
    uint32_t             pen;
    uint32_t             penStyle;
    GdkPixbuf           *pixbuf;
    int                  hatchType;
    U_COLORREF           hatchColor;
    uint32_t             width,height;
    char                *px=NULL;
    char                *rgba_px;
    uint32_t             cbPx=0;
    uint32_t             colortype;
    PU_RGBQUAD           ct=NULL;
    int                  numCt=0;
    U_BITMAPINFOHEADER   Bmih;
    PU_BITMAPINFO        Bmi=NULL;
// std::cout << "create_pen " << std::endl;
  
    if (!et) return 0;

    // set a default stroke  in case we can't figure out a better way to do it
    penStyle   = U_BS_SOLID;
    hatchColor = U_RGB(0, 0, 0);
    hatchType  = U_HS_HORIZONTAL;

    if (style) {
        float rgb[3];

        if(SP_IS_PATTERN(SP_STYLE_STROKE_SERVER(style))){ // must be paint-server
           SPPaintServer *paintserver = style->stroke.value.href->getObject();
           SPPattern *pat = SP_PATTERN (paintserver);
           double dwidth  = pattern_width(pat);
           double dheight = pattern_height(pat);
           width  = dwidth;
           height = dheight;
           brush_classify(pat,0,&pixbuf,&hatchType,&hatchColor);
           if(pixbuf){
              penStyle    = U_BS_DIBPATTERN;
              rgba_px = (char *) gdk_pixbuf_get_pixels(pixbuf); // Do NOT free this!!!
              colortype = U_BCBM_COLOR32;
              (void) RGBA_to_DIB(&px, &cbPx, &ct, &numCt,  rgba_px,  width, height, width*4, colortype, 0, 1);
              // Not sure why the next swap is needed because the preceding does it, and the code is identical
              // to that in stretchdibits_set, which does not need this.
              swapRBinRGBA(px, width*height);
              Bmih = bitmapinfoheader_set(width, height, 1, colortype, U_BI_RGB, 0, PXPERMETER, PXPERMETER, numCt, 0);
              Bmi = bitmapinfo_set(Bmih, ct);
           }
           else {  // pattern
              penStyle    = U_BS_HATCHED;
              if(hatchType == -1){  // Not a standard hatch, so force it to something
                 hatchType  = U_HS_CROSS;
                 hatchColor = U_RGB(0xFF,0xC3,0xC3);
              }
           }
           if(FixPPTPatternAsHatch){
              if(hatchType == -1){  // image or unclassified 
                 penStyle     = U_BS_HATCHED;
                 hatchType    = U_HS_DIAGCROSS;
                 hatchColor   = U_RGB(0xFF,0xC3,0xC3);
              } 
           }
        }
        else if(SP_IS_GRADIENT(SP_STYLE_STROKE_SERVER(style))){ // must be a gradient
           // currently we do not do anything with gradients, the code below has no net effect.

           SPPaintServer *paintserver = style->stroke.value.href->getObject();
           if (SP_IS_LINEARGRADIENT (paintserver)) {
              SPLinearGradient *lg=SP_LINEARGRADIENT(paintserver);

              SP_GRADIENT(lg)->ensureVector(); // when exporting from commandline, vector is not built

              Geom::Point p1 (lg->x1.computed, lg->y1.computed);
              Geom::Point p2 (lg->x2.computed, lg->y2.computed);

              if (lg->gradientTransform_set) {
                 p1 = p1 * lg->gradientTransform;
                 p2 = p2 * lg->gradientTransform;
              }
              hatchColor = avg_stop_color(lg);
           }
           else if (SP_IS_RADIALGRADIENT (paintserver)) {
              SPRadialGradient *rg=SP_RADIALGRADIENT(paintserver);

              SP_GRADIENT(rg)->ensureVector(); // when exporting from commandline, vector is not built
              double r = rg->r.computed;

              Geom::Point c (rg->cx.computed, rg->cy.computed);
              Geom::Point xhandle_point(r, 0);
              Geom::Point yhandle_point(0, -r);
              yhandle_point += c;
              xhandle_point += c;
              if (rg->gradientTransform_set) {
                 c           = c           * rg->gradientTransform;
                 yhandle_point = yhandle_point * rg->gradientTransform;
                 xhandle_point = xhandle_point * rg->gradientTransform;
              }
              hatchColor = avg_stop_color(rg);
           }
           else {
             // default fill
           }
        }
        else if(style->stroke.isColor()){ // test last, always seems to be set, even for other types above
           sp_color_get_rgb_floatv( &style->stroke.value.color, rgb );
           penStyle   = U_BS_SOLID;
           hatchColor = U_RGB(255*rgb[0], 255*rgb[1], 255*rgb[2]);
           hatchType  = U_HS_SOLIDCLR;
        }
        else {
          // default fill
        }



        using Geom::X;
        using Geom::Y;

        Geom::Point zero(0, 0);
        Geom::Point one(1, 1);
        Geom::Point p0(zero * transform);
        Geom::Point p1(one * transform);
        Geom::Point p(p1 - p0);

        double scale = sqrt( (p[X]*p[X]) + (p[Y]*p[Y]) ) / sqrt(2);

        if(!style->stroke_width.computed){return 0;}  //if width is 0 do not (reset) the pen, it should already be NULL_PEN
        uint32_t linewidth = MAX( 1, (uint32_t) (scale * style->stroke_width.computed * PX2WORLD) );

        if (style->stroke_linecap.computed == 0) {
            linecap = U_PS_ENDCAP_FLAT;
        }
        else if (style->stroke_linecap.computed == 1) {
            linecap = U_PS_ENDCAP_ROUND;
        }
        else if (style->stroke_linecap.computed == 2) {
            linecap = U_PS_ENDCAP_SQUARE;
        }

        if (style->stroke_linejoin.computed == 0) {
            linejoin = U_PS_JOIN_MITER;
        }
        else if (style->stroke_linejoin.computed == 1) {
            linejoin = U_PS_JOIN_ROUND;
        }
        else if (style->stroke_linejoin.computed == 2) {
            linejoin = U_PS_JOIN_BEVEL;
        }

        if (style->stroke_dash.n_dash   &&
            style->stroke_dash.dash       )
        {
            if(FixPPTDashLine){ // will break up line into many smaller lines.  Override gradient if that was set, cannot do both.
               penStyle   = U_BS_SOLID;
               hatchType  = U_HS_HORIZONTAL;
            }
            else {
               int i = 0;
               while (linestyle != U_PS_USERSTYLE &&
                      (i < style->stroke_dash.n_dash)) {
                   if (style->stroke_dash.dash[i] > 0.00000001)
                       linestyle = U_PS_USERSTYLE;
                   i++;
               }

               if (linestyle == U_PS_USERSTYLE) {
                   n_dash = style->stroke_dash.n_dash;
                   dash = new uint32_t[n_dash];
                   for (i = 0; i < style->stroke_dash.n_dash; i++) {
                       dash[i] = (uint32_t) (style->stroke_dash.dash[i]);
                   }
               }
            }
        }

        elp = extlogpen_set(
            U_PS_GEOMETRIC | linestyle | linecap | linejoin,
            linewidth,
            penStyle,
            hatchColor,
            hatchType,
            n_dash,
            dash);

    }
    else { // if (!style)
        linejoin=0;
        elp = extlogpen_set(
            linestyle,
            1,
            U_BS_SOLID,
            U_RGB(0,0,0),
            U_HS_HORIZONTAL,
            0,
            NULL);
    }

    rec = extcreatepen_set(&pen, eht,  Bmi, cbPx, px, elp );
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::create_pen at extcreatepen_set";
    }
    free(elp);
    if(Bmi)free(Bmi);
    if(px)free(px);  // ct will always be NULL

    rec = selectobject_set(pen, eht);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::create_pen at selectobject_set";
    }
    hpen = pen;  // need this later for destroy_pen

    if (linejoin == U_PS_JOIN_MITER) {
        float miterlimit = style->stroke_miterlimit.value;  // This is a ratio.

        if (miterlimit < 1)miterlimit = 1;

        rec = U_EMRSETMITERLIMIT_set((uint32_t) miterlimit);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
            throw "Fatal programming error in PrintEmf::create_pen at U_EMRSETMITERLIMIT_set";
        }
    }

    if (n_dash) {
        delete[] dash;
    }
    return 0;
// std::cout << "end create_pen" << std::endl;
}

// set the current pen to the stock object NULL_PEN and then delete the defined pen object, if there is one.
void PrintEmf::destroy_pen()
{
// std::cout << "destroy_pen hpen: " << hpen<< std::endl;
    char *rec = NULL;
    // before an object may be safely deleted it must no longer be selected
    // select in a stock object to deselect this one, the stock object should
    // never be used because we always select in a new one before drawing anythingrestore previous brush, necessary??? Would using a default stock object not work?
    rec = selectobject_set(U_NULL_PEN, eht);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::destroy_pen at selectobject_set";
    }
    if (hpen){
       rec = deleteobject_set(&hpen, eht);
       if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
         throw "Fatal programming error in PrintEmf::destroy_pen";
       }
       hpen = 0;
    }
// std::cout << "end destroy_pen " << std::endl;
}



unsigned int PrintEmf::bind(Inkscape::Extension::Print * /*mod*/, Geom::Affine const &transform, float /*opacity*/)
{   
// std::cout << "bind " << std::endl;
    if (!m_tr_stack.empty()) {
        Geom::Affine tr_top = m_tr_stack.top();
        m_tr_stack.push(transform * tr_top);
    } else {
        m_tr_stack.push(transform);
    }

// std::cout << "end bind" << std::endl;
    return 1;
}

unsigned int PrintEmf::release(Inkscape::Extension::Print * /*mod*/)
{
// std::cout << "release " << std::endl;
    m_tr_stack.pop();
// std::cout << "end release" << std::endl;
    return 1;
}

#define clrweight(a,b,t) ((1-t)*((double) a) + (t)*((double) b))
inline U_COLORREF weight_opacity(U_COLORREF c1){
    float opa = c1.Reserved/255.0;
    U_COLORREF result = U_RGB(
        255*opweight((float)c1.Red  /255.0, gv.rgb[0], opa),
        255*opweight((float)c1.Green/255.0, gv.rgb[1], opa),
        255*opweight((float)c1.Blue /255.0, gv.rgb[2], opa)
    );
    return result;
}


// return the color between c1 and c2, c1 for t=0, c2 for t=1.0
U_COLORREF weight_colors(U_COLORREF c1, U_COLORREF c2, double t){
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
Geom::PathVector center_ellipse_as_SVG_PathV(Geom::Point ctr, double rx, double ry, double F){
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
Geom::PathVector center_elliptical_ring_as_SVG_PathV(Geom::Point ctr, double rx1, double ry1, double rx2, double ry2, double F){
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
      x21,y21,  rx2,ry2,degrot,x22,y22,   rx2,ry2,degrot,x21,y21);
    std::vector<Geom::Path> outres =  Geom::parse_svg_path(text);

    return outres;
}

/* Elliptical hole in a large square extending from -50k to +50k */
Geom::PathVector center_elliptical_hole_as_SVG_PathV(Geom::Point ctr, double rx, double ry, double F){
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
Geom::PathVector rect_cutter(Geom::Point ctr, Geom::Point pos, Geom::Point neg, Geom::Point width){
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

/* Convert from SPWindRule to livarot's FillRule
   This is similar to what sp_selected_path_boolop() does
*/
FillRule SPWR_to_LVFR(SPWindRule wr){
    FillRule fr;
    if(wr ==  SP_WIND_RULE_EVENODD){
       fr = fill_oddEven;
    }
    else {
       fr = fill_nonZero;
    }
    return fr;
}


unsigned int PrintEmf::fill(Inkscape::Extension::Print * /*mod*/,
                    Geom::PathVector const &pathv, Geom::Affine const & /*transform*/, SPStyle const *style,
                    Geom::OptRect const &/*pbox*/, Geom::OptRect const &/*dbox*/, Geom::OptRect const &/*bbox*/)
{
// std::cout << "fill " << std::endl;
    using Geom::X;
    using Geom::Y;

    Geom::Affine tf = m_tr_stack.top();

    use_fill   = true;
    use_stroke = false;

    // earlier versions had flush of fill here, but it never executed and was removed

    fill_transform = tf;

    if (create_brush(style, NULL)){ 
       /*
          Handle gradients.  Uses modified livarot as 2geom boolops is currently broken.
          Can handle gradients with multiple stops.

          The overlap is needed to avoid antialiasing artifacts when edges are not strictly aligned on pixel boundaries.
          There is an inevitable loss of accuracy saving through an EMF file because of the integer coordinate system.
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
          throw "Fatal programming error in PrintEmf::fill, invalid gradient type detected";
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
            (style->stroke.noneSet || style->stroke_width.computed == 0.0)               ||
            (style->stroke_dash.n_dash   &&  style->stroke_dash.dash  && FixPPTDashLine) ||
            !all_closed
          )
       {
          print_pathv(pathv, fill_transform);  // do any fills. side effect: clears fill_pathv
          use_fill = false;
       }
    }



// std::cout << "end fill" << std::endl;
    return 0;
}


unsigned int PrintEmf::stroke (Inkscape::Extension::Print * /*mod*/,
                       Geom::PathVector const &pathv, const Geom::Affine &/*transform*/, const SPStyle *style,
                       Geom::OptRect const &/*pbox*/, Geom::OptRect const &/*dbox*/, Geom::OptRect const &/*bbox*/)
{
// std::cout << "stroke " << std::endl;
    
    Geom::Affine tf = m_tr_stack.top();

    use_stroke = true;
//  use_fill was set in ::fill, if it is needed

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


// std::cout << "end stroke " << std::endl;
    return 0;
}


// Draws simple_shapes, those with closed EMR_* primitives, like polygons, rectangles and ellipses.
// These use whatever the current pen/brush are and need not be followed by a FILLPATH or STROKEPATH.
// For other paths it sets a few flags and returns.
bool PrintEmf::print_simple_shape(Geom::PathVector const &pathv, const Geom::Affine &transform)
{
// std::cout << "print_simple_shape " << std::endl <<std::flush;

    Geom::PathVector pv = pathv_to_linear_and_cubic_beziers( pathv * transform );
    
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
            
            if ( is_straight_curve(*cit) ) {
                lines++;
            }
            else if (Geom::CubicBezier const *cubic = dynamic_cast<Geom::CubicBezier const*>(&*cit)) {
                cubic = cubic;
                curves++;
            }
        }
    }

    if (!nodes)
        return false;
    
    U_POINT *lpPoints = new U_POINT[moves + lines + curves*3];
    int i = 0;

    /**
     * For all Subpaths in the <path>
     */
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

        /**
         * For all segments in the subpath
         */
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
 
        if (use_fill && !use_stroke) {  // only fill
            rec = selectobject_set(U_NULL_PEN, eht);
            if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
                throw "Fatal programming error in PrintEmf::print_simple_shape at selectobject_set pen";
            }
        }
        else if(!use_fill && use_stroke) { // only stroke
            rec = selectobject_set(U_NULL_BRUSH, eht);
            if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
                throw "Fatal programming error in PrintEmf::print_simple_shape at selectobject_set brush";
            }
        }

        if (polygon) {
            if (rectangle){
              U_RECTL rcl = rectl_set((U_POINTL) {lpPoints[0].x, lpPoints[0].y}, (U_POINTL) {lpPoints[2].x, lpPoints[2].y});
              rec = U_EMRRECTANGLE_set(rcl);
            }
            else {
               rec = U_EMRPOLYGON_set(U_RCL_DEF, nodes, lpPoints);
            }
        }
        else if (ellipse) {
            U_RECTL rcl = rectl_set((U_POINTL) {lpPoints[6].x, lpPoints[3].y}, (U_POINTL) {lpPoints[0].x, lpPoints[9].y});
            rec = U_EMRELLIPSE_set(rcl);
        }
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
            throw "Fatal programming error in PrintEmf::print_simple_shape at retangle/ellipse/polygon";
        }
        
        done = true;

        // replace the handle we moved above, assuming there was something set already
        if (use_fill && !use_stroke && hpen) { // only fill
           rec = selectobject_set(hpen, eht);
           if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
               throw "Fatal programming error in PrintEmf::print_simple_shape at selectobject_set pen";
           }
        }
        else if (!use_fill && use_stroke && hbrush){ // only stroke
           rec = selectobject_set(hbrush, eht);
           if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
               throw "Fatal programming error in PrintEmf::print_simple_shape at selectobject_set brush";
           }
        }

    }

    delete[] lpPoints;
    
// std::cout << "end simple_shape " << std::endl;
    return done;
}

/** Some parts based on win32.cpp by Lauris Kaplinski <lauris@kaplinski.com>.  Was a part of Inkscape
   in the past (or will be in the future?)  Not in current trunk. (4/19/2012)
   
   Limitations of this code:
   1.  rotated images are mangled.  They stay in their original orientation and are stretched
       along X or Y.  
   2.  Transparency is lost on export.  (Apparently a limitation of the EMF format.)
   3.  Probably messes up if row stride != w*4
   4.  There is still a small memory leak somewhere, possibly in a pixbuf created in a routine
       that calls this one and passes px, but never removes the rest of the pixbuf.  The first time
       this is called it leaked 5M (in one test) and each subsequent call leaked around 200K more.
       If this routine is reduced to 
         if(1)return(0);
       and called for a single 1280 x 1024 image then the program leaks 11M per call, or roughly the
       size of two bitmaps.
*/

unsigned int PrintEmf::image(Inkscape::Extension::Print * /* module */,  /** not used */
                           unsigned char *rgba_px,   /** array of pixel values, Gdk::Pixbuf bitmap format */
                           unsigned int w,      /** width of bitmap */
                           unsigned int h,      /** height of bitmap */
                           unsigned int rs,     /** row stride (normally w*4) */
                           Geom::Affine const &tf_ignore,  /** WRONG affine transform, use the one from m_tr_stack */
                           SPStyle const *style)  /** provides indirect link to image object */
{
// std::cout << "image " << std::endl;
     double x1,x2,y1,y2;
     char *rec = NULL;
     Geom::Affine tf = m_tr_stack.top();

     rec = U_EMRSETSTRETCHBLTMODE_set(U_COLORONCOLOR);
     if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::image at EMRHEADER";
     }

     x1=     atof(style->object->getAttribute("x"));
     y1=     atof(style->object->getAttribute("y"));
     x2=x1 + atof(style->object->getAttribute("width"));
     y2=y1 + atof(style->object->getAttribute("height"));
     Geom::Point pLL(x1,y1);
     Geom::Point pUR(x2,y2);
     Geom::Point p2LL = pLL * tf;
     Geom::Point p2UR = pUR * tf;

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

     U_POINTL Dest  = pointl_set(round(p2LL[Geom::X] * PX2WORLD), round(p2LL[Geom::Y] * PX2WORLD));
     U_POINTL cDest = pointl_set(round((p2UR[Geom::X]-p2LL[Geom::X]) * PX2WORLD), round((p2UR[Geom::Y]-p2LL[Geom::Y]) * PX2WORLD));
     U_POINTL Src   = pointl_set(0,0);
     U_POINTL cSrc  = pointl_set(w,h);
     rec = U_EMRSTRETCHDIBITS_set(
           U_RCL_DEF,           //! Bounding rectangle in device units
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
     if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::image at U_EMRSTRETCHDIBITS_set";
     }
     free(px);
     free(Bmi);
     if(numCt)free(ct);
        
// std::cout << "end image" << std::endl;
  return 0;
}

// may also be called with a simple_shape or an empty path, whereupon it just returns without doing anything
unsigned int PrintEmf::print_pathv(Geom::PathVector const &pathv, const Geom::Affine &transform)
{
// std::cout << "print_pathv " << std::endl << std::flush;
    char *rec = NULL;

    simple_shape = print_simple_shape(pathv, transform);
    if (simple_shape || pathv.empty()){
       if (use_fill){    destroy_brush(); }  // these must be cleared even if nothing is drawn or hbrush,hpen fill up
       if (use_stroke){  destroy_pen();   }
       return TRUE;
    }

    Geom::PathVector pv = pathv_to_linear_and_cubic_beziers( pathv * transform );
    
    rec = U_EMRBEGINPATH_set();
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::print_pathv at U_EMRBEGINPATH_set";
    }

    /**
     * For all Subpaths in the <path>
     */
    for (Geom::PathVector::const_iterator pit = pv.begin(); pit != pv.end(); ++pit)
    {
        using Geom::X;
        using Geom::Y;

 
        Geom::Point p0 = pit->initialPoint();

        p0[X] = (p0[X] * PX2WORLD);
        p0[Y] = (p0[Y] * PX2WORLD);
        
        U_POINTL ptl = pointl_set((int32_t) round(p0[X]), (int32_t) round(p0[Y]));
        rec = U_EMRMOVETOEX_set(ptl);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
            throw "Fatal programming error in PrintEmf::print_pathv at U_EMRMOVETOEX_set";
        }

        /**
         * For all segments in the subpath
         */
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

                ptl = pointl_set((int32_t) round(p1[X]), (int32_t) round(p1[Y]));
                rec = U_EMRLINETO_set(ptl);
                if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
                    throw "Fatal programming error in PrintEmf::print_pathv at U_EMRLINETO_set";
                }
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

                U_POINTL pt[3];
                pt[0].x = x1;
                pt[0].y = y1;
                pt[1].x = x2;
                pt[1].y = y2;
                pt[2].x = x3;
                pt[2].y = y3;

                rec = U_EMRPOLYBEZIERTO_set(U_RCL_DEF, 3, pt);
                if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
                    throw "Fatal programming error in PrintEmf::print_pathv at U_EMRPOLYBEZIERTO_set";
                }
            }
            else
            {
                g_warning("logical error, because pathv_to_linear_and_cubic_beziers was used");
            }
        }

        if (pit->end_default() == pit->end_closed()) {  // there may be multiples of this on a single path
            rec = U_EMRCLOSEFIGURE_set();
            if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
                throw "Fatal programming error in PrintEmf::print_pathv at U_EMRCLOSEFIGURE_set";
            }
        }

    }

    rec = U_EMRENDPATH_set();  // there may be only be one of these on a single path
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
        throw "Fatal programming error in PrintEmf::print_pathv at U_EMRENDPATH_set";
    }

    // explicit FILL/STROKE commands are needed for each sub section of the path
    if (use_fill && !use_stroke){
        rec = U_EMRFILLPATH_set(U_RCL_DEF);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
          throw "Fatal programming error in PrintEmf::fill at U_EMRFILLPATH_set";
        }
    }
    else if (use_fill && use_stroke) {
        rec  = U_EMRSTROKEANDFILLPATH_set(U_RCL_DEF);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){                        
            throw "Fatal programming error in PrintEmf::stroke at U_EMRSTROKEANDFILLPATH_set"; 
        }                                                                                     
    }
    else if (!use_fill && use_stroke){
        rec  = U_EMRSTROKEPATH_set(U_RCL_DEF);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){                        
            throw "Fatal programming error in PrintEmf::stroke at U_EMRSTROKEPATH_set"; 
        }                                                                                     
    }

    // clean out brush and pen, but only after all parts of the draw complete
    if (use_fill){
        destroy_brush();
    }
    if (use_stroke){
        destroy_pen();
    }
// std::cout << "end pathv" << std::endl;

    return TRUE;
}


bool PrintEmf::textToPath(Inkscape::Extension::Print * ext)
{
    return ext->get_param_bool("textToPath");
}

unsigned int PrintEmf::text(Inkscape::Extension::Print * /*mod*/, char const *text, Geom::Point const &p,
                    SPStyle const *const style)
{
// std::cout << "text "  << std::endl;
    if (!et) return 0;

    char *rec = NULL;
    int ccount,newfont;
    int fix90n=0;
    uint32_t hfont = 0;
    Geom::Affine tf = m_tr_stack.top();
    double rot = -1800.0*std::atan2(tf[1], tf[0])/M_PI;    // 0.1 degree rotation,  - sign for MM_TEXT
    double rotb = -std::atan2(tf[1], tf[0]);  // rotation for baseline offset for superscript/subscript, used below
    double dx,dy;
    double f1,f2,f3;

#ifdef USE_PANGO_WIN32
/*
    font_instance *tf = (font_factory::Default())->Face(style->text->font_family.value, font_style_to_pos(*style));
    if (tf) {
        LOGFONT *lf = pango_win32_font_logfont(tf->pFont);
        tf->Unref();
        hfont = CreateFontIndirect(lf);
        g_free(lf);
    }
*/
#endif

    // the dx array is smuggled in like: text<nul>w1 w2 w3 ...wn<nul><nul>, where the widths are floats 7 characters wide, including the space
    int ndx;
    uint32_t *adx;
    smuggle_adx_out(text, &adx, &ndx, PX2WORLD * std::min(tf.expansionX(),tf.expansionY())); // side effect: free() adx
    
    char *text2 = strdup(text);  // because U_Utf8ToUtf16le calls iconv which does not like a const char *
    uint16_t *unicode_text = U_Utf8ToUtf16le( text2, 0, NULL );
    free(text2);
    //translates Unicode to NonUnicode, if possible.  If any translate, all will, and all to
    //the same font, because of code in Layout::print
    UnicodeToNon(unicode_text, &ccount, &newfont);

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

    /* Note that text font sizes are stored into the EMF as fairly small integers and that limits their precision.  
       The EMF output files produced here have been designed so that the integer valued pt sizes
       land right on an integer value in the EMF file, so those are exact.  However, something like 18.1 pt will be
       somewhat off, so that when it is read back in it becomes 18.11 pt.  (For instance.)   
    */
    int textheight = round(-style->font_size.computed * PX2WORLD * std::min(tf.expansionX(),tf.expansionY()));
    if (!hfont) {

        // Get font face name.  Use changed font name if unicode mapped to one
        // of the special fonts.
        uint16_t *wfacename;
        if(!newfont){
           wfacename = U_Utf8ToUtf16le(style->text->font_family.value, 0, NULL);
        }
        else {
           wfacename = U_Utf8ToUtf16le(FontName(newfont), 0, NULL);
        }

        // Scale the text to the minimum stretch. (It tends to stay within bounding rectangles even if
        // it was streteched asymmetrically.)  Few applications support text from EMF which is scaled
        // differently by height/width, so leave lfWidth alone.  

        U_LOGFONT lf = logfont_set(
            textheight, 
            0,        
            rot,
            rot,
            transweight(style->font_weight.computed),
            (style->font_style.computed == SP_CSS_FONT_STYLE_ITALIC),
            style->text_decoration.underline,
            style->text_decoration.line_through,
            U_DEFAULT_CHARSET,
            U_OUT_DEFAULT_PRECIS,
            U_CLIP_DEFAULT_PRECIS,
            U_DEFAULT_QUALITY,
            U_DEFAULT_PITCH | U_FF_DONTCARE,
            wfacename);
	free(wfacename);
       
        rec  = extcreatefontindirectw_set(&hfont, eht,  (char *) &lf, NULL);
        if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
           throw "Fatal programming error in PrintEmf::text at extcreatefontindirectw_set";
        }
    }
    
    rec = selectobject_set(hfont, eht);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at selectobject_set";
    }

    float rgb[3];
    sp_color_get_rgb_floatv( &style->fill.value.color, rgb );
    rec = U_EMRSETTEXTCOLOR_set(U_RGB(255*rgb[0], 255*rgb[1], 255*rgb[2]));
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at U_EMRSETTEXTCOLOR_set";
    }

    // Text alignment:
    //   - (x,y) coordinates received by this filter are those of the point where the text
    //     actually starts, and already takes into account the text object's alignment;
    //   - for this reason, the EMF text alignment must always be TA_BASELINE|TA_LEFT.
    rec = U_EMRSETTEXTALIGN_set(U_TA_BASELINE | U_TA_LEFT);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at U_EMRSETTEXTALIGN_set";
    }

    // Transparent text background
    rec = U_EMRSETBKMODE_set(U_TRANSPARENT);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at U_EMRSETBKMODE_set";
    }

    Geom::Point p2 = p * tf;

    //Handle super/subscripts.  Negative sign because of geometry of MM_TEXT.
    p2[Geom::X] -= style->baseline_shift.computed * std::sin( rotb );
    p2[Geom::Y] -= style->baseline_shift.computed * std::cos( rotb );

    //Conditionally handle compensation for PPT EMF import bug (affects PPT 2003-2010, at least)
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
    char *rec2 = emrtext_set( (U_POINTL) {xpos, ypos}, ndx, 2, unicode_text, U_ETO_NONE, U_RCL_DEF, adx);
    free(unicode_text);
    free(adx);
    rec = U_EMREXTTEXTOUTW_set(U_RCL_DEF,U_GM_COMPATIBLE,1.0,1.0,(PU_EMRTEXT)rec2);
    free(rec2);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at U_EMREXTTEXTOUTW_set";
    }

    // Must deselect an object before deleting it.  Put the default font (back) in.
    rec = selectobject_set(U_DEVICE_DEFAULT_FONT, eht);
    if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
       throw "Fatal programming error in PrintEmf::text at selectobject_set";
    }

    if(hfont){
       rec = deleteobject_set(&hfont, eht);
       if(!rec || emf_append((PU_ENHMETARECORD)rec, et, U_REC_FREE)){
         throw "Fatal programming error in PrintEmf::text at deleteobject_set";
       }
    }
    
// std::cout << "end text" << std::endl;
    return 0;
}

void PrintEmf::init (void)
{
// std::cout << "init " << std::endl;
    read_system_fflist();

    /* EMF print */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
        "<name>Enhanced Metafile Print</name>\n"
        "<id>org.inkscape.print.emf</id>\n"
        "<param name=\"destination\" type=\"string\"></param>\n"
        "<param name=\"textToPath\" type=\"boolean\">true</param>\n"
        "<param name=\"pageBoundingBox\" type=\"boolean\">true</param>\n"
        "<param name=\"FixPPTCharPos\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTDashLine\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTGrad2Polys\" type=\"boolean\">false</param>\n"
        "<param name=\"FixPPTPatternAsHatch\" type=\"boolean\">false</param>\n"
        "<print/>\n"
        "</inkscape-extension>", new PrintEmf());

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
