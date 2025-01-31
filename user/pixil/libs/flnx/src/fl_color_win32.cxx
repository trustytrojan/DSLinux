//
// "$Id$"
//
// WIN32 color functions for the Fast Light Tool Kit (FLTK).
//
// Copyright 1998-1999 by Bill Spitzak and others.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA.
//
// Please report all bugs and problems to "fltk-bugs@easysw.com".
//

// The fltk "colormap".  This allows ui colors to be stored in 8-bit
// locations, and provides a level of indirection so that global color
// changes can be made.  Not to be confused with the X colormap, which
// I try to hide completely.

// SGI compiler seems to have problems with unsigned char arguments
// being used to index arrays.  So I always copy them to an integer
// before use.

#include <config.h>
#include <FL/Fl.H>
#include <FL/win32.H>
#include <FL/fl_draw.H>

static unsigned fl_cmap[256] = {
#include "fl_cmap.h" // this is a file produced by "cmap.cxx":
};

// Translations to win32 data structures:
Fl_XMap fl_xmap[256];

Fl_XMap* fl_current_xmap;

HPALETTE fl_palette;
HPEN tmppen=0;
HBRUSH tmpbrush=0;

static void clear_xmap(Fl_XMap& xmap) {
  if (xmap.pen) {
    if(!tmppen) tmppen = CreatePen(PS_SOLID, 1, 0);
    if(!tmpbrush) tmpbrush = CreateSolidBrush(0);
    HPEN oldpen = (HPEN)SelectObject(fl_gc, tmppen); // Push out the current pen of the gc
    if(oldpen != xmap.pen) SelectObject(fl_gc, oldpen); // Put it back if it is not the one we are about to delete
    SelectObject(fl_gc, tmpbrush); // Push out the old pen of the gc
    //fl_current_xmap = 0;
    DeleteObject((HGDIOBJ)(xmap.pen));
    xmap.pen = 0;
    xmap.brush = -1;
  }
}

static void set_xmap(Fl_XMap& xmap, COLORREF c) {
  xmap.rgb = c;
  xmap.pen = CreatePen(PS_SOLID, 1, xmap.rgb);
  xmap.brush = -1;
}

Fl_Color fl_color_;

void fl_color(Fl_Color i) {
  fl_color_ = i;
  Fl_XMap &xmap = fl_xmap[i];
  if (!xmap.pen) {
#if USE_COLORMAP
    if (fl_palette) {
      set_xmap(xmap, PALETTEINDEX(i));
    } else {
#endif
      unsigned c = fl_cmap[i];
      set_xmap(xmap, RGB(uchar(c>>24), uchar(c>>16), uchar(c>>8)));
#if USE_COLORMAP
    }
#endif
  }
  fl_current_xmap = &xmap;
  SelectObject(fl_gc, (HGDIOBJ)(xmap.pen));
}

void fl_color(uchar r, uchar g, uchar b) {
  static Fl_XMap xmap;
  COLORREF c = RGB(r,g,b);
  if (!xmap.pen || c != xmap.rgb) {
    clear_xmap(xmap);
    set_xmap(xmap, c);
  }
  fl_current_xmap = &xmap;
  SelectObject(fl_gc, (HGDIOBJ)(xmap.pen));
}

HBRUSH fl_brush() {
  Fl_XMap *xmap = fl_current_xmap;
  // Wonko: we use some statistics to cache only a limited number
  // of brushes:
#define FL_N_BRUSH 16
  static struct Fl_Brush {
    HBRUSH brush;
    unsigned short usage;
    Fl_XMap* backref;
  } brushes[FL_N_BRUSH];

  int i = xmap->brush; // find the associated brush
  if (i != -1) { // if the brush was allready allocated
    if (brushes[i].brush == NULL) goto CREATE_BRUSH;
    if ( (++brushes[i].usage) > 32000 ) { // keep a usage statistic
      for (int j=0; j<FL_N_BRUSH; j++) {
	if (brushes[j].usage>16000)
	  brushes[j].usage -= 16000;
	else 
	  brushes[j].usage = 0;
      }
    }
    return brushes[i].brush;
  } else {
    int umin = 32000, imin = 0;
    for (i=0; i<FL_N_BRUSH; i++) {
      if (brushes[i].brush == NULL) goto CREATE_BRUSH;
      if (brushes[i].usage<umin) {
	umin = brushes[i].usage;
	imin = i;
      }
    }
    i = imin;
    DeleteObject(brushes[i].brush);
    brushes[i].brush = NULL;
    brushes[i].backref->brush = -1;
  }
CREATE_BRUSH:
  brushes[i].brush = CreateSolidBrush(xmap->rgb);
  brushes[i].usage = 0;
  brushes[i].backref = xmap;
  xmap->brush = i;
  return brushes[i].brush;
}

void Fl::free_color(Fl_Color i, int overlay) {
  if (overlay) return; // do something about GL overlay?
  clear_xmap(fl_xmap[i]);
}

void Fl::set_color(Fl_Color i, unsigned c) {
  if (fl_cmap[i] != c) {
    clear_xmap(fl_xmap[i]);
    fl_cmap[i] = c;
  }
}

#if USE_COLORMAP

// 'fl_select_palette()' - Make a color palette for 8-bit displays if necessary
// Thanks to Michael Sweet @ Easy Software Products for this

HPALETTE
fl_select_palette(void)
{
  static char beenhere;
  if (!beenhere) {
    beenhere = 1;

    //if (GetDeviceCaps(fl_gc, BITSPIXEL) > 8) return NULL;
    int nColors = GetDeviceCaps(fl_gc, SIZEPALETTE);
    if (nColors <= 0 || nColors > 256) return NULL;
    // this will try to work on < 256 color screens, but will probably
    // come out quite badly.

    // I lamely try to get this variable-sized object allocated on stack:
    ulong foo[(sizeof(LOGPALETTE)+256*sizeof(PALETTEENTRY))/sizeof(ulong)+1];
    LOGPALETTE *pPal = (LOGPALETTE*)foo;

    pPal->palVersion    = 0x300;
    pPal->palNumEntries = nColors;

    // Build 256 colors from the standard FLTK colormap...

    for (int i = 0; i < nColors; i ++) {
      pPal->palPalEntry[i].peRed   = (fl_cmap[i] >> 24) & 255;
      pPal->palPalEntry[i].peGreen = (fl_cmap[i] >> 16) & 255;
      pPal->palPalEntry[i].peBlue  = (fl_cmap[i] >>  8) & 255;
      pPal->palPalEntry[i].peFlags = 0;
    };

    // Create the palette:
    fl_palette = CreatePalette(pPal);
  }
  if (fl_palette) {
    SelectPalette(fl_gc, fl_palette, FALSE);
    RealizePalette(fl_gc);
  }
  return fl_palette;
}

#endif

//
// End of "$Id$".
//
