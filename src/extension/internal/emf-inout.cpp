/** @file
 * @brief Windows-only Enhanced Metafile input and output.
 */
/* Authors:
 *   Ulf Erikson <ulferikson@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2006-2008 Authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 *
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

#define  EMF_DRIVER
#include "sp-root.h"
#include "sp-path.h"
#include "style.h"
#include "print.h"
#include "extension/system.h"
#include "extension/print.h"
#include "extension/db.h"
#include "extension/input.h"
#include "extension/output.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "unit-constants.h"
#include "clear-n_.h"
#include "document.h"
#include "libunicode-convert/unicode-convert.h"


#include "emf-print.h"
#include "emf-inout.h"
#include "uemf.h"

#define PRINT_EMF "org.inkscape.print.emf"

#ifndef U_PS_JOIN_MASK
#define U_PS_JOIN_MASK (U_PS_JOIN_BEVEL|U_PS_JOIN_MITER|U_PS_JOIN_ROUND)
#endif

namespace Inkscape {
namespace Extension {
namespace Internal {


static float device_scale = DEVICESCALE;
static U_RECTL rc_old;
static bool clipset = false;
static uint32_t ICMmode=0;  // not used yet, but code to read it from EMF implemented
static uint32_t BLTmode=0;

/** Construct a PNG in memory from an RGB from the EMF file 

from:
http://www.lemoda.net/c/write-png/

which was based on:
http://stackoverflow.com/questions/1821806/how-to-encode-png-to-buffer-using-libpng

gcc -Wall -o testpng testpng.c -lpng
*/

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
    
/* A coloured pixel. */

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t opacity;
} pixel_t;

/* A picture. */
    
typedef struct  {
    pixel_t *pixels;
    size_t width;
    size_t height;
} bitmap_t;
    
/* structure to store PNG image bytes */
typedef struct {
      char *buffer;
      size_t size;
} MEMPNG, *PMEMPNG;

/* Given "bitmap", this returns the pixel of bitmap at the point 
   ("x", "y"). */

static pixel_t * pixel_at (bitmap_t * bitmap, int x, int y)
{
    return bitmap->pixels + bitmap->width * y + x;
}
    
/* Write "bitmap" to a PNG file specified by "path"; returns 0 on
   success, non-zero on error. */



void
my_png_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
  PMEMPNG p=(PMEMPNG)png_get_io_ptr(png_ptr);
   
  size_t nsize = p->size + length;

  /* allocate or grow buffer */
  if(p->buffer)
    p->buffer = (char *) realloc(p->buffer, nsize);
  else
    p->buffer = (char *) malloc(nsize);

  if(!p->buffer)
    png_error(png_ptr, "Write Error");

  /* copy new bytes to end of buffer */
  memcpy(p->buffer + p->size, data, length);
  p->size += length;
}

void toPNG(PMEMPNG accum, int width, int height, char *px){
    bitmap_t bmstore;
    bitmap_t *bitmap=&bmstore;
    accum->buffer=NULL;  // PNG constructed in memory will end up here, caller must free().
    accum->size=0;
    bitmap->pixels=(pixel_t *)px;
    bitmap->width  = width;
    bitmap->height = height;
    
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    size_t x, y;
    png_byte ** row_pointers = NULL;
    /* The following number is set by trial and error only. I cannot
       see where it it is documented in the libpng manual.
    */
    int pixel_size = 3;
    int depth = 8;
    
    png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL){ 
        accum->buffer=NULL;
        return;
    }
    
    info_ptr = png_create_info_struct (png_ptr);
    if (info_ptr == NULL){
        png_destroy_write_struct (&png_ptr, &info_ptr);
        accum->buffer=NULL; 
        return;
    }
    
    /* Set up error handling. */

    if (setjmp (png_jmpbuf (png_ptr))) {
        png_destroy_write_struct (&png_ptr, &info_ptr);
        accum->buffer=NULL; 
        return;
    }
    
    /* Set image attributes. */

    png_set_IHDR (png_ptr,
                  info_ptr,
                  bitmap->width,
                  bitmap->height,
                  depth,
                  PNG_COLOR_TYPE_RGB,
                  PNG_INTERLACE_NONE,
                  PNG_COMPRESSION_TYPE_DEFAULT,
                  PNG_FILTER_TYPE_DEFAULT);
    
    /* Initialize rows of PNG. */

    row_pointers = (png_byte **) png_malloc (png_ptr, bitmap->height * sizeof (png_byte *));
    for (y = 0; y < bitmap->height; ++y) {
        png_byte *row = 
            (png_byte *) png_malloc (png_ptr, sizeof (uint8_t) * bitmap->width * pixel_size);
        row_pointers[bitmap->height - y - 1] = row;  // Row order in EMF is reversed.
        for (x = 0; x < bitmap->width; ++x) {
            pixel_t * pixel = pixel_at (bitmap, x, y);
            *row++ = pixel->red;   // R & B channels were set correctly by DIB_to_RGB
            *row++ = pixel->green;
            *row++ = pixel->blue;
        }
    }
    
    /* Write the image data to memory */

    png_set_rows (png_ptr, info_ptr, row_pointers);

    png_set_write_fn(png_ptr, accum, my_png_write_data, NULL);
    
    png_write_png (png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);
    
    for (y = 0; y < bitmap->height; y++) {
        png_free (png_ptr, row_pointers[y]);
    }
    png_free (png_ptr, row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    
}


/* convert an EMF RGB(A) color to 0RGB
inverse of gethexcolor() in emf-print.cpp
*/
uint32_t sethexcolor(U_COLORREF color){

    uint32_t out;
    out = (U_RGBAGetR(color) << 16) + 
          (U_RGBAGetG(color) << 8 ) +
          (U_RGBAGetB(color)      );
    return(out);
}


Emf::Emf (void) // The null constructor
{
    return;
}


Emf::~Emf (void) //The destructor
{
    return;
}


bool
Emf::check (Inkscape::Extension::Extension * /*module*/)
{
    if (NULL == Inkscape::Extension::db.get(PRINT_EMF))
        return FALSE;
    return TRUE;
}


static void
emf_print_document_to_file(SPDocument *doc, gchar const *filename)
{
    Inkscape::Extension::Print *mod;
    SPPrintContext context;
    gchar const *oldconst;
    gchar *oldoutput;
    unsigned int ret;

    doc->ensureUpToDate();

    mod = Inkscape::Extension::get_print(PRINT_EMF);
    oldconst = mod->get_param_string("destination");
    oldoutput = g_strdup(oldconst);
    mod->set_param_string("destination", filename);

/* Start */
    context.module = mod;
    /* fixme: This has to go into module constructor somehow */
    /* Create new arena */
    mod->base = doc->getRoot();
    Inkscape::Drawing drawing;
    mod->dkey = SPItem::display_key_new(1);
    mod->root = mod->base->invoke_show(drawing, mod->dkey, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(mod->root);
    /* Print document */
    ret = mod->begin(doc);
    if (ret) {
        g_free(oldoutput);
        throw Inkscape::Extension::Output::save_failed();
    }
    mod->base->invoke_print(&context);
    ret = mod->finish();
    /* Release arena */
    mod->base->invoke_hide(mod->dkey);
    mod->base = NULL;
    mod->root = NULL; // deleted by invoke_hide
/* end */

    mod->set_param_string("destination", oldoutput);
    g_free(oldoutput);

    return;
}


void
Emf::save(Inkscape::Extension::Output *mod, SPDocument *doc, gchar const *filename)
{
    Inkscape::Extension::Extension * ext;

    ext = Inkscape::Extension::db.get(PRINT_EMF);
    if (ext == NULL)
        return;

    bool new_val         = mod->get_param_bool("textToPath");
    bool new_FixPPTCharPos     = mod->get_param_bool("FixPPTCharPos");  // character position bug
    // reserve FixPPT2 for opacity bug.  Currently EMF does not export opacity values
    bool new_FixPPTDashLine       = mod->get_param_bool("FixPPTDashLine");  // dashed line bug
    bool new_FixPPTGrad2Polys     = mod->get_param_bool("FixPPTGrad2Polys");  // gradient bug
    bool new_FixPPTPatternAsHatch = mod->get_param_bool("FixPPTPatternAsHatch");  // force all patterns as standard EMF hatch

    TableGen(                  //possibly regenerate the unicode-convert tables
      mod->get_param_bool("TnrToSymbol"),
      mod->get_param_bool("TnrToWingdings"),
      mod->get_param_bool("TnrToZapfDingbats"),
      mod->get_param_bool("UsePUA")
    );

    ext->set_param_bool("FixPPTCharPos",new_FixPPTCharPos);   // Remember to add any new ones to PrintEmf::init or a mysterious failure will result!
    ext->set_param_bool("FixPPTDashLine",new_FixPPTDashLine);
    ext->set_param_bool("FixPPTGrad2Polys",new_FixPPTGrad2Polys);
    ext->set_param_bool("FixPPTPatternAsHatch",new_FixPPTPatternAsHatch);
    ext->set_param_bool("textToPath", new_val);

    emf_print_document_to_file(doc, filename);

    return;
}


enum drawmode {DRAW_PAINT, DRAW_PATTERN, DRAW_IMAGE};  // apply to either fill or stroke

typedef struct {
    int type;
    int level;
    char *lpEMFR;
} EMF_OBJECT, *PEMF_OBJECT;

typedef struct {
    int size;         // number of slots allocated in strings
    int count;        // number of slots used in strings
    char **strings;   // place to store strings
} EMF_STRINGS, *PEMF_STRINGS;

typedef struct emf_device_context {
    struct SPStyle style;
    char *font_name;
    bool stroke_set;
    int  stroke_mode;  // enumeration from drawmode, not used if fill_set is not True
    int  stroke_idx;   // used with DRAW_PATTERN and DRAW_IMAGE to return the appropriate fill
    bool fill_set;
    int  fill_mode;    // enumeration from drawmode, not used if fill_set is not True
    int  fill_idx;     // used with DRAW_PATTERN and DRAW_IMAGE to return the appropriate fill

    U_SIZEL sizeWnd;
    U_SIZEL sizeView;
    float PixelsInX, PixelsInY;
    float PixelsOutX, PixelsOutY;
    U_POINTL winorg;
    U_POINTL vieworg;
    double ScaleInX, ScaleInY;
    double ScaleOutX, ScaleOutY;
    U_COLORREF textColor;
    bool textColorSet;
    U_COLORREF bkColor;
    bool bkColorSet;
    uint32_t textAlign;
    U_XFORM worldTransform;
    U_POINTL cur;
} EMF_DEVICE_CONTEXT, *PEMF_DEVICE_CONTEXT;

#define EMF_MAX_DC 128


typedef struct emf_callback_data {
    Glib::ustring *outsvg;
    Glib::ustring *path;
    Glib::ustring *outdef;
    Glib::ustring *defs;

    EMF_DEVICE_CONTEXT dc[EMF_MAX_DC+1]; // FIXME: This should be dynamic..
    int level;
    
    double xDPI, yDPI;
    uint32_t mask;          // Draw properties
    int arcdir;             //U_AD_COUNTERCLOCKWISE 1 or U_AD_CLOCKWISE 2
    
    uint32_t dwRop2;         // Binary raster operation, 0 if none (use brush/pen unmolested)
    uint32_t dwRop3;         // Ternary raster operation, 0 if none (use brush/pen unmolested)

    float MMX;
    float MMY;
    float dwInchesX;
    float dwInchesY;

    unsigned int id;
    unsigned int drawtype;  // one of 0 or U_EMR_FILLPATH, U_EMR_STROKEPATH, U_EMR_STROKEANDFILLPATH
    char *pDesc;
                              // both of these end up in <defs> under the names shown here.  These structures allow duplicates to be avoided.
    EMF_STRINGS hatches;      // hold pattern names, all like EMFhatch#_$$$$$$ where # is the EMF hatch code and $$$$$$ is the color
    EMF_STRINGS images;       // hold images, all like Image#, where # is the slot the image lives.


    int n_obj;
    PEMF_OBJECT emf_obj;
} EMF_CALLBACK_DATA, *PEMF_CALLBACK_DATA;

/*  Add another 100 blank slots to the hatches array.
*/
void enlarge_hatches(PEMF_CALLBACK_DATA d){
   d->hatches.size += 100;
   d->hatches.strings = (char **) realloc(d->hatches.strings,d->hatches.size + sizeof(char *));
}

/*  See if the pattern name is already in the list.  If it is return its position (1->n, not 1-n-1)
*/
int in_hatches(PEMF_CALLBACK_DATA d, char *test){
   int i;
   for(i=0; i<d->hatches.count; i++){
     if(strcmp(test,d->hatches.strings[i])==0)return(i+1);
   }
   return(0);
}

/*  (Conditionally) add a hatch.  If a matching hatch already exists nothing happens.  If one
    does not exist it is added to the hatches list and also entered into <defs>. 
*/
uint32_t add_hatch(PEMF_CALLBACK_DATA d, uint32_t hatchType, U_COLORREF hatchColor){
   char hatchname[64]; // big enough
   char tmpcolor[8];
   uint32_t idx;

   if(hatchType==U_HS_DIAGCROSS){  // This is the only one with dependencies on others
      (void) add_hatch(d,U_HS_FDIAGONAL,hatchColor);
      (void) add_hatch(d,U_HS_BDIAGONAL,hatchColor);
   }
   
   sprintf(tmpcolor,"%6.6X",sethexcolor(hatchColor));
   switch(hatchType){
      case U_HS_SOLIDTEXTCLR:
      case U_HS_DITHEREDTEXTCLR:
         if(d->dc[d->level].textColorSet){
            sprintf(tmpcolor,"%6.6X",sethexcolor(d->dc[d->level].textColor));
         }
         break;
      case U_HS_SOLIDBKCLR:
      case U_HS_DITHEREDBKCLR:
         if(d->dc[d->level].bkColorSet){
            sprintf(tmpcolor,"%6.6X",sethexcolor(d->dc[d->level].bkColor));
         }
         break;
      default:
         break;
   }

   // EMF can take solid colors from background or the default text color but on conversion to inkscape
   // these need to go to a defined color.  Consequently the hatchType also has to go to a solid color, otherwise
   // on export the background/text might not match at the time this is written, and the colors will shift.
   if(hatchType > U_HS_SOLIDCLR)hatchType = U_HS_SOLIDCLR;

   sprintf(hatchname,"EMFhatch%d_%s",hatchType,tmpcolor);
   idx = in_hatches(d,hatchname);
   if(!idx){  // add it if not already present
      if(d->hatches.count == d->hatches.size){  enlarge_hatches(d); }
      d->hatches.strings[d->hatches.count++]=strdup(hatchname);

      *(d->defs) += "\n";
      *(d->defs) += "    <pattern id=\"";
      *(d->defs) += hatchname;
      *(d->defs) += "\"\n";
      switch(hatchType){
         case U_HS_HORIZONTAL:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  >\n";
            *(d->defs) += "       <path d=\"M 0 0 6 0\" style=\"fill:none;stroke:#";
            *(d->defs) += tmpcolor;
            *(d->defs) += "\" />\n";
            *(d->defs) += "    </pattern>\n";
            break;
         case U_HS_VERTICAL:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  >\n";
            *(d->defs) += "       <path d=\"M 0 0 0 6\" style=\"fill:none;stroke:#";
            *(d->defs) += tmpcolor;
            *(d->defs) += "\" />\n";
            *(d->defs) += "    </pattern>\n";
            break;
         case U_HS_FDIAGONAL:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  viewBox=\"0 0 6 6\" preserveAspectRatio=\"none\" >\n";
            *(d->defs) += "       <line x1=\"-1\" y1=\"-1\" x2=\"7\" y2=\"7\" stroke=\"#";
            *(d->defs) += tmpcolor;
            *(d->defs) += "\" id=\"sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\"/>\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(6,0)\"/>\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(-6,0)\"/>\n";
            *(d->defs) += "    </pattern>\n";
            break;
         case U_HS_BDIAGONAL:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  viewBox=\"0 0 6 6\" preserveAspectRatio=\"none\" >\n";
            *(d->defs) += "       <line x1=\"-1\" y1=\"7\" x2=\"7\" y2=\"-1\" stroke=\"#";
            *(d->defs) += tmpcolor;
            *(d->defs) += "\" id=\"sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\"/>\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(6,0)\"/>\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(-6,0)\"/>\n";
            *(d->defs) += "    </pattern>\n";
            break;
         case U_HS_CROSS:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  >\n";
            *(d->defs) += "       <path d=\"M 0 0 6 0 M 0 0 0 6\" style=\"fill:none;stroke:#";
            *(d->defs) += tmpcolor;
            *(d->defs) += "\" />\n";
            *(d->defs) += "    </pattern>\n";
             break;
         case U_HS_DIAGCROSS:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  viewBox=\"0 0 6 6\" preserveAspectRatio=\"none\" >\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            sprintf(hatchname,"EMFhatch%d_%6.6X",U_HS_FDIAGONAL,sethexcolor(hatchColor));
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(0,0)\"/>\n";
            *(d->defs) += "       <use xlink:href=\"#sub";
            sprintf(hatchname,"EMFhatch%d_%6.6X",U_HS_BDIAGONAL,sethexcolor(hatchColor));
            *(d->defs) += hatchname;
            *(d->defs) += "\" transform=\"translate(0,0)\"/>\n";
            *(d->defs) += "    </pattern>\n";
            break;
         case U_HS_SOLIDCLR:
         case U_HS_DITHEREDCLR:
         case U_HS_SOLIDTEXTCLR:
         case U_HS_DITHEREDTEXTCLR:
         case U_HS_SOLIDBKCLR:
         case U_HS_DITHEREDBKCLR:
         default:
            *(d->defs) += "       patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\" x=\"0\" y=\"0\"  >\n";
            *(d->defs) += "       <path d=\"M 0 0 6 0 6 6 0 6 z\" style=\"fill:#";
            *(d->defs) += tmpcolor;
            *(d->defs) += ";stroke:none";
            *(d->defs) += "\" />\n";
            *(d->defs) += "    </pattern>\n";
            break;
      }
      idx = d->hatches.count;
   }
   return(idx-1);
}

/*  Add another 100 blank slots to the images array.
*/
void enlarge_images(PEMF_CALLBACK_DATA d){
   d->images.size += 100;
   d->images.strings = (char **) realloc(d->images.strings,d->images.size + sizeof(char *));
}

/*  See if the image string is already in the list.  If it is return its position (1->n, not 1-n-1)
*/
int in_images(PEMF_CALLBACK_DATA d, char *test){
   int i;
   for(i=0; i<d->images.count; i++){
     if(strcmp(test,d->images.strings[i])==0)return(i+1);
   }
   return(0);
}

/*  (Conditionally) add an image.  If a matching image already exists nothing happens.  If one
    does not exist it is added to the images list and also entered into <defs>. 
    
    U_EMRCREATEMONOBRUSH records only work when the bitmap is monochrome.  If we hit one that isn't
      set idx to 2^32-1 and let the caller handle it.
*/
uint32_t add_image(PEMF_CALLBACK_DATA d,  void *pEmr, uint32_t cbBits, uint32_t cbBmi, uint32_t iUsage, uint32_t offBits, uint32_t offBmi){
       
   uint32_t idx;
   char imagename[64]; // big enough
   char xywh[64]; // big enough

   MEMPNG mempng; // PNG in memory comes back in this
   mempng.buffer = NULL;
   
   char *rgba_px=NULL;     // RGBA pixels
   char *px=NULL;          // DIB pixels
   uint32_t width, height, colortype, numCt, invert;
   PU_RGBQUAD ct = NULL;
   if(!cbBits || 
      !cbBmi  ||                                                                                    
      (iUsage != U_DIB_RGB_COLORS) ||                                                               
      !get_DIB_params(  // this returns pointers and values, but allocates no memory                
          pEmr,                                                                                     
          offBits,                                                                                  
          offBmi,                                                                                   
         &px,                                                                                       
         &ct,                                                                                       
         &numCt,                                                                                    
         &width,                                                                                    
         &height,                                                                                   
         &colortype,                                                                                
         &invert                                                                                    
      )){                                                                                           

      // U_EMRCREATEMONOBRUSH uses text/bk colors instead of what is in the color map.              
      if(((PU_EMR)pEmr)->iType == U_EMR_CREATEMONOBRUSH){                                                 
         if(numCt==2){                                                                              
            ct[0] =  U_RGB2BGR(d->dc[d->level].textColor);                                          
            ct[1] =  U_RGB2BGR(d->dc[d->level].bkColor);                                            
         }                                                                                          
         else {  // createmonobrush renders on other platforms this way                             
            return(0xFFFFFFFF);                                                                     
         }                                                                                          
      }                                                                                             

      if(!DIB_to_RGBA(                                                                              
            px,         // DIB pixel array                                                          
            ct,         // DIB color table                                                          
            numCt,      // DIB color table number of entries                                        
            &rgba_px,   // U_RGBA pixel array (32 bits), created by this routine, caller must free. 
            width,      // Width of pixel array                                                     
            height,     // Height of pixel array                                                    
            colortype,  // DIB BitCount Enumeration                                                 
            numCt,      // Color table used if not 0                                                
            invert      // If DIB rows are in opposite order from RGBA rows                         
            ) &&                                                                                    
         rgba_px)                                                                                   
      {                                                                                             
         toPNG(         // Get the image from the RGBA px into mempng                               
             &mempng,                                                                               
             width, height,                                                                         
             rgba_px);                                                                   
         free(rgba_px);                                                                             
      }                                                                                             
   }
   gchar *base64String;
   if(mempng.buffer){
       base64String = g_base64_encode((guchar*) mempng.buffer, mempng.size );
       free(mempng.buffer);
       idx = in_images(d, (char *) base64String);
   }
   else {
       // insert a random 3x4 blotch otherwise
       width  = 3;
       height = 4;
       base64String = strdup("iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAA3NCSVQICAjb4U/gAAAALElEQVQImQXBQQ2AMAAAsUJQMSWI2H8qME1yMshojwrvGB8XcHKvR1XtOTc/8HENumHCsOMAAAAASUVORK5CYII=");
       idx = in_images(d, (char *) base64String);
   }
   if(!idx){  // add it if not already present
      if(d->images.count == d->images.size){  enlarge_images(d); }
      idx = d->images.count;
      d->images.strings[d->images.count++]=strdup(base64String);

      sprintf(imagename,"EMFimage%d",idx++);
      sprintf(xywh," x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" ",width,height); // reuse this buffer

      *(d->defs) += "\n";
      *(d->defs) += "    <image id=\"";
      *(d->defs) += imagename;
      *(d->defs) += "\"\n      ";
      *(d->defs) += xywh;
      *(d->defs) += "\n";
      *(d->defs) += "       xlink:href=\"data:image/png;base64,";
      *(d->defs) += base64String;
      *(d->defs) += "\"\n";
      *(d->defs) += "    />\n";


      *(d->defs) += "\n";
      *(d->defs) += "    <pattern id=\"";
      *(d->defs) += imagename;
      *(d->defs) += "_ref\"\n      ";
      *(d->defs) += xywh;
      *(d->defs) += "\n       patternUnits=\"userSpaceOnUse\"";
      *(d->defs) += " >\n";
      *(d->defs) += "       <use id=\"";
      *(d->defs) += imagename;
      *(d->defs) += "_ign\" ";
      *(d->defs) += " xlink:href=\"#";
      *(d->defs) += imagename;
      *(d->defs) += "\" />\n";
      *(d->defs) += "    </pattern>\n";
   }
   g_free(base64String);
   return(idx-1);
}


static void
output_style(PEMF_CALLBACK_DATA d, int iType)
{
//    SVGOStringStream tmp_id;
    SVGOStringStream tmp_style;
    char tmp[1024] = {0};

    float fill_rgb[3];
    sp_color_get_rgb_floatv( &(d->dc[d->level].style.fill.value.color), fill_rgb );
    float stroke_rgb[3];
    sp_color_get_rgb_floatv(&(d->dc[d->level].style.stroke.value.color), stroke_rgb);
    
    // for U_EMR_BITBLT with no image, try to approximate some of these operations/
    // Assume src color is "white"
    if(d->dwRop3){
       switch(d->dwRop3){
          case U_PATINVERT: // treat all of these as black
          case U_SRCINVERT: 
          case U_DSTINVERT: 
          case U_BLACKNESS:
          case U_SRCERASE: 
          case U_NOTSRCCOPY: 
             fill_rgb[0]=fill_rgb[1]=fill_rgb[2]=0.0; 
             break; 
          case U_SRCCOPY:    // treat all of these as white
          case U_NOTSRCERASE: 
          case U_PATCOPY: 
          case U_WHITENESS: 
             fill_rgb[0]=fill_rgb[1]=fill_rgb[2]=1.0;
             break;
          case U_SRCPAINT:  // use the existing color
          case U_SRCAND: 
          case U_MERGECOPY: 
          case U_MERGEPAINT: 
          case U_PATPAINT:
          default:
             break; 
       }
       d->dwRop3 = 0;  // might as well reset it here, it must be set for each BITBLT
    }

    // Implement some of these, the ones where the original screen color does not matter.
    // The options that merge screen and pen colors cannot be done correctly because we 
    // have no way of knowing what color is already on the screen. For those just pass the
    // pen color through.  
    switch(d->dwRop2){
       case U_R2_BLACK:
             fill_rgb[0]  = fill_rgb[1]  = fill_rgb[2]   = 0.0;
             stroke_rgb[0]= stroke_rgb[1]= stroke_rgb[2] = 0.0;
             break;
       case U_R2_NOTMERGEPEN:
       case U_R2_MASKNOTPEN: 
             break;
       case U_R2_NOTCOPYPEN:
             fill_rgb[0]    =  1.0 - fill_rgb[0];
             fill_rgb[1]    =  1.0 - fill_rgb[1];
             fill_rgb[2]    =  1.0 - fill_rgb[2];
             stroke_rgb[0]  =  1.0 - stroke_rgb[0];
             stroke_rgb[1]  =  1.0 - stroke_rgb[1];
             stroke_rgb[2]  =  1.0 - stroke_rgb[2];
             break;
       case U_R2_MASKPENNOT:
       case U_R2_NOT:
       case U_R2_XORPEN:
       case U_R2_NOTMASKPEN:
       case U_R2_NOTXORPEN:
       case U_R2_NOP:
       case U_R2_MERGENOTPEN:
       case U_R2_COPYPEN:
       case U_R2_MASKPEN:
       case U_R2_MERGEPENNOT:
       case U_R2_MERGEPEN:
             break; 
       case U_R2_WHITE:
             fill_rgb[0]  = fill_rgb[1]  = fill_rgb[2]   = 1.0;
             stroke_rgb[0]= stroke_rgb[1]= stroke_rgb[2] = 1.0;
             break;
       default:
             break;
    }


//    tmp_id << "\n\tid=\"" << (d->id++) << "\"";
//    *(d->outsvg) += tmp_id.str().c_str();
    *(d->outsvg) += "\n\tstyle=\"";
    if (iType == U_EMR_STROKEPATH || !d->dc[d->level].fill_set) {
        tmp_style << "fill:none;";
    } else {
        switch(d->dc[d->level].fill_mode){
            // both of these use the url(#) method
            case DRAW_PATTERN:
               snprintf(tmp, 1023, "fill:url(#%s); ",d->hatches.strings[d->dc[d->level].fill_idx]);
               tmp_style << tmp;
               break;
            case DRAW_IMAGE:
               snprintf(tmp, 1023, "fill:url(#EMFimage%d_ref); ",d->dc[d->level].fill_idx);
               tmp_style << tmp;
               break;
            case DRAW_PAINT:
            default:  // <--  this should never happen, but just in case...
               snprintf(tmp, 1023,
                        "fill:#%02x%02x%02x;",
                        SP_COLOR_F_TO_U(fill_rgb[0]),
                        SP_COLOR_F_TO_U(fill_rgb[1]),
                        SP_COLOR_F_TO_U(fill_rgb[2]));
               tmp_style << tmp;
               break;
        }
        snprintf(tmp, 1023,
                 "fill-rule:%s;",
                 d->dc[d->level].style.fill_rule.value == 0 ? "evenodd" : "nonzero");
        tmp_style << tmp;
        tmp_style << "fill-opacity:1;";

        if (d->dc[d->level].fill_set && d->dc[d->level].stroke_set && d->dc[d->level].style.stroke_width.value == 1 &&
            fill_rgb[0]==stroke_rgb[0] && fill_rgb[1]==stroke_rgb[1] && fill_rgb[2]==stroke_rgb[2])
        {
            d->dc[d->level].stroke_set = false;
        }
    }

    if (iType == U_EMR_FILLPATH || !d->dc[d->level].stroke_set) {
        tmp_style << "stroke:none;";
    } else {
        switch(d->dc[d->level].stroke_mode){
            // both of these use the url(#) method
            case DRAW_PATTERN:
               snprintf(tmp, 1023, "stroke:url(#%s); ",d->hatches.strings[d->dc[d->level].stroke_idx]);
               tmp_style << tmp;
               break;
            case DRAW_IMAGE:
               snprintf(tmp, 1023, "stroke:url(#EMFimage%d_ref); ",d->dc[d->level].stroke_idx);
               tmp_style << tmp;
               break;
            case DRAW_PAINT:
            default:  // <--  this should never happen, but just in case...
               snprintf(tmp, 1023,
                        "stroke:#%02x%02x%02x;",
                        SP_COLOR_F_TO_U(stroke_rgb[0]),
                        SP_COLOR_F_TO_U(stroke_rgb[1]),
                        SP_COLOR_F_TO_U(stroke_rgb[2]));
               tmp_style << tmp;
               break;
        }
        tmp_style << "stroke-width:" <<
            MAX( 0.001, d->dc[d->level].style.stroke_width.value ) << "px;";

        tmp_style << "stroke-linecap:" <<
            (d->dc[d->level].style.stroke_linecap.computed == 0 ? "butt" :
             d->dc[d->level].style.stroke_linecap.computed == 1 ? "round" :
             d->dc[d->level].style.stroke_linecap.computed == 2 ? "square" :
             "unknown") << ";";

        tmp_style << "stroke-linejoin:" <<
            (d->dc[d->level].style.stroke_linejoin.computed == 0 ? "miter" :
             d->dc[d->level].style.stroke_linejoin.computed == 1 ? "round" :
             d->dc[d->level].style.stroke_linejoin.computed == 2 ? "bevel" :
             "unknown") << ";";

        // Set miter limit if known, even if it is not needed immediately (not miter)
        tmp_style << "stroke-miterlimit:" <<
            MAX( 2.0, d->dc[d->level].style.stroke_miterlimit.value ) << ";";

        if (d->dc[d->level].style.stroke_dasharray_set &&
            d->dc[d->level].style.stroke_dash.n_dash && d->dc[d->level].style.stroke_dash.dash)
        {
            tmp_style << "stroke-dasharray:";
            for (int i=0; i<d->dc[d->level].style.stroke_dash.n_dash; i++) {
                if (i)
                    tmp_style << ",";
                tmp_style << d->dc[d->level].style.stroke_dash.dash[i];
            }
            tmp_style << ";";
            tmp_style << "stroke-dashoffset:0;";
        } else {
            tmp_style << "stroke-dasharray:none;";
        }
        tmp_style << "stroke-opacity:1;";
    }
    tmp_style << "\" ";
    if (clipset)
        tmp_style << "\n\tclip-path=\"url(#clipEmfPath" << d->id << ")\" ";
    clipset = false;

    *(d->outsvg) += tmp_style.str().c_str();
}


static double
_pix_x_to_point(PEMF_CALLBACK_DATA d, double px)
{
    double tmp = px - d->dc[d->level].winorg.x;
    tmp *= d->dc[d->level].ScaleInX ? d->dc[d->level].ScaleInX : 1.0;
    tmp += d->dc[d->level].vieworg.x;
    return tmp;
}

static double
_pix_y_to_point(PEMF_CALLBACK_DATA d, double px)
{
    double tmp = px - d->dc[d->level].winorg.y;
    tmp *= d->dc[d->level].ScaleInY ? d->dc[d->level].ScaleInY : 1.0;
    tmp += d->dc[d->level].vieworg.y;
    return tmp;
}


static double
pix_to_x_point(PEMF_CALLBACK_DATA d, double px, double py)
{
    double ppx = _pix_x_to_point(d, px);
    double ppy = _pix_y_to_point(d, py);

    double x = ppx * d->dc[d->level].worldTransform.eM11 + ppy * d->dc[d->level].worldTransform.eM21 + d->dc[d->level].worldTransform.eDx;
    x *= device_scale;
    
    return x;
}

static double
pix_to_y_point(PEMF_CALLBACK_DATA d, double px, double py)
{
    double ppx = _pix_x_to_point(d, px);
    double ppy = _pix_y_to_point(d, py);

    double y = ppx * d->dc[d->level].worldTransform.eM12 + ppy * d->dc[d->level].worldTransform.eM22 + d->dc[d->level].worldTransform.eDy;
    y *= device_scale;
    
    return y;
}

static double
pix_to_size_point(PEMF_CALLBACK_DATA d, double px)
{
    double ppx = px * (d->dc[d->level].ScaleInX ? d->dc[d->level].ScaleInX : 1.0);
    double ppy = 0;

    double dx = ppx * d->dc[d->level].worldTransform.eM11 + ppy * d->dc[d->level].worldTransform.eM21;
    dx *= device_scale;
    double dy = ppx * d->dc[d->level].worldTransform.eM12 + ppy * d->dc[d->level].worldTransform.eM22;
    dy *= device_scale;

    double tmp = sqrt(dx * dx + dy * dy);
    return tmp;
}


static void
select_pen(PEMF_CALLBACK_DATA d, int index)
{
    PU_EMRCREATEPEN pEmr = NULL;

    if (index >= 0 && index < d->n_obj)
        pEmr = (PU_EMRCREATEPEN) d->emf_obj[index].lpEMFR;

    if (!pEmr)
        return;

    switch (pEmr->lopn.lopnStyle & U_PS_STYLE_MASK) {
        case U_PS_DASH:
        case U_PS_DOT:
        case U_PS_DASHDOT:
        case U_PS_DASHDOTDOT:
        {
            int i = 0;
            int penstyle = (pEmr->lopn.lopnStyle & U_PS_STYLE_MASK);
            d->dc[d->level].style.stroke_dash.n_dash =
                penstyle == U_PS_DASHDOTDOT ? 6 : penstyle == U_PS_DASHDOT ? 4 : 2;
            if (d->dc[d->level].style.stroke_dash.dash && (d->level==0 || (d->level>0 && d->dc[d->level].style.stroke_dash.dash!=d->dc[d->level-1].style.stroke_dash.dash)))
                delete[] d->dc[d->level].style.stroke_dash.dash;
            d->dc[d->level].style.stroke_dash.dash = new double[d->dc[d->level].style.stroke_dash.n_dash];
            if (penstyle==U_PS_DASH || penstyle==U_PS_DASHDOT || penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 3;
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
            }
            if (penstyle==U_PS_DOT || penstyle==U_PS_DASHDOT || penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
            }
            if (penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
            }
            
            d->dc[d->level].style.stroke_dasharray_set = 1;
            break;
        }
        
        case U_PS_SOLID:
        default:
        {
            d->dc[d->level].style.stroke_dasharray_set = 0;
            break;
        }
    }

    switch (pEmr->lopn.lopnStyle & U_PS_ENDCAP_MASK) {
        case U_PS_ENDCAP_ROUND:
        {
            d->dc[d->level].style.stroke_linecap.computed = 1;
            break;
        }
        case U_PS_ENDCAP_SQUARE:
        {
            d->dc[d->level].style.stroke_linecap.computed = 2;
            break;
        }
        case U_PS_ENDCAP_FLAT:
        default:
        {
            d->dc[d->level].style.stroke_linecap.computed = 0;
            break;
        }
    }

    switch (pEmr->lopn.lopnStyle & U_PS_JOIN_MASK) {
        case U_PS_JOIN_BEVEL:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 2;
            break;
        }
        case U_PS_JOIN_MITER:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 0;
            break;
        }
        case U_PS_JOIN_ROUND:
        default:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 1;
            break;
        }
    }

    d->dc[d->level].stroke_set = true;

    if (pEmr->lopn.lopnStyle == U_PS_NULL) {
        d->dc[d->level].style.stroke_width.value = 0;
        d->dc[d->level].stroke_set = false;
    } else if (pEmr->lopn.lopnWidth.x) {
        int cur_level = d->level;
        d->level = d->emf_obj[index].level;
        double pen_width = pix_to_size_point( d, pEmr->lopn.lopnWidth.x );
        d->level = cur_level;
        d->dc[d->level].style.stroke_width.value = pen_width;
    } else { // this stroke should always be rendered as 1 pixel wide, independent of zoom level (can that be done in SVG?)
        //d->dc[d->level].style.stroke_width.value = 1.0;
        int cur_level = d->level;
        d->level = d->emf_obj[index].level;
        double pen_width = pix_to_size_point( d, 1 );
        d->level = cur_level;
        d->dc[d->level].style.stroke_width.value = pen_width;
    }

    double r, g, b;
    r = SP_COLOR_U_TO_F( U_RGBAGetR(pEmr->lopn.lopnColor) );
    g = SP_COLOR_U_TO_F( U_RGBAGetG(pEmr->lopn.lopnColor) );
    b = SP_COLOR_U_TO_F( U_RGBAGetB(pEmr->lopn.lopnColor) );
    d->dc[d->level].style.stroke.value.color.set( r, g, b );
}


static void
select_extpen(PEMF_CALLBACK_DATA d, int index)
{
    PU_EMREXTCREATEPEN pEmr = NULL;

    if (index >= 0 && index < d->n_obj)
        pEmr = (PU_EMREXTCREATEPEN) d->emf_obj[index].lpEMFR;

    if (!pEmr)
        return;

    switch (pEmr->elp.elpPenStyle & U_PS_STYLE_MASK) {
        case U_PS_USERSTYLE:
        {
            if (pEmr->elp.elpNumEntries) {
                d->dc[d->level].style.stroke_dash.n_dash = pEmr->elp.elpNumEntries;
                if (d->dc[d->level].style.stroke_dash.dash && (d->level==0 || (d->level>0 && d->dc[d->level].style.stroke_dash.dash!=d->dc[d->level-1].style.stroke_dash.dash)))
                    delete[] d->dc[d->level].style.stroke_dash.dash;
                d->dc[d->level].style.stroke_dash.dash = new double[pEmr->elp.elpNumEntries];
                for (unsigned int i=0; i<pEmr->elp.elpNumEntries; i++) {
                    int cur_level = d->level;
                    d->level = d->emf_obj[index].level;
//  Doing it this way typically results in a pattern that is tiny, better to assume the array
//  is the same scale as for dot/dash below, that is, no scaling should be applied
//                    double dash_length = pix_to_size_point( d, pEmr->elp.elpStyleEntry[i] );
                    double dash_length = pEmr->elp.elpStyleEntry[i];
                    d->level = cur_level;
                    d->dc[d->level].style.stroke_dash.dash[i] = dash_length;
                }
                d->dc[d->level].style.stroke_dasharray_set = 1;
            } else {
                d->dc[d->level].style.stroke_dasharray_set = 0;
            }
            break;
        }

        case U_PS_DASH:
        case U_PS_DOT:
        case U_PS_DASHDOT:
        case U_PS_DASHDOTDOT:
        {
            int i = 0;
            int penstyle = (pEmr->elp.elpPenStyle & U_PS_STYLE_MASK);
            d->dc[d->level].style.stroke_dash.n_dash =
                penstyle == U_PS_DASHDOTDOT ? 6 : penstyle == U_PS_DASHDOT ? 4 : 2;
            if (d->dc[d->level].style.stroke_dash.dash && (d->level==0 || (d->level>0 && d->dc[d->level].style.stroke_dash.dash!=d->dc[d->level-1].style.stroke_dash.dash)))
                delete[] d->dc[d->level].style.stroke_dash.dash;
            d->dc[d->level].style.stroke_dash.dash = new double[d->dc[d->level].style.stroke_dash.n_dash];
            if (penstyle==U_PS_DASH || penstyle==U_PS_DASHDOT || penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 3;
                d->dc[d->level].style.stroke_dash.dash[i++] = 2;
            }
            if (penstyle==U_PS_DOT || penstyle==U_PS_DASHDOT || penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
                d->dc[d->level].style.stroke_dash.dash[i++] = 2;
            }
            if (penstyle==U_PS_DASHDOTDOT) {
                d->dc[d->level].style.stroke_dash.dash[i++] = 1;
                d->dc[d->level].style.stroke_dash.dash[i++] = 2;
            }
            
            d->dc[d->level].style.stroke_dasharray_set = 1;
            break;
        }
        
        case U_PS_SOLID:
        default:
        {
            d->dc[d->level].style.stroke_dasharray_set = 0;
            break;
        }
    }

    switch (pEmr->elp.elpPenStyle & U_PS_ENDCAP_MASK) {
        case U_PS_ENDCAP_ROUND:
        {
            d->dc[d->level].style.stroke_linecap.computed = 1;
            break;
        }
        case U_PS_ENDCAP_SQUARE:
        {
            d->dc[d->level].style.stroke_linecap.computed = 2;
            break;
        }
        case U_PS_ENDCAP_FLAT:
        default:
        {
            d->dc[d->level].style.stroke_linecap.computed = 0;
            break;
        }
    }

    switch (pEmr->elp.elpPenStyle & U_PS_JOIN_MASK) {
        case U_PS_JOIN_BEVEL:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 2;
            break;
        }
        case U_PS_JOIN_MITER:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 0;
            break;
        }
        case U_PS_JOIN_ROUND:
        default:
        {
            d->dc[d->level].style.stroke_linejoin.computed = 1;
            break;
        }
    }

    d->dc[d->level].stroke_set = true;

    if (pEmr->elp.elpPenStyle == U_PS_NULL) {
        d->dc[d->level].style.stroke_width.value = 0;
        d->dc[d->level].stroke_set = false;
    } else if (pEmr->elp.elpWidth) {
        int cur_level = d->level;
        d->level = d->emf_obj[index].level;
        double pen_width = pix_to_size_point( d, pEmr->elp.elpWidth );
        d->level = cur_level;
        d->dc[d->level].style.stroke_width.value = pen_width;
    } else { // this stroke should always be rendered as 1 pixel wide, independent of zoom level (can that be done in SVG?)
        //d->dc[d->level].style.stroke_width.value = 1.0;
        int cur_level = d->level;
        d->level = d->emf_obj[index].level;
        double pen_width = pix_to_size_point( d, 1 );
        d->level = cur_level;
        d->dc[d->level].style.stroke_width.value = pen_width;
    }

    if(     pEmr->elp.elpBrushStyle == U_BS_SOLID){
       double r, g, b;
       r = SP_COLOR_U_TO_F( U_RGBAGetR(pEmr->elp.elpColor) );
       g = SP_COLOR_U_TO_F( U_RGBAGetG(pEmr->elp.elpColor) );
       b = SP_COLOR_U_TO_F( U_RGBAGetB(pEmr->elp.elpColor) );
       d->dc[d->level].style.stroke.value.color.set( r, g, b );
       d->dc[d->level].stroke_mode = DRAW_PAINT;
       d->dc[d->level].stroke_set  = true;
    }
    else if(pEmr->elp.elpBrushStyle == U_BS_HATCHED){
       d->dc[d->level].stroke_idx  = add_hatch(d, pEmr->elp.elpHatch, pEmr->elp.elpColor);
       d->dc[d->level].stroke_mode = DRAW_PATTERN;
       d->dc[d->level].stroke_set  = true;
    }
    else if(pEmr->elp.elpBrushStyle == U_BS_DIBPATTERN || pEmr->elp.elpBrushStyle == U_BS_DIBPATTERNPT){
       d->dc[d->level].stroke_idx  = add_image(d, pEmr, pEmr->cbBits, pEmr->cbBmi, *(uint32_t *) &(pEmr->elp.elpColor), pEmr->offBits, pEmr->offBmi);
       d->dc[d->level].stroke_mode = DRAW_IMAGE;
       d->dc[d->level].stroke_set  = true;
    }
    else { // U_BS_PATTERN and anything strange that falls in, stroke is solid textColor
       double r, g, b;
       r = SP_COLOR_U_TO_F( U_RGBAGetR(d->dc[d->level].textColor));
       g = SP_COLOR_U_TO_F( U_RGBAGetG(d->dc[d->level].textColor));
       b = SP_COLOR_U_TO_F( U_RGBAGetB(d->dc[d->level].textColor));
       d->dc[d->level].style.stroke.value.color.set( r, g, b );
       d->dc[d->level].stroke_mode = DRAW_PAINT;
       d->dc[d->level].stroke_set  = true;
    }
}


static void
select_brush(PEMF_CALLBACK_DATA d, int index)
{
    uint32_t                          tidx;
    uint32_t                          iType;

    if (index >= 0 && index < d->n_obj){
        iType = ((PU_EMR) (d->emf_obj[index].lpEMFR))->iType;
        if(iType == U_EMR_CREATEBRUSHINDIRECT){
           PU_EMRCREATEBRUSHINDIRECT pEmr = (PU_EMRCREATEBRUSHINDIRECT) d->emf_obj[index].lpEMFR;
           if(     pEmr->lb.lbStyle == U_BS_SOLID){
              double r, g, b;
              r = SP_COLOR_U_TO_F( U_RGBAGetR(pEmr->lb.lbColor) );
              g = SP_COLOR_U_TO_F( U_RGBAGetG(pEmr->lb.lbColor) );
              b = SP_COLOR_U_TO_F( U_RGBAGetB(pEmr->lb.lbColor) );
              d->dc[d->level].style.fill.value.color.set( r, g, b );
              d->dc[d->level].fill_mode = DRAW_PAINT;
              d->dc[d->level].fill_set = true;
           }
           else if(pEmr->lb.lbStyle == U_BS_HATCHED){
              d->dc[d->level].fill_idx  = add_hatch(d, pEmr->lb.lbHatch, pEmr->lb.lbColor);
              d->dc[d->level].fill_mode = DRAW_PATTERN;
              d->dc[d->level].fill_set = true;
           }
        }
        else if(iType == U_EMR_CREATEDIBPATTERNBRUSHPT || iType == U_EMR_CREATEMONOBRUSH){
           PU_EMRCREATEDIBPATTERNBRUSHPT pEmr = (PU_EMRCREATEDIBPATTERNBRUSHPT) d->emf_obj[index].lpEMFR;
           tidx = add_image(d, (void *) pEmr, pEmr->cbBits, pEmr->cbBmi, pEmr->iUsage, pEmr->offBits, pEmr->offBmi);
           if(tidx == 0xFFFFFFFF){  // This happens if createmonobrush has a DIB that isn't monochrome
              double r, g, b;
              r = SP_COLOR_U_TO_F( U_RGBAGetR(d->dc[d->level].textColor));
              g = SP_COLOR_U_TO_F( U_RGBAGetG(d->dc[d->level].textColor));
              b = SP_COLOR_U_TO_F( U_RGBAGetB(d->dc[d->level].textColor));
              d->dc[d->level].style.fill.value.color.set( r, g, b );
              d->dc[d->level].fill_mode = DRAW_PAINT;
           }
           else {
              d->dc[d->level].fill_idx  = tidx;
              d->dc[d->level].fill_mode = DRAW_IMAGE;
           }
           d->dc[d->level].fill_set = true;
        }
    }
}


static void
select_font(PEMF_CALLBACK_DATA d, int index)
{
    PU_EMREXTCREATEFONTINDIRECTW pEmr = NULL;

    if (index >= 0 && index < d->n_obj)
        pEmr = (PU_EMREXTCREATEFONTINDIRECTW) d->emf_obj[index].lpEMFR;

    if (!pEmr)return;


    /* The logfont information always starts with a U_LOGFONT structure but the U_EMREXTCREATEFONTINDIRECTW
       is defined as U_LOGFONT_PANOSE so it can handle one of those if that is actually present. Currently only logfont
       is supported, and the remainder, it it really is a U_LOGFONT_PANOSE record, is ignored
    */
    int cur_level = d->level;
    d->level = d->emf_obj[index].level;
    double font_size = pix_to_size_point( d, pEmr->elfw.elfLogFont.lfHeight );
    /* snap the font_size to the nearest .01.  
       See the notes where device_scale is set for the reason why.
       Typically this will set the font to the desired exact size.  If some peculiar size
       was intended this will, at worst, make it 1% off, which is unlikely to be a problem. */
    font_size = round(100.0 * font_size)/100.0;
    d->level = cur_level;
    d->dc[d->level].style.font_size.computed = font_size;
    d->dc[d->level].style.font_weight.value =
        pEmr->elfw.elfLogFont.lfWeight == U_FW_THIN ? SP_CSS_FONT_WEIGHT_100 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_EXTRALIGHT ? SP_CSS_FONT_WEIGHT_200 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_LIGHT ? SP_CSS_FONT_WEIGHT_300 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_NORMAL ? SP_CSS_FONT_WEIGHT_400 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_MEDIUM ? SP_CSS_FONT_WEIGHT_500 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_SEMIBOLD ? SP_CSS_FONT_WEIGHT_600 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_BOLD ? SP_CSS_FONT_WEIGHT_700 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_EXTRABOLD ? SP_CSS_FONT_WEIGHT_800 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_HEAVY ? SP_CSS_FONT_WEIGHT_900 :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_NORMAL ? SP_CSS_FONT_WEIGHT_NORMAL :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_BOLD ? SP_CSS_FONT_WEIGHT_BOLD :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_EXTRALIGHT ? SP_CSS_FONT_WEIGHT_LIGHTER :
        pEmr->elfw.elfLogFont.lfWeight == U_FW_EXTRABOLD ? SP_CSS_FONT_WEIGHT_BOLDER :
        U_FW_NORMAL;
    d->dc[d->level].style.font_style.value = (pEmr->elfw.elfLogFont.lfItalic ? SP_CSS_FONT_STYLE_ITALIC : SP_CSS_FONT_STYLE_NORMAL);
    d->dc[d->level].style.text_decoration.underline = pEmr->elfw.elfLogFont.lfUnderline;
    d->dc[d->level].style.text_decoration.line_through = pEmr->elfw.elfLogFont.lfStrikeOut;
    // malformed  EMF with empty filename may exist, ignore font change if encountered
    char *ctmp = U_Utf16leToUtf8((uint16_t *) (pEmr->elfw.elfLogFont.lfFaceName), U_LF_FACESIZE, NULL);
    if(ctmp){
       if (d->dc[d->level].font_name){ free(d->dc[d->level].font_name); }
       if(*ctmp){ 
          d->dc[d->level].font_name = ctmp;
       }
       else {  // Malformed EMF might specify an empty font name
          free(ctmp);
          d->dc[d->level].font_name = strdup("Arial");  // Default font, EMF spec says device can pick whatever it wants
       }
    }
    d->dc[d->level].style.baseline_shift.value = ((pEmr->elfw.elfLogFont.lfEscapement + 3600) % 3600) / 10;   // use baseline_shift instead of text_transform to avoid overflow
}

static void
delete_object(PEMF_CALLBACK_DATA d, int index)
{
    if (index >= 0 && index < d->n_obj) {
        d->emf_obj[index].type = 0;
// We are keeping a copy of the EMR rather than just a structure.  Currently that is not necessary as the entire
// EMF is read in at once and is stored in a big malloc.  However, in past versions it was handled
// reord by record, and we might need to do that again at some point in the future if we start running into EMF
// files too big to fit into memory.
        if (d->emf_obj[index].lpEMFR)
            free(d->emf_obj[index].lpEMFR);
        d->emf_obj[index].lpEMFR = NULL;
    }
}


static void
insert_object(PEMF_CALLBACK_DATA d, int index, int type, PU_ENHMETARECORD pObj)
{
    if (index >= 0 && index < d->n_obj) {
        delete_object(d, index);
        d->emf_obj[index].type = type;
        d->emf_obj[index].level = d->level;
        d->emf_obj[index].lpEMFR = emr_dup((char *) pObj);
    }
}

/**
  \fn create a UTF-32LE buffer and fill it with UNICODE unknown character
  \param count number of copies of the Unicode unknown character to fill with
*/
uint32_t *unknown_chars(size_t count){
   uint32_t *res = (uint32_t *) malloc(sizeof(uint32_t) * (count + 1));
   if(!res)throw "Inkscape fatal memory allocation error - cannot continue";
   for(uint32_t i=0; i<count; i++){ res[i] = 0xFFFD; }
   res[count]=0;
   return res;
}

void common_image_extraction(PEMF_CALLBACK_DATA d, void *pEmr, double l, double t, double r, double b, 
  uint32_t iUsage, uint32_t offBits, uint32_t cbBits, uint32_t offBmi, uint32_t cbBmi){
            SVGOStringStream tmp_image;
            tmp_image << " y=\"" << t << "\"\n x=\"" << l <<"\"\n ";

            // The image ID is filled in much later when tmp_image is converted

            tmp_image << " xlink:href=\"data:image/png;base64,";
           
            MEMPNG mempng; // PNG in memory comes back in this
            mempng.buffer = NULL;
           
            char *rgba_px=NULL;     // RGBA pixels
            char *px=NULL;          // DIB pixels
            uint32_t width, height, colortype, numCt, invert;
            PU_RGBQUAD ct = NULL;
            if(!cbBits || 
               !cbBmi  || 
               (iUsage != U_DIB_RGB_COLORS) || 
               !get_DIB_params(  // this returns pointers and values, but allocates no memory
                   pEmr,
                   offBits,
                   offBmi,
                  &px,
                  &ct,
                  &numCt,
                  &width,
                  &height,
                  &colortype,
                  &invert
               )){

               if(!DIB_to_RGBA(
                     px,         // DIB pixel array
                     ct,         // DIB color table
                     numCt,      // DIB color table number of entries
                     &rgba_px,   // U_RGBA pixel array (32 bits), created by this routine, caller must free.
                     width,      // Width of pixel array
                     height,     // Height of pixel array
                     colortype,  // DIB BitCount Enumeration
                     numCt,      // Color table used if not 0
                     invert      // If DIB rows are in opposite order from RGBA rows
                     ) && 
                  rgba_px)
               {
                  toPNG(         // Get the image from the RGBA px into mempng
                      &mempng,
                      width, height,
                      rgba_px);
                  free(rgba_px);
               }
            }
            if(mempng.buffer){
                gchar *base64String = g_base64_encode((guchar*) mempng.buffer, mempng.size );
                free(mempng.buffer);
                tmp_image << base64String ;
                g_free(base64String);
            }
            else {
              // insert a random 3x4 blotch otherwise
              tmp_image << "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAA3NCSVQICAjb4U/gAAAALElEQVQImQXBQQ2AMAAAsUJQMSWI2H8qME1yMshojwrvGB8XcHKvR1XtOTc/8HENumHCsOMAAAAASUVORK5CYII=";
            }
               
            tmp_image << "\"\n height=\"" << b-t+1 << "\"\n width=\"" << r-l+1 << "\"\n";

            *(d->outsvg) += "\n\t <image\n";
            *(d->outsvg) += tmp_image.str().c_str();
            *(d->outsvg) += "/> \n";
            *(d->path) = "";
}

/**
  \fn myEnhMetaFileProc(char *contents, unsigned int length, PEMF_CALLBACK_DATA lpData)
  \param contents binary contents of an EMF file
  \param length   length in bytes of contents
  \param d   Inkscape data structures returned by this call
*/
//THis was a callback, just build it into a normal function
int myEnhMetaFileProc(char *contents, unsigned int length, PEMF_CALLBACK_DATA d)
{
    uint32_t off=0;
    uint32_t emr_mask;
    int      OK =1;
    PU_ENHMETARECORD lpEMFR;
    
    while(OK){
    if(off>=length)return(0);  //normally should exit from while after EMREOF sets OK to false.  

    lpEMFR = (PU_ENHMETARECORD)(contents + off);
//  Uncomment the following to track down toxic records
//std::cout << "record type: " << lpEMFR->iType << " length: " << lpEMFR->nSize << " offset: " << off <<std::endl;
    off += lpEMFR->nSize;
 
    SVGOStringStream tmp_outsvg;
    SVGOStringStream tmp_path;
    SVGOStringStream tmp_str;
    SVGOStringStream dbg_str;
    
    emr_mask = emr_properties(lpEMFR->iType); 
    if(emr_mask == U_EMR_INVALID){ throw "Inkscape fatal memory allocation error - cannot continue"; }

// std::cout << "BEFORE DRAW logic d->mask: " << std::hex << d->mask << " emr_mask: " << emr_mask << std::dec << std::endl;
/*
std::cout << "BEFORE DRAW"
 << " test0 " << ( d->mask & U_DRAW_VISIBLE) 
 << " test1 " << ( d->mask & U_DRAW_FORCE) 
 << " test2 " << (emr_mask & U_DRAW_ALTERS) 
 << " test3 " << (emr_mask & U_DRAW_VISIBLE)
 << " test4 " << !(d->mask & U_DRAW_ONLYTO)
 << " test5 " << ((d->mask & U_DRAW_ONLYTO) && !(emr_mask & U_DRAW_ONLYTO)  )
 << std::endl;
*/
    if ( (emr_mask != 0xFFFFFFFF)   &&                                           // next record is valid type
         (d->mask & U_DRAW_VISIBLE) &&                                           // This record is drawable
         (  (d->mask & U_DRAW_FORCE)   ||                                        // This draw is forced by STROKE/FILL/STROKEANDFILL PATH
            (emr_mask & U_DRAW_ALTERS) ||                                        // Next record would alter the drawing environment in some way
            (  (emr_mask & U_DRAW_VISIBLE)                                       // Next record is visible...
                &&
               (
                 ( !(d->mask & U_DRAW_ONLYTO) )                                  //   Non *TO records cannot be followed by any Visible
                 ||
                 ((d->mask & U_DRAW_ONLYTO) && !(emr_mask & U_DRAW_ONLYTO)  )    //   *TO records can only be followed by other *TO records
               )                                     
            )
         )
       ){
// std::cout << "PATH DRAW at TOP" << std::endl;
            *(d->outsvg) += "    <path ";    // this is the ONLY place <path should be used!!!!
            if(d->drawtype){                 // explicit draw type EMR record
               output_style(d, d->drawtype);
            }
            else if(d->mask & U_DRAW_CLOSED){              // implicit draw type
               output_style(d, U_EMR_STROKEANDFILLPATH);
            }
            else {
               output_style(d, U_EMR_STROKEPATH);
            }
            *(d->outsvg) += "\n\t";
            *(d->outsvg) += "\n\td=\"";      // this is the ONLY place d=" should be used!!!!
            *(d->outsvg) += *(d->path);
            *(d->outsvg) += " \" /> \n";
            *(d->path) = "";
            // reset the flags
            d->mask = 0;
            d->drawtype = 0;
    }
// std::cout << "AFTER DRAW logic d->mask: " << std::hex << d->mask << " emr_mask: " << emr_mask << std::dec << std::endl;

    switch (lpEMFR->iType)
    {
        case U_EMR_HEADER:
        {
            dbg_str << "<!-- U_EMR_HEADER -->\n";

            *(d->outdef) += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";

            if (d->pDesc) {
                *(d->outdef) += "<!-- ";
                *(d->outdef) += d->pDesc;
                *(d->outdef) += " -->\n";
            }

            PU_EMRHEADER pEmr = (PU_EMRHEADER) lpEMFR;
            SVGOStringStream tmp_outdef;
            tmp_outdef << "<svg\n";
            tmp_outdef << "  xmlns:svg=\"http://www.w3.org/2000/svg\"\n";
            tmp_outdef << "  xmlns=\"http://www.w3.org/2000/svg\"\n";
            tmp_outdef << "  xmlns:xlink=\"http://www.w3.org/1999/xlink\"\n";
            tmp_outdef << "  xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\"\n"; // needed for sodipodi:role
            tmp_outdef << "  version=\"1.0\"\n";

            d->xDPI = 2540;
            d->yDPI = 2540;

            d->dc[d->level].PixelsInX = pEmr->rclFrame.right;  // - pEmr->rclFrame.left;
            d->dc[d->level].PixelsInY = pEmr->rclFrame.bottom; // - pEmr->rclFrame.top;

            d->MMX = d->dc[d->level].PixelsInX / 100.0;
            d->MMY = d->dc[d->level].PixelsInY / 100.0;

            d->dc[d->level].PixelsOutX = d->MMX * PX_PER_MM;
            d->dc[d->level].PixelsOutY = d->MMY * PX_PER_MM;

            /* 
               calculate ratio of Inkscape dpi/device dpi
               This can cause problems later due to accuracy limits in the EMF.  A super high resolution
               EMF might have a final device_scale of 0.074998, and adjusting the (integer) device size
               by 1 will still not get it exactly to 0.075.  Later when the font size is calculated it
               can end up as 29.9992 or 22.4994 instead of the intended 30 or 22.5.  This is handled by
               snapping font sizes to the nearest .01.
            */
            if (pEmr->szlMillimeters.cx && pEmr->szlDevice.cx)
                device_scale = PX_PER_MM*pEmr->szlMillimeters.cx/pEmr->szlDevice.cx;
            
            tmp_outdef <<
                "  width=\"" << d->MMX << "mm\"\n" <<
                "  height=\"" << d->MMY << "mm\">\n";
            *(d->outdef) += tmp_outdef.str().c_str();
            *(d->outdef) += "<defs>";                           // temporary end of header

            // d->defs holds any defines which are read in.

            tmp_outsvg << "\n</defs>\n<g>\n";                   // start of main body

            if (pEmr->nHandles) {
                d->n_obj = pEmr->nHandles;
                d->emf_obj = new EMF_OBJECT[d->n_obj];
                
                // Init the new emf_obj list elements to null, provided the
                // dynamic allocation succeeded.
                if ( d->emf_obj != NULL )
                {
                    for( int i=0; i < d->n_obj; ++i )
                        d->emf_obj[i].lpEMFR = NULL;
                } //if

            } else {
                d->emf_obj = NULL;
            }

            break;
        }
        case U_EMR_POLYBEZIER:
        {
            dbg_str << "<!-- U_EMR_POLYBEZIER -->\n";

            PU_EMRPOLYBEZIER pEmr = (PU_EMRPOLYBEZIER) lpEMFR;
            uint32_t i,j;

            if (pEmr->cptl<4)
                break;

            d->mask |= emr_mask;

            tmp_str <<
                "\n\tM " <<
                pix_to_x_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y ) << " " <<
                pix_to_y_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y) << " ";

            for (i=1; i<pEmr->cptl; ) {
                tmp_str << "\n\tC ";
                for (j=0; j<3 && i<pEmr->cptl; j++,i++) {
                    tmp_str <<
                        pix_to_x_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " " <<
                        pix_to_y_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " ";
                }
            }

            tmp_path << tmp_str.str().c_str();

            break;
        }
        case U_EMR_POLYGON:
        {
            dbg_str << "<!-- U_EMR_POLYGON -->\n";

            PU_EMRPOLYGON pEmr = (PU_EMRPOLYGON) lpEMFR;
            uint32_t i;

            if (pEmr->cptl < 2)
                break;

            d->mask |= emr_mask;

            tmp_str <<
                "\n\tM " <<
                pix_to_x_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y ) << " " <<
                pix_to_y_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y ) << " ";

            for (i=1; i<pEmr->cptl; i++) {
                tmp_str <<
                    "\n\tL " <<
                    pix_to_x_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " " <<
                    pix_to_y_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " ";
            }

            tmp_path << tmp_str.str().c_str();
            tmp_path << " z";

            break;
        }
        case U_EMR_POLYLINE:
        {
            dbg_str << "<!-- U_EMR_POLYLINE -->\n";

            PU_EMRPOLYLINE pEmr = (PU_EMRPOLYLINE) lpEMFR;
            uint32_t i;

            if (pEmr->cptl<2)
                break;

            d->mask |= emr_mask;

            tmp_str <<
                "\n\tM " <<
                pix_to_x_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y ) << " " <<
                pix_to_y_point( d, pEmr->aptl[0].x, pEmr->aptl[0].y ) << " ";

            for (i=1; i<pEmr->cptl; i++) {
                tmp_str <<
                    "\n\tL " <<
                    pix_to_x_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " " <<
                    pix_to_y_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " ";
            }

            tmp_path << tmp_str.str().c_str();
 
            break;
        }
        case U_EMR_POLYBEZIERTO:
        {
            dbg_str << "<!-- U_EMR_POLYBEZIERTO -->\n";

            PU_EMRPOLYBEZIERTO pEmr = (PU_EMRPOLYBEZIERTO) lpEMFR;
            uint32_t i,j;

            d->mask |= emr_mask;

            for (i=0; i<pEmr->cptl;) {
                tmp_path << "\n\tC ";
                for (j=0; j<3 && i<pEmr->cptl; j++,i++) {
                    tmp_path <<
                        pix_to_x_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " " <<
                        pix_to_y_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " ";
                }
            }

            break;
        }
        case U_EMR_POLYLINETO:
        {
            dbg_str << "<!-- U_EMR_POLYLINETO -->\n";

            PU_EMRPOLYLINETO pEmr = (PU_EMRPOLYLINETO) lpEMFR;
            uint32_t i;

            d->mask |= emr_mask;

            for (i=0; i<pEmr->cptl;i++) {
                tmp_path <<
                    "\n\tL " <<
                    pix_to_x_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " " <<
                    pix_to_y_point( d, pEmr->aptl[i].x, pEmr->aptl[i].y ) << " ";
            }

            break;
        }
        case U_EMR_POLYPOLYLINE:
        case U_EMR_POLYPOLYGON:
        {
            if (lpEMFR->iType == U_EMR_POLYPOLYLINE)
                dbg_str << "<!-- U_EMR_POLYPOLYLINE -->\n";
            if (lpEMFR->iType == U_EMR_POLYPOLYGON)
                dbg_str << "<!-- U_EMR_POLYPOLYGON -->\n";

            PU_EMRPOLYPOLYGON pEmr = (PU_EMRPOLYPOLYGON) lpEMFR;
            unsigned int n, i, j;

            d->mask |= emr_mask;

            U_POINTL *aptl = (PU_POINTL) &pEmr->aPolyCounts[pEmr->nPolys];

            i = 0;
            for (n=0; n<pEmr->nPolys && i<pEmr->cptl; n++) {
                SVGOStringStream poly_path;

                poly_path << "\n\tM " <<
                    pix_to_x_point( d, aptl[i].x, aptl[i].y ) << " " <<
                    pix_to_y_point( d, aptl[i].x, aptl[i].y ) << " ";
                i++;

                for (j=1; j<pEmr->aPolyCounts[n] && i<pEmr->cptl; j++) {
                    poly_path << "\n\tL " <<
                        pix_to_x_point( d, aptl[i].x, aptl[i].y ) << " " <<
                        pix_to_y_point( d, aptl[i].x, aptl[i].y ) << " ";
                    i++;
                }

                tmp_str << poly_path.str().c_str();
                if (lpEMFR->iType == U_EMR_POLYPOLYGON)
                    tmp_str << " z";
                tmp_str << " \n";
            }

            tmp_path << tmp_str.str().c_str();

            break;
        }
        case U_EMR_SETWINDOWEXTEX:
        {
            dbg_str << "<!-- U_EMR_SETWINDOWEXTEX -->\n";

            PU_EMRSETWINDOWEXTEX pEmr = (PU_EMRSETWINDOWEXTEX) lpEMFR;

            d->dc[d->level].sizeWnd = pEmr->szlExtent;

            if (!d->dc[d->level].sizeWnd.cx || !d->dc[d->level].sizeWnd.cy) {
                d->dc[d->level].sizeWnd = d->dc[d->level].sizeView;
                if (!d->dc[d->level].sizeWnd.cx || !d->dc[d->level].sizeWnd.cy) {
                    d->dc[d->level].sizeWnd.cx = d->dc[d->level].PixelsOutX;
                    d->dc[d->level].sizeWnd.cy = d->dc[d->level].PixelsOutY;
                }
            }

            if (!d->dc[d->level].sizeView.cx || !d->dc[d->level].sizeView.cy) {
                d->dc[d->level].sizeView = d->dc[d->level].sizeWnd;
            }

            d->dc[d->level].PixelsInX = d->dc[d->level].sizeWnd.cx;
            d->dc[d->level].PixelsInY = d->dc[d->level].sizeWnd.cy;
            
            if (d->dc[d->level].PixelsInX && d->dc[d->level].PixelsInY) {
                d->dc[d->level].ScaleInX = (double) d->dc[d->level].sizeView.cx / (double) d->dc[d->level].PixelsInX;
                d->dc[d->level].ScaleInY = (double) d->dc[d->level].sizeView.cy / (double) d->dc[d->level].PixelsInY;
            }
            else {
                d->dc[d->level].ScaleInX = 1;
                d->dc[d->level].ScaleInY = 1;
            }

            break;
        }
        case U_EMR_SETWINDOWORGEX:
        {
            dbg_str << "<!-- U_EMR_SETWINDOWORGEX -->\n";

            PU_EMRSETWINDOWORGEX pEmr = (PU_EMRSETWINDOWORGEX) lpEMFR;
            d->dc[d->level].winorg = pEmr->ptlOrigin;
            break;
        }
        case U_EMR_SETVIEWPORTEXTEX:
        {
            dbg_str << "<!-- U_EMR_SETVIEWPORTEXTEX -->\n";

            PU_EMRSETVIEWPORTEXTEX pEmr = (PU_EMRSETVIEWPORTEXTEX) lpEMFR;

            d->dc[d->level].sizeView = pEmr->szlExtent;

            if (!d->dc[d->level].sizeView.cx || !d->dc[d->level].sizeView.cy) {
                d->dc[d->level].sizeView = d->dc[d->level].sizeWnd;
                if (!d->dc[d->level].sizeView.cx || !d->dc[d->level].sizeView.cy) {
                    d->dc[d->level].sizeView.cx = d->dc[d->level].PixelsOutX;
                    d->dc[d->level].sizeView.cy = d->dc[d->level].PixelsOutY;
                }
            }

            if (!d->dc[d->level].sizeWnd.cx || !d->dc[d->level].sizeWnd.cy) {
                d->dc[d->level].sizeWnd = d->dc[d->level].sizeView;
            }

            d->dc[d->level].PixelsInX = d->dc[d->level].sizeWnd.cx;
            d->dc[d->level].PixelsInY = d->dc[d->level].sizeWnd.cy;
            
            if (d->dc[d->level].PixelsInX && d->dc[d->level].PixelsInY) {
                d->dc[d->level].ScaleInX = (double) d->dc[d->level].sizeView.cx / (double) d->dc[d->level].PixelsInX;
                d->dc[d->level].ScaleInY = (double) d->dc[d->level].sizeView.cy / (double) d->dc[d->level].PixelsInY;
            }
            else {
                d->dc[d->level].ScaleInX = 1;
                d->dc[d->level].ScaleInY = 1;
            }

            break;
        }
        case U_EMR_SETVIEWPORTORGEX:
        {
            dbg_str << "<!-- U_EMR_SETVIEWPORTORGEX -->\n";

            PU_EMRSETVIEWPORTORGEX pEmr = (PU_EMRSETVIEWPORTORGEX) lpEMFR;
            d->dc[d->level].vieworg = pEmr->ptlOrigin;
            break;
        }
        case U_EMR_SETBRUSHORGEX:        dbg_str << "<!-- U_EMR_SETBRUSHORGEX -->\n";      break;
        case U_EMR_EOF:
        {
            dbg_str << "<!-- U_EMR_EOF -->\n";

            tmp_outsvg << "</g>\n";
            tmp_outsvg << "</svg>\n";
            *(d->outsvg) = *(d->outdef) + *(d->defs) + *(d->outsvg);
            OK=0;
            break;
        }
        case U_EMR_SETPIXELV:            dbg_str << "<!-- U_EMR_SETPIXELV -->\n";          break;
        case U_EMR_SETMAPPERFLAGS:       dbg_str << "<!-- U_EMR_SETMAPPERFLAGS -->\n";     break;
        case U_EMR_SETMAPMODE:           dbg_str << "<!-- U_EMR_SETMAPMODE -->\n";         break;
        case U_EMR_SETBKMODE:            dbg_str << "<!-- U_EMR_SETBKMODE -->\n";          break;
        case U_EMR_SETPOLYFILLMODE:
        {
            dbg_str << "<!-- U_EMR_SETPOLYFILLMODE -->\n";

            PU_EMRSETPOLYFILLMODE pEmr = (PU_EMRSETPOLYFILLMODE) lpEMFR;
            d->dc[d->level].style.fill_rule.value =
                (pEmr->iMode == U_ALTERNATE ? 0 :
                 pEmr->iMode == U_WINDING ? 1 : 0);
            break;
        }
        case U_EMR_SETROP2:
        {
            dbg_str << "<!-- U_EMR_SETROP2 -->\n";
            PU_EMRSETROP2 pEmr = (PU_EMRSETROP2) lpEMFR;
            d->dwRop2 = pEmr->iMode;
            break;
        }
        case U_EMR_SETSTRETCHBLTMODE:
        {
            PU_EMRSETSTRETCHBLTMODE pEmr = (PU_EMRSETSTRETCHBLTMODE) lpEMFR; // from wingdi.h
            BLTmode = pEmr->iMode;
            dbg_str << "<!-- U_EMR_SETSTRETCHBLTMODE -->\n";
            break;
        }
        case U_EMR_SETTEXTALIGN:
        {
            dbg_str << "<!-- U_EMR_SETTEXTALIGN -->\n";

            PU_EMRSETTEXTALIGN pEmr = (PU_EMRSETTEXTALIGN) lpEMFR;
            d->dc[d->level].textAlign = pEmr->iMode;
            break;
        }
        case U_EMR_SETCOLORADJUSTMENT:
            dbg_str << "<!-- U_EMR_SETCOLORADJUSTMENT -->\n";
            break;
        case U_EMR_SETTEXTCOLOR:
        {
            dbg_str << "<!-- U_EMR_SETTEXTCOLOR -->\n";

            PU_EMRSETTEXTCOLOR pEmr = (PU_EMRSETTEXTCOLOR) lpEMFR;
            d->dc[d->level].textColor = pEmr->crColor;
            d->dc[d->level].textColorSet = true;
            break;
        }
        case U_EMR_SETBKCOLOR:
        {
            dbg_str << "<!-- U_EMR_SETBKCOLOR -->\n";

            PU_EMRSETBKCOLOR pEmr = (PU_EMRSETBKCOLOR) lpEMFR;
            d->dc[d->level].bkColor = pEmr->crColor;
            d->dc[d->level].bkColorSet = true;
            break;
        }
        case U_EMR_OFFSETCLIPRGN:        dbg_str << "<!-- U_EMR_OFFSETCLIPRGN -->\n";      break;
        case U_EMR_MOVETOEX:
        {
            dbg_str << "<!-- U_EMR_MOVETOEX -->\n";

            PU_EMRMOVETOEX pEmr = (PU_EMRMOVETOEX) lpEMFR;

            d->mask |= emr_mask;

            d->dc[d->level].cur = pEmr->ptl;

            tmp_path <<
                "\n\tM " <<
                pix_to_x_point( d, pEmr->ptl.x, pEmr->ptl.y ) << " " <<
                pix_to_y_point( d, pEmr->ptl.x, pEmr->ptl.y ) << " ";
            break;
        }
        case U_EMR_SETMETARGN:           dbg_str << "<!-- U_EMR_SETMETARGN -->\n";         break;
        case U_EMR_EXCLUDECLIPRECT:      dbg_str << "<!-- U_EMR_EXCLUDECLIPRECT -->\n";    break;
        case U_EMR_INTERSECTCLIPRECT:
        {
            dbg_str << "<!-- U_EMR_INTERSECTCLIPRECT -->\n";

            PU_EMRINTERSECTCLIPRECT pEmr = (PU_EMRINTERSECTCLIPRECT) lpEMFR;
            U_RECTL rc = pEmr->rclClip;
            clipset = true;
            if ((rc.left == rc_old.left) && (rc.top == rc_old.top) && (rc.right == rc_old.right) && (rc.bottom == rc_old.bottom))
                break;
            rc_old = rc;

            double l = pix_to_x_point( d, rc.left, rc.top );
            double t = pix_to_y_point( d, rc.left, rc.top );
            double r = pix_to_x_point( d, rc.right, rc.bottom );
            double b = pix_to_y_point( d, rc.right, rc.bottom );

            SVGOStringStream tmp_rectangle;
            tmp_rectangle << "\n<clipPath\n\tclipPathUnits=\"userSpaceOnUse\" ";
            tmp_rectangle << "\n\tid=\"clipEmfPath" << ++(d->id) << "\" >";
            tmp_rectangle << "\n<rect ";
            tmp_rectangle << "\n\tx=\"" << l << "\" ";
            tmp_rectangle << "\n\ty=\"" << t << "\" ";
            tmp_rectangle << "\n\twidth=\"" << r-l << "\" ";
            tmp_rectangle << "\n\theight=\"" << b-t << "\" />";
            tmp_rectangle << "\n</clipPath>";

            *(d->outdef) += tmp_rectangle.str().c_str();
            *(d->path) = "";
            break;
        }
        case U_EMR_SCALEVIEWPORTEXTEX:   dbg_str << "<!-- U_EMR_SCALEVIEWPORTEXTEX -->\n"; break;
        case U_EMR_SCALEWINDOWEXTEX:     dbg_str << "<!-- U_EMR_SCALEWINDOWEXTEX -->\n";   break;
        case U_EMR_SAVEDC:
            dbg_str << "<!-- U_EMR_SAVEDC -->\n";

            if (d->level < EMF_MAX_DC) {
                d->dc[d->level + 1] = d->dc[d->level];
                if(d->dc[d->level].font_name){
                  d->dc[d->level + 1].font_name = strdup(d->dc[d->level].font_name); // or memory access problems because font name pointer duplicated
                }
                d->level = d->level + 1;
            }
            break;
        case U_EMR_RESTOREDC:
        {
            dbg_str << "<!-- U_EMR_RESTOREDC -->\n";
            
            PU_EMRRESTOREDC pEmr = (PU_EMRRESTOREDC) lpEMFR;
            int old_level = d->level;
            if (pEmr->iRelative >= 0) {
                if (pEmr->iRelative < d->level)
                    d->level = pEmr->iRelative;
            }
            else {
                if (d->level + pEmr->iRelative >= 0)
                    d->level = d->level + pEmr->iRelative;
            }
            while (old_level > d->level) {
                if (d->dc[old_level].style.stroke_dash.dash && (old_level==0 || (old_level>0 && d->dc[old_level].style.stroke_dash.dash!=d->dc[old_level-1].style.stroke_dash.dash))){
                    delete[] d->dc[old_level].style.stroke_dash.dash;
                }
                if(d->dc[old_level].font_name){
                   free(d->dc[old_level].font_name); // else memory leak
                   d->dc[old_level].font_name = NULL;
                }
                old_level--;
            }
            break;
        }
        case U_EMR_SETWORLDTRANSFORM:
        {
            dbg_str << "<!-- U_EMR_SETWORLDTRANSFORM -->\n";

            PU_EMRSETWORLDTRANSFORM pEmr = (PU_EMRSETWORLDTRANSFORM) lpEMFR;
            d->dc[d->level].worldTransform = pEmr->xform;
            break;
        }
        case U_EMR_MODIFYWORLDTRANSFORM:
        {
            dbg_str << "<!-- U_EMR_MODIFYWORLDTRANSFORM -->\n";

            PU_EMRMODIFYWORLDTRANSFORM pEmr = (PU_EMRMODIFYWORLDTRANSFORM) lpEMFR;
            switch (pEmr->iMode)
            {
                case U_MWT_IDENTITY:
                    d->dc[d->level].worldTransform.eM11 = 1.0;
                    d->dc[d->level].worldTransform.eM12 = 0.0;
                    d->dc[d->level].worldTransform.eM21 = 0.0;
                    d->dc[d->level].worldTransform.eM22 = 1.0;
                    d->dc[d->level].worldTransform.eDx  = 0.0;
                    d->dc[d->level].worldTransform.eDy  = 0.0;
                    break;
                case U_MWT_LEFTMULTIPLY:
                {
//                    d->dc[d->level].worldTransform = pEmr->xform * worldTransform;

                    float a11 = pEmr->xform.eM11;
                    float a12 = pEmr->xform.eM12;
                    float a13 = 0.0;
                    float a21 = pEmr->xform.eM21;
                    float a22 = pEmr->xform.eM22;
                    float a23 = 0.0;
                    float a31 = pEmr->xform.eDx;
                    float a32 = pEmr->xform.eDy;
                    float a33 = 1.0;

                    float b11 = d->dc[d->level].worldTransform.eM11;
                    float b12 = d->dc[d->level].worldTransform.eM12;
                    //float b13 = 0.0;
                    float b21 = d->dc[d->level].worldTransform.eM21;
                    float b22 = d->dc[d->level].worldTransform.eM22;
                    //float b23 = 0.0;
                    float b31 = d->dc[d->level].worldTransform.eDx;
                    float b32 = d->dc[d->level].worldTransform.eDy;
                    //float b33 = 1.0;

                    float c11 = a11*b11 + a12*b21 + a13*b31;;
                    float c12 = a11*b12 + a12*b22 + a13*b32;;
                    //float c13 = a11*b13 + a12*b23 + a13*b33;;
                    float c21 = a21*b11 + a22*b21 + a23*b31;;
                    float c22 = a21*b12 + a22*b22 + a23*b32;;
                    //float c23 = a21*b13 + a22*b23 + a23*b33;;
                    float c31 = a31*b11 + a32*b21 + a33*b31;;
                    float c32 = a31*b12 + a32*b22 + a33*b32;;
                    //float c33 = a31*b13 + a32*b23 + a33*b33;;

                    d->dc[d->level].worldTransform.eM11 = c11;;
                    d->dc[d->level].worldTransform.eM12 = c12;;
                    d->dc[d->level].worldTransform.eM21 = c21;;
                    d->dc[d->level].worldTransform.eM22 = c22;;
                    d->dc[d->level].worldTransform.eDx = c31;
                    d->dc[d->level].worldTransform.eDy = c32;
                    
                    break;
                }
                case U_MWT_RIGHTMULTIPLY:
                {
//                    d->dc[d->level].worldTransform = worldTransform * pEmr->xform;

                    float a11 = d->dc[d->level].worldTransform.eM11;
                    float a12 = d->dc[d->level].worldTransform.eM12;
                    float a13 = 0.0;
                    float a21 = d->dc[d->level].worldTransform.eM21;
                    float a22 = d->dc[d->level].worldTransform.eM22;
                    float a23 = 0.0;
                    float a31 = d->dc[d->level].worldTransform.eDx;
                    float a32 = d->dc[d->level].worldTransform.eDy;
                    float a33 = 1.0;

                    float b11 = pEmr->xform.eM11;
                    float b12 = pEmr->xform.eM12;
                    //float b13 = 0.0;
                    float b21 = pEmr->xform.eM21;
                    float b22 = pEmr->xform.eM22;
                    //float b23 = 0.0;
                    float b31 = pEmr->xform.eDx;
                    float b32 = pEmr->xform.eDy;
                    //float b33 = 1.0;

                    float c11 = a11*b11 + a12*b21 + a13*b31;;
                    float c12 = a11*b12 + a12*b22 + a13*b32;;
                    //float c13 = a11*b13 + a12*b23 + a13*b33;;
                    float c21 = a21*b11 + a22*b21 + a23*b31;;
                    float c22 = a21*b12 + a22*b22 + a23*b32;;
                    //float c23 = a21*b13 + a22*b23 + a23*b33;;
                    float c31 = a31*b11 + a32*b21 + a33*b31;;
                    float c32 = a31*b12 + a32*b22 + a33*b32;;
                    //float c33 = a31*b13 + a32*b23 + a33*b33;;

                    d->dc[d->level].worldTransform.eM11 = c11;;
                    d->dc[d->level].worldTransform.eM12 = c12;;
                    d->dc[d->level].worldTransform.eM21 = c21;;
                    d->dc[d->level].worldTransform.eM22 = c22;;
                    d->dc[d->level].worldTransform.eDx = c31;
                    d->dc[d->level].worldTransform.eDy = c32;

                    break;
                }
//                case MWT_SET:
                default:
                    d->dc[d->level].worldTransform = pEmr->xform;
                    break;
            }
            break;
        }
        case U_EMR_SELECTOBJECT:
        {
            dbg_str << "<!-- U_EMR_SELECTOBJECT -->\n";

            PU_EMRSELECTOBJECT pEmr = (PU_EMRSELECTOBJECT) lpEMFR;
            unsigned int index = pEmr->ihObject;

            if (index & U_STOCK_OBJECT) {
                switch (index) {
                    case U_NULL_BRUSH:
                        d->dc[d->level].fill_mode = DRAW_PAINT;
                        d->dc[d->level].fill_set = false;
                        break;
                    case U_BLACK_BRUSH:
                    case U_DKGRAY_BRUSH:
                    case U_GRAY_BRUSH:
                    case U_LTGRAY_BRUSH:
                    case U_WHITE_BRUSH:
                    {
                        float val = 0;
                        switch (index) {
                            case U_BLACK_BRUSH:
                                val = 0.0 / 255.0;
                                break;
                            case U_DKGRAY_BRUSH:
                                val = 64.0 / 255.0;
                                break;
                            case U_GRAY_BRUSH:
                                val = 128.0 / 255.0;
                                break;
                            case U_LTGRAY_BRUSH:
                                val = 192.0 / 255.0;
                                break;
                            case U_WHITE_BRUSH:
                                val = 255.0 / 255.0;
                                break;
                        }
                        d->dc[d->level].style.fill.value.color.set( val, val, val );

                        d->dc[d->level].fill_mode = DRAW_PAINT;
                        d->dc[d->level].fill_set = true;
                        break;
                    }
                    case U_NULL_PEN:
                        d->dc[d->level].stroke_mode = DRAW_PAINT;
                        d->dc[d->level].stroke_set = false;
                        break;
                    case U_BLACK_PEN:
                    case U_WHITE_PEN:
                    {
                        float val = index == U_BLACK_PEN ? 0 : 1;
                        d->dc[d->level].style.stroke_dasharray_set = 0;
                        d->dc[d->level].style.stroke_width.value = 1.0;
                        d->dc[d->level].style.stroke.value.color.set( val, val, val );

                        d->dc[d->level].stroke_mode = DRAW_PAINT;
                        d->dc[d->level].stroke_set = true;

                        break;
                    }
                }
            } else {
                if ( /*index >= 0 &&*/ index < (unsigned int) d->n_obj) {
                    switch (d->emf_obj[index].type)
                    {
                        case U_EMR_CREATEPEN:
                            select_pen(d, index);
                            break;
                        case U_EMR_CREATEBRUSHINDIRECT:
                        case U_EMR_CREATEDIBPATTERNBRUSHPT:
                        case U_EMR_CREATEMONOBRUSH:
                            select_brush(d, index);
                            break;
                        case U_EMR_EXTCREATEPEN:
                            select_extpen(d, index);
                            break;
                        case U_EMR_EXTCREATEFONTINDIRECTW:
                            select_font(d, index);
                            break;
                    }
                }
            }
            break;
        }
        case U_EMR_CREATEPEN:
        {
            dbg_str << "<!-- U_EMR_CREATEPEN -->\n";

            PU_EMRCREATEPEN pEmr = (PU_EMRCREATEPEN) lpEMFR;
            insert_object(d, pEmr->ihPen, U_EMR_CREATEPEN, lpEMFR);
            break;
        }
        case U_EMR_CREATEBRUSHINDIRECT:
        {
            dbg_str << "<!-- U_EMR_CREATEBRUSHINDIRECT -->\n";

            PU_EMRCREATEBRUSHINDIRECT pEmr = (PU_EMRCREATEBRUSHINDIRECT) lpEMFR;
            insert_object(d, pEmr->ihBrush, U_EMR_CREATEBRUSHINDIRECT, lpEMFR);
            break;
        }
        case U_EMR_DELETEOBJECT:
            dbg_str << "<!-- U_EMR_DELETEOBJECT -->\n";
            break;
        case U_EMR_ANGLEARC:
            dbg_str << "<!-- U_EMR_ANGLEARC -->\n";
            break;
        case U_EMR_ELLIPSE:
        {
            dbg_str << "<!-- U_EMR_ELLIPSE -->\n";

            PU_EMRELLIPSE pEmr = (PU_EMRELLIPSE) lpEMFR;
            U_RECTL rclBox = pEmr->rclBox;

            double l = pix_to_x_point( d, rclBox.left,  rclBox.top );
            double t = pix_to_y_point( d, rclBox.left,  rclBox.top );
            double r = pix_to_x_point( d, rclBox.right, rclBox.bottom );
            double b = pix_to_y_point( d, rclBox.right, rclBox.bottom );

            double cx = (l + r) / 2.0;
            double cy = (t + b) / 2.0;
            double rx = fabs(l - r) / 2.0;
            double ry = fabs(t - b) / 2.0;

            SVGOStringStream tmp_ellipse;
            tmp_ellipse << "cx=\"" << cx << "\" ";
            tmp_ellipse << "cy=\"" << cy << "\" ";
            tmp_ellipse << "rx=\"" << rx << "\" ";
            tmp_ellipse << "ry=\"" << ry << "\" ";

            d->mask |= emr_mask;

           *(d->outsvg) += "    <ellipse ";
            output_style(d, lpEMFR->iType);  //
            *(d->outsvg) += "\n\t";
            *(d->outsvg) += tmp_ellipse.str().c_str();
            *(d->outsvg) += "/> \n";
            *(d->path) = "";
            break;
        }
        case U_EMR_RECTANGLE:
        {
            dbg_str << "<!-- U_EMR_RECTANGLE -->\n";

            PU_EMRRECTANGLE pEmr = (PU_EMRRECTANGLE) lpEMFR;
            U_RECTL rc = pEmr->rclBox;

            double l = pix_to_x_point( d, rc.left, rc.top );
            double t = pix_to_y_point( d, rc.left, rc.top );
            double r = pix_to_x_point( d, rc.right, rc.bottom );
            double b = pix_to_y_point( d, rc.right, rc.bottom );

            SVGOStringStream tmp_rectangle;
            tmp_rectangle << "\n\tM " << l << " " << t << " ";
            tmp_rectangle << "\n\tL " << r << " " << t << " ";
            tmp_rectangle << "\n\tL " << r << " " << b << " ";
            tmp_rectangle << "\n\tL " << l << " " << b << " ";
            tmp_rectangle << "\n\tz";

            d->mask |= emr_mask;

            tmp_path << tmp_rectangle.str().c_str();
            break;
        }
        case U_EMR_ROUNDRECT:
        {
            dbg_str << "<!-- U_EMR_ROUNDRECT -->\n";

            PU_EMRROUNDRECT pEmr = (PU_EMRROUNDRECT) lpEMFR;
            U_RECTL rc = pEmr->rclBox;
            U_SIZEL corner = pEmr->szlCorner;
            double f = 4.*(sqrt(2) - 1)/3;

            double l = pix_to_x_point(d, rc.left, rc.top);
            double t = pix_to_y_point(d, rc.left, rc.top);
            double r = pix_to_x_point(d, rc.right, rc.bottom);
            double b = pix_to_y_point(d, rc.right, rc.bottom);
            double cnx = pix_to_size_point(d, corner.cx/2);
            double cny = pix_to_size_point(d, corner.cy/2);

            SVGOStringStream tmp_rectangle;
            tmp_rectangle << "\n\tM " << l << ", " << t + cny << " ";
            tmp_rectangle << "\n\tC " << l << ", " << t + (1-f)*cny << " " << l + (1-f)*cnx << ", " << t << " " << l + cnx << ", " << t << " ";
            tmp_rectangle << "\n\tL " << r - cnx << ", " << t << " ";
            tmp_rectangle << "\n\tC " << r - (1-f)*cnx << ", " << t << " " << r << ", " << t + (1-f)*cny << " " << r << ", " << t + cny << " ";
            tmp_rectangle << "\n\tL " << r << ", " << b - cny << " ";
            tmp_rectangle << "\n\tC " << r << ", " << b - (1-f)*cny << " " << r - (1-f)*cnx << ", " << b << " " << r - cnx << ", " << b << " ";
            tmp_rectangle << "\n\tL " << l + cnx << ", " << b << " ";
            tmp_rectangle << "\n\tC " << l + (1-f)*cnx << ", " << b << " " << l << ", " << b - (1-f)*cny << " " << l << ", " << b - cny << " ";
            tmp_rectangle << "\n\tz";

            d->mask |= emr_mask;

            tmp_path <<   tmp_rectangle.str().c_str();
            break;
        }
        case U_EMR_ARC:
        {
            dbg_str << "<!-- U_EMR_ARC -->\n";
            U_PAIRF center,start,end,size;
            int f1;
            int f2 = (d->arcdir == U_AD_COUNTERCLOCKWISE ? 0 : 1);
            if(!emr_arc_points( lpEMFR, &f1, f2, &center, &start, &end, &size)){
               tmp_path <<  "\n\tM " << pix_to_x_point(d, start.x, start.y)   << "," << pix_to_y_point(d, start.x, start.y);
               tmp_path <<  " A "    << pix_to_x_point(d, size.x, size.y)/2.0     << "," << pix_to_y_point(d, size.x, size.y)/2.0 ;
               tmp_path <<  " 0 ";
               tmp_path <<  " " << f1 << "," << f2 << " ";
               tmp_path <<              pix_to_x_point(d, end.x, end.y)       << "," << pix_to_y_point(d, end.x, end.y)<< " ";

               d->mask |= emr_mask;
            }
            else {
               dbg_str << "<!-- ARC record is invalid -->\n";
            }
            break;
        }
        case U_EMR_CHORD:
        {
            dbg_str << "<!-- U_EMR_CHORD -->\n";
            U_PAIRF center,start,end,size;
            int f1;
            int f2 = (d->arcdir == U_AD_COUNTERCLOCKWISE ? 0 : 1);
            if(!emr_arc_points( lpEMFR, &f1, f2, &center, &start, &end, &size)){
               tmp_path <<  "\n\tM " << pix_to_x_point(d, start.x, start.y)   << "," << pix_to_y_point(d, start.x, start.y);
               tmp_path <<  " A "    << pix_to_x_point(d, size.x, size.y)/2.0     << "," << pix_to_y_point(d, size.x, size.y)/2.0 ;
               tmp_path <<  " 0 ";
               tmp_path <<  " " << f1 << "," << f2 << " ";
               tmp_path <<              pix_to_x_point(d, end.x, end.y)       << "," << pix_to_y_point(d, end.x, end.y);
               tmp_path << " z ";
               d->mask |= emr_mask;
            }
            else {
               dbg_str << "<!-- CHORD record is invalid -->\n";
            }
            break;
        }
        case U_EMR_PIE:
        {
            dbg_str << "<!-- U_EMR_PIE -->\n";
            U_PAIRF center,start,end,size;
            int f1;
            int f2 = (d->arcdir == U_AD_COUNTERCLOCKWISE ? 0 : 1);
            if(!emr_arc_points( lpEMFR, &f1, f2, &center, &start, &end, &size)){
               tmp_path <<  "\n\tM " << pix_to_x_point(d, center.x, center.y) << "," << pix_to_y_point(d, center.x, center.y);
               tmp_path <<  "\n\tL " << pix_to_x_point(d, start.x, start.y)   << "," << pix_to_y_point(d, start.x, start.y);
               tmp_path <<  " A "    << pix_to_x_point(d, size.x, size.y)/2.0     << "," << pix_to_y_point(d, size.x, size.y)/2.0;
               tmp_path <<  " 0 ";
               tmp_path <<  " " << f1 << "," << f2 << " ";
               tmp_path <<              pix_to_x_point(d, end.x, end.y)       << "," << pix_to_y_point(d, end.x, end.y);
               tmp_path << " z ";
               d->mask |= emr_mask;
            }
            else {
               dbg_str << "<!-- PIE record is invalid -->\n";
            }
            break;
        }
        case U_EMR_SELECTPALETTE:        dbg_str << "<!-- U_EMR_SELECTPALETTE -->\n";        break;
        case U_EMR_CREATEPALETTE:        dbg_str << "<!-- U_EMR_CREATEPALETTE -->\n";        break;
        case U_EMR_SETPALETTEENTRIES:    dbg_str << "<!-- U_EMR_SETPALETTEENTRIES -->\n";    break;
        case U_EMR_RESIZEPALETTE:        dbg_str << "<!-- U_EMR_RESIZEPALETTE -->\n";        break;
        case U_EMR_REALIZEPALETTE:       dbg_str << "<!-- U_EMR_REALIZEPALETTE -->\n";       break;
        case U_EMR_EXTFLOODFILL:         dbg_str << "<!-- U_EMR_EXTFLOODFILL -->\n";         break;
        case U_EMR_LINETO:
        {
            dbg_str << "<!-- U_EMR_LINETO -->\n";

            PU_EMRLINETO pEmr = (PU_EMRLINETO) lpEMFR;

            d->mask |= emr_mask;

            tmp_path <<
                "\n\tL " <<
                pix_to_x_point( d, pEmr->ptl.x, pEmr->ptl.y ) << " " <<
                pix_to_y_point( d, pEmr->ptl.x, pEmr->ptl.y ) << " ";
            break;
        }
        case U_EMR_ARCTO:
        {
            dbg_str << "<!-- U_EMR_ARCTO -->\n";
            U_PAIRF center,start,end,size;
            int f1;
            int f2 = (d->arcdir == U_AD_COUNTERCLOCKWISE ? 0 : 1);
            if(!emr_arc_points( lpEMFR, &f1, f2, &center, &start, &end, &size)){
               // draw a line from current position to start
               tmp_path <<  "\n\tL " << pix_to_x_point(d, start.x, start.y)   << "," << pix_to_y_point(d, start.x, start.y);
               tmp_path <<  "\n\tM " << pix_to_x_point(d, start.x, start.y)   << "," << pix_to_y_point(d, start.x, start.y);
               tmp_path <<  " A "    << pix_to_x_point(d, size.x, size.y)/2.0     << "," << pix_to_y_point(d, size.x, size.y)/2.0 ;
               tmp_path <<  " 0 ";
               tmp_path <<  " " << f1 << "," << f2 << " ";
               tmp_path <<              pix_to_x_point(d, end.x, end.y)       << "," << pix_to_y_point(d, end.x, end.y)<< " ";

               d->mask |= emr_mask;
            }
            else {
               dbg_str << "<!-- ARCTO record is invalid -->\n";
            }
            break;
        }
        case U_EMR_POLYDRAW:             dbg_str << "<!-- U_EMR_POLYDRAW -->\n";             break;
        case U_EMR_SETARCDIRECTION:
        {
              dbg_str << "<!-- U_EMR_SETARCDIRECTION -->\n";
              PU_EMRSETARCDIRECTION pEmr = (PU_EMRSETARCDIRECTION) lpEMFR;
              if(d->arcdir == U_AD_CLOCKWISE || d->arcdir == U_AD_COUNTERCLOCKWISE){ // EMF file could be corrupt
                 d->arcdir = pEmr->iArcDirection;
              }
              break;
        }
        case U_EMR_SETMITERLIMIT:
        {
            dbg_str << "<!-- U_EMR_SETMITERLIMIT -->\n";

            PU_EMRSETMITERLIMIT pEmr = (PU_EMRSETMITERLIMIT) lpEMFR;

            //The function takes a float but saves a 32 bit int in the U_EMR_SETMITERLIMIT record.
            float miterlimit = *((int32_t *) &(pEmr->eMiterLimit));
            d->dc[d->level].style.stroke_miterlimit.value = miterlimit;   //ratio, not a pt size
            if (d->dc[d->level].style.stroke_miterlimit.value < 2)
                d->dc[d->level].style.stroke_miterlimit.value = 2.0;
            break;
        }
        case U_EMR_BEGINPATH:
        {
            dbg_str << "<!-- U_EMR_BEGINPATH -->\n";
            // The next line should never be needed, should have been handled before main switch
            *(d->path) = "";
            d->mask |= emr_mask;
            break;
        }
        case U_EMR_ENDPATH:
        {
            dbg_str << "<!-- U_EMR_ENDPATH -->\n";
            d->mask &= (0xFFFFFFFF - U_DRAW_ONLYTO);  // clear the OnlyTo bit (it might not have been set), prevents any further path extension
            break;
        }
        case U_EMR_CLOSEFIGURE:
        {
            dbg_str << "<!-- U_EMR_CLOSEFIGURE -->\n";
            // EMF may contain multiple closefigures on one path
            tmp_path << "\n\tz";
            d->mask |= U_DRAW_CLOSED;
            break;
        }
        case U_EMR_FILLPATH:
        {
            dbg_str << "<!-- U_EMR_FILLPATH -->\n";
            if(d->mask & U_DRAW_PATH){          // Operation only effects declared paths
               if(!(d->mask & U_DRAW_CLOSED)){  // Close a path not explicitly closed by an EMRCLOSEFIGURE, otherwise fill makes no sense
                  tmp_path << "\n\tz";
                  d->mask |= U_DRAW_CLOSED;
               }
               d->mask |= emr_mask;
               d->drawtype = U_EMR_FILLPATH;
            }
            break;
        }
        case U_EMR_STROKEANDFILLPATH:
        {
            dbg_str << "<!-- U_EMR_STROKEANDFILLPATH -->\n";
            if(d->mask & U_DRAW_PATH){          // Operation only effects declared paths
               if(!(d->mask & U_DRAW_CLOSED)){  // Close a path not explicitly closed by an EMRCLOSEFIGURE, otherwise fill makes no sense
                  tmp_path << "\n\tz";
                  d->mask |= U_DRAW_CLOSED;
               }
               d->mask |= emr_mask;
               d->drawtype = U_EMR_STROKEANDFILLPATH;
            }
            break;
        }
        case U_EMR_STROKEPATH:
        {
            dbg_str << "<!-- U_EMR_STROKEPATH -->\n";
            if(d->mask & U_DRAW_PATH){          // Operation only effects declared paths
               d->mask |= emr_mask;
               d->drawtype = U_EMR_STROKEPATH;
            }
            break;
        }
        case U_EMR_FLATTENPATH:          dbg_str << "<!-- U_EMR_FLATTENPATH -->\n";          break;
        case U_EMR_WIDENPATH:            dbg_str << "<!-- U_EMR_WIDENPATH -->\n";            break;
        case U_EMR_SELECTCLIPPATH:       dbg_str << "<!-- U_EMR_SELECTCLIPPATH -->\n";       break;
        case U_EMR_ABORTPATH:
        {
            dbg_str << "<!-- U_EMR_ABORTPATH -->\n";
            *(d->path) = "";
            d->drawtype = 0;
            break;
        }
        case U_EMR_UNDEF69:              dbg_str << "<!-- U_EMR_UNDEF69 -->\n";              break;
        case U_EMR_COMMENT:
        {
            dbg_str << "<!-- U_EMR_COMMENT -->\n";
            
            PU_EMRCOMMENT pEmr = (PU_EMRCOMMENT) lpEMFR;

            char *szTxt = (char *) pEmr->Data;

            for (uint32_t i = 0; i < pEmr->cbData; i++) {
                if ( *szTxt) {
                    if ( *szTxt >= ' ' && *szTxt < 'z' && *szTxt != '<' && *szTxt != '>' ) {
                        tmp_str << *szTxt;
                    }
                    szTxt++;
                }
            }

            if (0 && strlen(tmp_str.str().c_str())) {
                tmp_outsvg << "    <!-- \"";
                tmp_outsvg << tmp_str.str().c_str();
                tmp_outsvg << "\" -->\n";
            }
            
            break;
        }   
        case U_EMR_FILLRGN:              dbg_str << "<!-- U_EMR_FILLRGN -->\n";              break;
        case U_EMR_FRAMERGN:             dbg_str << "<!-- U_EMR_FRAMERGN -->\n";             break;
        case U_EMR_INVERTRGN:            dbg_str << "<!-- U_EMR_INVERTRGN -->\n";            break;
        case U_EMR_PAINTRGN:             dbg_str << "<!-- U_EMR_PAINTRGN -->\n";             break;
        case U_EMR_EXTSELECTCLIPRGN:
        {
            dbg_str << "<!-- U_EMR_EXTSELECTCLIPRGN -->\n";

            PU_EMREXTSELECTCLIPRGN pEmr = (PU_EMREXTSELECTCLIPRGN) lpEMFR;
            if (pEmr->iMode == U_RGN_COPY)
                clipset = false;
            break;
        }
        case U_EMR_BITBLT:
        {
            dbg_str << "<!-- U_EMR_BITBLT -->\n";

            PU_EMRBITBLT pEmr = (PU_EMRBITBLT) lpEMFR;
            double l = pix_to_x_point( d, pEmr->Dest.x, pEmr->Dest.y);
            double t = pix_to_y_point( d, pEmr->Dest.x, pEmr->Dest.y);
            double r = pix_to_x_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
            double b = pix_to_y_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
            // Treat all nonImage bitblts as a rectangular write.  Definitely not correct, but at
            // least it leaves objects where the operations should have been.
            if (!pEmr->cbBmiSrc) {
                // should be an application of a DIBPATTERNBRUSHPT, use a solid color instead

                SVGOStringStream tmp_rectangle;
                tmp_rectangle << "\n\tM " << l << " " << t << " ";
                tmp_rectangle << "\n\tL " << r << " " << t << " ";
                tmp_rectangle << "\n\tL " << r << " " << b << " ";
                tmp_rectangle << "\n\tL " << l << " " << b << " ";
                tmp_rectangle << "\n\tz";

                d->mask |= emr_mask;
                d->dwRop3 = pEmr->dwRop;   // we will try to approximate SOME of these
                d->mask |= U_DRAW_CLOSED; // Bitblit is not really open or closed, but we need it to fill, and this is the flag for that

                tmp_path <<   tmp_rectangle.str().c_str();
            }
            else {
                common_image_extraction(d,pEmr,l,t,r,b,
                   pEmr->iUsageSrc, pEmr->offBitsSrc, pEmr->cbBitsSrc, pEmr->offBmiSrc, pEmr->cbBmiSrc);
            }
            break;
        }
        case U_EMR_STRETCHBLT:
        {
            dbg_str << "<!-- U_EMR_STRETCHBLT -->\n";
            PU_EMRSTRETCHBLT pEmr = (PU_EMRSTRETCHBLT) lpEMFR;
            // Always grab image, ignore modes.
            if (pEmr->cbBmiSrc) {
                double l = pix_to_x_point( d, pEmr->Dest.x, pEmr->Dest.y);
                double t = pix_to_y_point( d, pEmr->Dest.x, pEmr->Dest.y);
                double r = pix_to_x_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
                double b = pix_to_y_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
                common_image_extraction(d,pEmr,l,t,r,b,
                   pEmr->iUsageSrc, pEmr->offBitsSrc, pEmr->cbBitsSrc, pEmr->offBmiSrc, pEmr->cbBmiSrc);
            }
            break;
        }
        case U_EMR_MASKBLT:
        {
            dbg_str << "<!-- U_EMR_MASKBLT -->\n";
            PU_EMRMASKBLT pEmr = (PU_EMRMASKBLT) lpEMFR;
            // Always grab image, ignore masks and modes.
            if (pEmr->cbBmiSrc) {
                double l = pix_to_x_point( d, pEmr->Dest.x, pEmr->Dest.y);
                double t = pix_to_y_point( d, pEmr->Dest.x, pEmr->Dest.y);
                double r = pix_to_x_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
                double b = pix_to_y_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y);
                common_image_extraction(d,pEmr,l,t,r,b,
                   pEmr->iUsageSrc, pEmr->offBitsSrc, pEmr->cbBitsSrc, pEmr->offBmiSrc, pEmr->cbBmiSrc);
            }
            break;
        }
        case U_EMR_PLGBLT:               dbg_str << "<!-- U_EMR_PLGBLT -->\n";               break;
        case U_EMR_SETDIBITSTODEVICE:    dbg_str << "<!-- U_EMR_SETDIBITSTODEVICE -->\n";    break;
        case U_EMR_STRETCHDIBITS:
        {
            // Some applications use multiple EMF operations, including multiple STRETCHDIBITS to create
            // images with transparent regions.  PowerPoint does this with rotated images, for instance.
            // Parsing all of that to derive a single resultant image object is left for a later version
            // of this code.  In the meantime, every STRETCHDIBITS goes directly to an image.  The Inkscape
            // user can sort out transparency later using Gimp, if need be.

            PU_EMRSTRETCHDIBITS pEmr = (PU_EMRSTRETCHDIBITS) lpEMFR;
            double l = pix_to_x_point( d, pEmr->Dest.x,                 pEmr->Dest.y                 );
            double t = pix_to_y_point( d, pEmr->Dest.x,                 pEmr->Dest.y                 );
            double r = pix_to_x_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y );
            double b = pix_to_y_point( d, pEmr->Dest.x + pEmr->cDest.x, pEmr->Dest.y + pEmr->cDest.y );
            common_image_extraction(d,pEmr,l,t,r,b,
               pEmr->iUsageSrc, pEmr->offBitsSrc, pEmr->cbBitsSrc, pEmr->offBmiSrc, pEmr->cbBmiSrc);

            dbg_str << "<!-- U_EMR_STRETCHDIBITS -->\n";
            break;
        }
        case U_EMR_EXTCREATEFONTINDIRECTW:
        {
            dbg_str << "<!-- U_EMR_EXTCREATEFONTINDIRECTW -->\n";

            PU_EMREXTCREATEFONTINDIRECTW pEmr = (PU_EMREXTCREATEFONTINDIRECTW) lpEMFR;
            insert_object(d, pEmr->ihFont, U_EMR_EXTCREATEFONTINDIRECTW, lpEMFR);
            break;
        }
        case U_EMR_EXTTEXTOUTA:
        case U_EMR_EXTTEXTOUTW:
        case U_EMR_SMALLTEXTOUT:
        {
            dbg_str << "<!-- U_EMR_EXTTEXTOUTA/W -->\n";

            PU_EMREXTTEXTOUTW  pEmr  = (PU_EMREXTTEXTOUTW) lpEMFR;
            PU_EMRSMALLTEXTOUT pEmrS = (PU_EMRSMALLTEXTOUT) lpEMFR;

            double x1,y1;
            int roff   = sizeof(U_EMRSMALLTEXTOUT);  //offset to the start of the variable fields, only used with U_EMR_SMALLTEXTOUT
            int cChars;
            if(lpEMFR->iType==U_EMR_SMALLTEXTOUT){
                x1 = pEmrS->Dest.x;
                y1 = pEmrS->Dest.y;
                cChars = pEmrS->cChars;
                if(!(pEmrS->fuOptions & U_ETO_NO_RECT)){ roff += sizeof(U_RECTL); }
            }
            else {
                x1 = pEmr->emrtext.ptlReference.x;
                y1 = pEmr->emrtext.ptlReference.y;
                cChars = 0;
            }
            
            if (d->dc[d->level].textAlign & U_TA_UPDATECP) {
                x1 = d->dc[d->level].cur.x;
                y1 = d->dc[d->level].cur.y;
            }

            double x = pix_to_x_point(d, x1, y1);
            double y = pix_to_y_point(d, x1, y1);

            double dfact;
            if (d->dc[d->level].textAlign & U_TA_BASEBIT){    dfact =  0.00;    }  // alignments 0x10 to U_TA_BASELINE 0x18 
            else if(d->dc[d->level].textAlign & U_TA_BOTTOM){ dfact = -0.35;    }  // alignments U_TA_BOTTOM 0x08 to 0x0E,  factor is approximate
            else {                                            dfact =  0.85;    }  // alignments U_TA_TOP 0x00 to 0x07, factor is approximate
            if (d->dc[d->level].style.baseline_shift.value) {
                x += dfact * std::sin(d->dc[d->level].style.baseline_shift.value*M_PI/180.0)*fabs(d->dc[d->level].style.font_size.computed);
                y += dfact * std::cos(d->dc[d->level].style.baseline_shift.value*M_PI/180.0)*fabs(d->dc[d->level].style.font_size.computed);
            }
            else {
                y += dfact * fabs(d->dc[d->level].style.font_size.computed);
            }

            uint32_t *dup_wt = NULL;

            if(       lpEMFR->iType==U_EMR_EXTTEXTOUTA){ 
               /* These should be JUST ASCII, but they might not be...
                  If it holds Utf-8 or plain ASCII the first call will succeed.
                  If not, assume that it holds Latin1.
                  If that fails then someting is really screwed up!
               */
               dup_wt = U_Utf8ToUtf32le((char *) pEmr + pEmr->emrtext.offString, pEmr->emrtext.nChars, NULL);
               if(!dup_wt)dup_wt = U_Latin1ToUtf32le((char *) pEmr + pEmr->emrtext.offString, pEmr->emrtext.nChars, NULL);
               if(!dup_wt)dup_wt = unknown_chars(pEmr->emrtext.nChars);
            }
            else if(  lpEMFR->iType==U_EMR_EXTTEXTOUTW){
               dup_wt = U_Utf16leToUtf32le((uint16_t *)((char *) pEmr + pEmr->emrtext.offString), pEmr->emrtext.nChars, NULL);
               if(!dup_wt)dup_wt = unknown_chars(pEmr->emrtext.nChars);
            }
            else { // U_EMR_SMALLTEXTOUT
               if(pEmrS->fuOptions & U_ETO_SMALL_CHARS){
                  dup_wt = U_Utf8ToUtf32le((char *) pEmrS + roff, cChars, NULL);
               }
               else {
                  dup_wt = U_Utf16leToUtf32le((uint16_t *)((char *) pEmrS + roff), cChars, NULL);
               }
               if(!dup_wt)dup_wt = unknown_chars(cChars);
            }

            msdepua(dup_wt); //convert everything in Microsoft's private use area.  For Symbol, Wingdings, Dingbats

            if(NonToUnicode(dup_wt, d->dc[d->level].font_name)){
               g_free(d->dc[d->level].font_name);
               d->dc[d->level].font_name =  g_strdup("Times New Roman");
            }

            char *ansi_text;
            ansi_text = (char *) U_Utf32leToUtf8((uint32_t *)dup_wt, 0, NULL);
            free(dup_wt);
            // Empty string or starts with an invalid escape/control sequence, which is bogus text.  Throw it out before g_markup_escape_text can make things worse
            if(*ansi_text <= 0x1F){
               free(ansi_text);
               ansi_text=NULL;
            }

            if (ansi_text) {
//                gchar *p = ansi_text;
//                while (*p) {
//                    if (*p < 32 || *p >= 127) {
//                        g_free(ansi_text);
//                        ansi_text = g_strdup("");
//                        break;
//                    }
//                    p++;
//                }

                SVGOStringStream ts;

                gchar *escaped_text = g_markup_escape_text(ansi_text, -1);

//                float text_rgb[3];
//                sp_color_get_rgb_floatv( &(d->dc[d->level].style.fill.value.color), text_rgb );

//                if (!d->dc[d->level].textColorSet) {
//                    d->dc[d->level].textColor = RGB(SP_COLOR_F_TO_U(text_rgb[0]),
//                                       SP_COLOR_F_TO_U(text_rgb[1]),
//                                       SP_COLOR_F_TO_U(text_rgb[2]));
//                }

                char tmp[128];
                snprintf(tmp, 127,
                         "fill:#%02x%02x%02x;",
                         U_RGBAGetR(d->dc[d->level].textColor),
                         U_RGBAGetG(d->dc[d->level].textColor),
                         U_RGBAGetB(d->dc[d->level].textColor));

                bool i = (d->dc[d->level].style.font_style.value == SP_CSS_FONT_STYLE_ITALIC);
                //bool o = (d->dc[d->level].style.font_style.value == SP_CSS_FONT_STYLE_OBLIQUE);
                bool b = (d->dc[d->level].style.font_weight.value == SP_CSS_FONT_WEIGHT_BOLD) ||
                    (d->dc[d->level].style.font_weight.value >= SP_CSS_FONT_WEIGHT_500 && d->dc[d->level].style.font_weight.value <= SP_CSS_FONT_WEIGHT_900);
                // EMF textalignment is a bit strange: 0x6 is center, 0x2 is right, 0x0 is left, the value 0x4 is also drawn left
                int lcr = ((d->dc[d->level].textAlign & U_TA_CENTER) == U_TA_CENTER) ? 2 : ((d->dc[d->level].textAlign & U_TA_CENTER) == U_TA_LEFT) ? 0 : 1;

                ts << "<text\n";
                ts << "  xml:space=\"preserve\"\n";
                ts << "    x=\"" << x << "\"\n";
                ts << "    y=\"" << y << "\"\n";
                if (d->dc[d->level].style.baseline_shift.value) {
                    ts << "    transform=\""
                       << "rotate(-" << d->dc[d->level].style.baseline_shift.value
                       << " " << x << " " << y << ")"
                       << "\"\n";
                }
                ts << "><tspan sodipodi:role=\"line\"";
                ts << "    x=\"" << x << "\"\n";
                ts << "    y=\"" << y << "\"\n";
                ts << "    style=\""
                   << "font-size:" << fabs(d->dc[d->level].style.font_size.computed) << "px;"
                   << tmp
                   << "font-style:" << (i ? "italic" : "normal") << ";"
                   << "font-weight:" << (b ? "bold" : "normal") << ";"
                   << "text-align:" << (lcr==2 ? "center" : lcr==1 ? "end" : "start") << ";"
                   << "text-anchor:" << (lcr==2 ? "middle" : lcr==1 ? "end" : "start") << ";"
                   << "font-family:" << d->dc[d->level].font_name << ";"
                   << "\"\n";
                ts << "    >";
                ts << escaped_text;
                ts << "</tspan>";
                ts << "</text>\n";
                
                *(d->outsvg) += ts.str().c_str();
                
                g_free(escaped_text);
                free(ansi_text);
            }
            
            break;
        }
        case U_EMR_POLYBEZIER16:
        {
            dbg_str << "<!-- U_EMR_POLYBEZIER16 -->\n";

            PU_EMRPOLYBEZIER16 pEmr = (PU_EMRPOLYBEZIER16) lpEMFR;
            PU_POINT16 apts = (PU_POINT16) pEmr->apts; // Bug in MinGW wingdi.h ?
            uint32_t i,j;

            if (pEmr->cpts<4)
                break;

            d->mask |= emr_mask;

            tmp_str <<
                "\n\tM " <<
                pix_to_x_point( d, apts[0].x, apts[0].y ) << " " <<
                pix_to_y_point( d, apts[0].x, apts[0].y ) << " ";

            for (i=1; i<pEmr->cpts; ) {
                tmp_str << "\n\tC ";
                for (j=0; j<3 && i<pEmr->cpts; j++,i++) {
                    tmp_str <<
                        pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                        pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
                }
            }

            tmp_path << tmp_str.str().c_str();

            break;
        }
        case U_EMR_POLYGON16:
        {
            dbg_str << "<!-- U_EMR_POLYGON16 -->\n";

            PU_EMRPOLYGON16 pEmr = (PU_EMRPOLYGON16) lpEMFR;
            PU_POINT16 apts = (PU_POINT16) pEmr->apts; // Bug in MinGW wingdi.h ?
            SVGOStringStream tmp_poly;
            unsigned int i;
            unsigned int first = 0;

            d->mask |= emr_mask;
            
            // skip the first point?
            tmp_poly << "\n\tM " <<
                pix_to_x_point( d, apts[first].x, apts[first].y ) << " " <<
                pix_to_y_point( d, apts[first].x, apts[first].y ) << " ";

            for (i=first+1; i<pEmr->cpts; i++) {
                tmp_poly << "\n\tL " <<
                    pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                    pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
            }

            tmp_path <<  tmp_poly.str().c_str();
            tmp_path << "\n\tz";
            d->mask |= U_DRAW_CLOSED;

            break;
        }
        case U_EMR_POLYLINE16:
        {
            dbg_str << "<!-- U_EMR_POLYLINE16 -->\n";

            PU_EMRPOLYLINE16 pEmr = (PU_EMRPOLYLINE16) lpEMFR;
            PU_POINT16 apts = (PU_POINT16) pEmr->apts; // Bug in MinGW wingdi.h ?
            uint32_t i;

            if (pEmr->cpts<2)
                break;

            d->mask |= emr_mask;

            tmp_str <<
                "\n\tM " <<
                pix_to_x_point( d, apts[0].x, apts[0].y ) << " " <<
                pix_to_y_point( d, apts[0].x, apts[0].y ) << " ";

            for (i=1; i<pEmr->cpts; i++) {
                tmp_str <<
                    "\n\tL " <<
                    pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                    pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
            }

            tmp_path << tmp_str.str().c_str();

            break;
        }
        case U_EMR_POLYBEZIERTO16:
        {
            dbg_str << "<!-- U_EMR_POLYBEZIERTO16 -->\n";

            PU_EMRPOLYBEZIERTO16 pEmr = (PU_EMRPOLYBEZIERTO16) lpEMFR;
            PU_POINT16 apts = (PU_POINT16) pEmr->apts; // Bug in MinGW wingdi.h ?
            uint32_t i,j;

            d->mask |= emr_mask;

            for (i=0; i<pEmr->cpts;) {
                tmp_path << "\n\tC ";
                for (j=0; j<3 && i<pEmr->cpts; j++,i++) {
                    tmp_path <<
                        pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                        pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
                }
            }

            break;
        }
        case U_EMR_POLYLINETO16:
        {
            dbg_str << "<!-- U_EMR_POLYLINETO16 -->\n";

            PU_EMRPOLYLINETO16 pEmr = (PU_EMRPOLYLINETO16) lpEMFR;
            PU_POINT16 apts = (PU_POINT16) pEmr->apts; // Bug in MinGW wingdi.h ?
            uint32_t i;

            d->mask |= emr_mask;

            for (i=0; i<pEmr->cpts;i++) {
                tmp_path <<
                    "\n\tL " <<
                    pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                    pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
            }

            break;
        }
        case U_EMR_POLYPOLYLINE16:
        case U_EMR_POLYPOLYGON16:
        {
            if (lpEMFR->iType == U_EMR_POLYPOLYLINE16)
                dbg_str << "<!-- U_EMR_POLYPOLYLINE16 -->\n";
            if (lpEMFR->iType == U_EMR_POLYPOLYGON16)
                dbg_str << "<!-- U_EMR_POLYPOLYGON16 -->\n";

            PU_EMRPOLYPOLYGON16 pEmr = (PU_EMRPOLYPOLYGON16) lpEMFR;
            unsigned int n, i, j;

            d->mask |= emr_mask;

            PU_POINT16 apts = (PU_POINT16) &pEmr->aPolyCounts[pEmr->nPolys];

            i = 0;
            for (n=0; n<pEmr->nPolys && i<pEmr->cpts; n++) {
                SVGOStringStream poly_path;

                poly_path << "\n\tM " <<
                    pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                    pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
                i++;

                for (j=1; j<pEmr->aPolyCounts[n] && i<pEmr->cpts; j++) {
                    poly_path << "\n\tL " <<
                        pix_to_x_point( d, apts[i].x, apts[i].y ) << " " <<
                        pix_to_y_point( d, apts[i].x, apts[i].y ) << " ";
                    i++;
                }

                tmp_str << poly_path.str().c_str();
                if (lpEMFR->iType == U_EMR_POLYPOLYGON16)
                    tmp_str << " z";
                tmp_str << " \n";
            }

            tmp_path << tmp_str.str().c_str();

            break;
        }
        case U_EMR_POLYDRAW16:           dbg_str << "<!-- U_EMR_POLYDRAW16 -->\n";           break;
        case U_EMR_CREATEMONOBRUSH:
        {
            dbg_str << "<!-- U_EMR_CREATEDIBPATTERNBRUSHPT -->\n";

            PU_EMRCREATEMONOBRUSH pEmr = (PU_EMRCREATEMONOBRUSH) lpEMFR;
            insert_object(d, pEmr->ihBrush, U_EMR_CREATEMONOBRUSH, lpEMFR);
            break;
        }
        case U_EMR_CREATEDIBPATTERNBRUSHPT:
        {
            dbg_str << "<!-- U_EMR_CREATEDIBPATTERNBRUSHPT -->\n";

            PU_EMRCREATEDIBPATTERNBRUSHPT pEmr = (PU_EMRCREATEDIBPATTERNBRUSHPT) lpEMFR;
            insert_object(d, pEmr->ihBrush, U_EMR_CREATEDIBPATTERNBRUSHPT, lpEMFR);
            break;
        }
        case U_EMR_EXTCREATEPEN:
        {
            dbg_str << "<!-- U_EMR_EXTCREATEPEN -->\n";

            PU_EMREXTCREATEPEN pEmr = (PU_EMREXTCREATEPEN) lpEMFR;
            insert_object(d, pEmr->ihPen, U_EMR_EXTCREATEPEN, lpEMFR);
            break;
        }
        case U_EMR_POLYTEXTOUTA:         dbg_str << "<!-- U_EMR_POLYTEXTOUTA -->\n";         break;
        case U_EMR_POLYTEXTOUTW:         dbg_str << "<!-- U_EMR_POLYTEXTOUTW -->\n";         break;
        case U_EMR_SETICMMODE:
        {
            dbg_str << "<!-- U_EMR_SETICMMODE -->\n";
            PU_EMRSETICMMODE pEmr = (PU_EMRSETICMMODE) lpEMFR;
            ICMmode= pEmr->iMode;
            break;
        }
        case U_EMR_CREATECOLORSPACE:     dbg_str << "<!-- U_EMR_CREATECOLORSPACE -->\n";     break;
        case U_EMR_SETCOLORSPACE:        dbg_str << "<!-- U_EMR_SETCOLORSPACE -->\n";        break;
        case U_EMR_DELETECOLORSPACE:     dbg_str << "<!-- U_EMR_DELETECOLORSPACE -->\n";     break;
        case U_EMR_GLSRECORD:            dbg_str << "<!-- U_EMR_GLSRECORD -->\n";            break;
        case U_EMR_GLSBOUNDEDRECORD:     dbg_str << "<!-- U_EMR_GLSBOUNDEDRECORD -->\n";     break;
        case U_EMR_PIXELFORMAT:          dbg_str << "<!-- U_EMR_PIXELFORMAT -->\n";          break;
        case U_EMR_DRAWESCAPE:           dbg_str << "<!-- U_EMR_DRAWESCAPE -->\n";           break;
        case U_EMR_EXTESCAPE:            dbg_str << "<!-- U_EMR_EXTESCAPE -->\n";            break;
        case U_EMR_UNDEF107:             dbg_str << "<!-- U_EMR_UNDEF107 -->\n";             break;
        // U_EMR_SMALLTEXTOUT is handled with U_EMR_EXTTEXTOUTA/W above
        case U_EMR_FORCEUFIMAPPING:      dbg_str << "<!-- U_EMR_FORCEUFIMAPPING -->\n";      break;
        case U_EMR_NAMEDESCAPE:          dbg_str << "<!-- U_EMR_NAMEDESCAPE -->\n";          break;
        case U_EMR_COLORCORRECTPALETTE:  dbg_str << "<!-- U_EMR_COLORCORRECTPALETTE -->\n";  break;
        case U_EMR_SETICMPROFILEA:       dbg_str << "<!-- U_EMR_SETICMPROFILEA -->\n";       break;
        case U_EMR_SETICMPROFILEW:       dbg_str << "<!-- U_EMR_SETICMPROFILEW -->\n";       break;
        case U_EMR_ALPHABLEND:           dbg_str << "<!-- U_EMR_ALPHABLEND -->\n";           break;
        case U_EMR_SETLAYOUT:            dbg_str << "<!-- U_EMR_SETLAYOUT -->\n";            break;
        case U_EMR_TRANSPARENTBLT:       dbg_str << "<!-- U_EMR_TRANSPARENTBLT -->\n";       break;
        case U_EMR_UNDEF117:             dbg_str << "<!-- U_EMR_UNDEF117 -->\n";             break;
        case U_EMR_GRADIENTFILL:         dbg_str << "<!-- U_EMR_GRADIENTFILL -->\n";         break;
        /* Gradient fill is doable for rectangles because those correspond to linear gradients.  However,
           the general case for the triangle fill, with a different color in each corner of the triangle,
           has no SVG equivalent and cannot be easily emulated with SVG gradients. Except that so far
           I (DM) have not been able to make an EMF with a rectangular gradientfill record which is not
           completely toxic to other EMF readers.  So far now, do nothing.
        */
        case U_EMR_SETLINKEDUFIS:        dbg_str << "<!-- U_EMR_SETLINKEDUFIS -->\n";        break;
        case U_EMR_SETTEXTJUSTIFICATION: dbg_str << "<!-- U_EMR_SETTEXTJUSTIFICATION -->\n"; break;
        case U_EMR_COLORMATCHTOTARGETW:  dbg_str << "<!-- U_EMR_COLORMATCHTOTARGETW -->\n";  break;
        case U_EMR_CREATECOLORSPACEW:    dbg_str << "<!-- U_EMR_CREATECOLORSPACEW -->\n";    break;
        default:
            dbg_str << "<!-- U_EMR_??? -->\n";
            break;
    }  //end of switch
// When testing, uncomment the following to place a comment for each processed EMR record in the SVG
//    *(d->outsvg) += dbg_str.str().c_str();
    *(d->outsvg) += tmp_outsvg.str().c_str();
    *(d->path) += tmp_path.str().c_str();

    }  //end of while
// When testing, uncomment the following to show the final SVG derived from the EMF
//std::cout << *(d->outsvg) << std::endl; 
    (void) emr_properties(U_EMR_INVALID);  // force the release of the lookup table memory, returned value is irrelevant

    return 1;
}


// Aldus Placeable Header ===================================================
// Since we are a 32bit app, we have to be sure this structure compiles to
// be identical to a 16 bit app's version. To do this, we use the #pragma
// to adjust packing, we use a uint16_t for the hmf handle, and a SMALL_RECT
// for the bbox rectangle.
#pragma pack( push )
#pragma pack( 2 )
typedef struct _SMALL_RECT {
    int16_t Left;
    int16_t Top;
    int16_t Right;
    int16_t Bottom;
} SMALL_RECT, *PSMALL_RECT;
typedef struct
{
    uint32_t       dwKey;
    uint16_t        hmf;
    SMALL_RECT  bbox;
    uint16_t        wInch;
    uint32_t       dwReserved;
    uint16_t        wCheckSum;
} APMHEADER, *PAPMHEADER;
#pragma pack( pop )

void free_emf_strings(EMF_STRINGS name){
   if(name.count){
      for(int i=0; i< name.count; i++){ free(name.strings[i]); }
      free(name.strings);
   }
}

SPDocument *
Emf::open( Inkscape::Extension::Input * /*mod*/, const gchar *uri )
{
    EMF_CALLBACK_DATA d;

    memset(&d, 0, sizeof(d));

    for(int i = 0; i < EMF_MAX_DC+1; i++){  // be sure all values and pointers are empty to start with
       memset(&(d.dc[i]),0,sizeof(EMF_DEVICE_CONTEXT));
    }
    
    d.dc[0].worldTransform.eM11 = 1.0;
    d.dc[0].worldTransform.eM12 = 0.0;
    d.dc[0].worldTransform.eM21 = 0.0;
    d.dc[0].worldTransform.eM22 = 1.0;
    d.dc[0].worldTransform.eDx  = 0.0;
    d.dc[0].worldTransform.eDy  = 0.0;
    d.dc[0].font_name = strdup("Arial");  // Default font, EMF spec says device can pick whatever it wants
        
    if (uri == NULL) {
        return NULL;
    }

    d.outsvg            = new Glib::ustring("");
    d.path              = new Glib::ustring("");
    d.outdef            = new Glib::ustring("");
    d.defs              = new Glib::ustring("");
    d.mask              = 0;
    d.drawtype          = 0;
    d.arcdir            = U_AD_COUNTERCLOCKWISE;
    d.dwRop2            = U_R2_COPYPEN;
    d.dwRop3            = 0;
    d.hatches.size      = 0;
    d.hatches.count     = 0;
    d.hatches.strings   = NULL;
    d.images.size       = 0;
    d.images.count      = 0;
    d.images.strings    = NULL;

    size_t length;
    char *contents;
    if(emf_readdata(uri, &contents, &length))return(NULL);   

    d.pDesc = NULL;

    
    (void) myEnhMetaFileProc(contents,length, &d);
    free(contents);

    
    if (d.pDesc)
        free( d.pDesc );

//    std::cout << "SVG Output: " << std::endl << *(d.outsvg) << std::endl;

    SPDocument *doc = SPDocument::createNewDocFromMem(d.outsvg->c_str(), strlen(d.outsvg->c_str()), TRUE);

    delete d.outsvg;
    delete d.path;
    delete d.outdef;
    delete d.defs;
    free_emf_strings(d.hatches);
    free_emf_strings(d.images);
    
    if (d.emf_obj) {
        int i;
        for (i=0; i<d.n_obj; i++)
            delete_object(&d, i);
        delete[] d.emf_obj;
    }
    
    if (d.dc[0].style.stroke_dash.dash)
        delete[] d.dc[0].style.stroke_dash.dash;
     
    for(int i=0; i<=d.level;i++){
      if(d.dc[i].font_name)free(d.dc[i].font_name);
    }

    return doc;
}


void
Emf::init (void)
{
    /* EMF in */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("EMF Input") "</name>\n"
            "<id>org.inkscape.input.emf</id>\n"
            "<input>\n"
                "<extension>.emf</extension>\n"
                "<mimetype>image/x-emf</mimetype>\n"
                "<filetypename>" N_("Enhanced Metafiles (*.emf)") "</filetypename>\n"
                "<filetypetooltip>" N_("Enhanced Metafiles") "</filetypetooltip>\n"
                "<output_extension>org.inkscape.output.emf</output_extension>\n"
            "</input>\n"
        "</inkscape-extension>", new Emf());

    /* EMF out */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("EMF Output") "</name>\n"
            "<id>org.inkscape.output.emf</id>\n"
            "<param name=\"textToPath\" gui-text=\"" N_("Convert texts to paths") "\" type=\"boolean\">true</param>\n"
            "<param name=\"TnrToSymbol\" gui-text=\"" N_("Map Unicode to Symbol font") "\" type=\"boolean\">true</param>\n"
            "<param name=\"TnrToWingdings\" gui-text=\"" N_("Map Unicode to Wingdings") "\" type=\"boolean\">true</param>\n"
            "<param name=\"TnrToZapfDingbats\" gui-text=\"" N_("Map Unicode to Zapf Dingbats") "\" type=\"boolean\">true</param>\n"
            "<param name=\"UsePUA\" gui-text=\"" N_("Use MS Unicode PUA (0xF020-0xF0FF) for converted characters") "\" type=\"boolean\">false</param>\n"
            "<param name=\"FixPPTCharPos\" gui-text=\"" N_("Compensate for PPT font bug") "\" type=\"boolean\">false</param>\n"
            "<param name=\"FixPPTDashLine\" gui-text=\"" N_("Convert dashed/dotted lines to single lines") "\" type=\"boolean\">false</param>\n"
            "<param name=\"FixPPTGrad2Polys\" gui-text=\"" N_("Convert gradients to colored polygon series") "\" type=\"boolean\">false</param>\n"
            "<param name=\"FixPPTPatternAsHatch\" gui-text=\"" N_("Map all fill patterns to standard EMF hatches") "\" type=\"boolean\">false</param>\n"
            "<output>\n"
                "<extension>.emf</extension>\n"
                "<mimetype>image/x-emf</mimetype>\n"
                "<filetypename>" N_("Enhanced Metafile (*.emf)") "</filetypename>\n"
                "<filetypetooltip>" N_("Enhanced Metafile") "</filetypetooltip>\n"
            "</output>\n"
        "</inkscape-extension>", new Emf());

    return;
}


} } }  /* namespace Inkscape, Extension, Implementation */

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
