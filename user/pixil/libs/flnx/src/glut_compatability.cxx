//
// "$Id$"
//
// GLUT emulation routines for the Fast Light Tool Kit (FLTK).
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

// Emulation of Glut using fltk.

// GLUT is Copyright (c) Mark J. Kilgard, 1994, 1995, 1996.
// "This program is freely distributable without licensing fees  and is
// provided without guarantee or warrantee expressed or  implied. This
// program is -not- in the public domain."

// Although I have copied the GLUT API, none of my code is based on
// any Glut implementation details and is therefore covered by the LGPL.

#include <config.h>
#if HAVE_GL

#include <FL/glut.H>

#define MAXWINDOWS 32
static Fl_Glut_Window *windows[MAXWINDOWS+1];

Fl_Glut_Window *glut_window;
int glut_menu;
void (*glut_menustate_function)(int);
void (*glut_menustatus_function)(int,int,int);

static void default_reshape(int w, int h) {glViewport(0,0,w,h);}

void Fl_Glut_Window::make_current() {
  glut_window = this;
  if (shown()) Fl_Gl_Window::make_current();
}

static int indraw;
void Fl_Glut_Window::draw() {
  glut_window = this;
  indraw = 1;
  if (!valid()) {reshape(w(),h()); valid(1);}
  display();
  indraw = 0;
}

void glutSwapBuffers() {
  if (!indraw) glut_window->swap_buffers();
}

void Fl_Glut_Window::draw_overlay() {
  glut_window = this;
  if (!valid()) {reshape(w(),h()); valid(1);}
  overlaydisplay();
}

static void domenu(int, int, int);

int Fl_Glut_Window::handle(int event) {
  make_current();
  int ex = Fl::event_x();
  int ey = Fl::event_y();
  int button;
  switch (event) {

  case FL_PUSH:
    button = Fl::event_button()-1;
    if (button<0) button = 0;
    if (button>2) button = 2;
    if (menu[button]) {domenu(menu[button],ex,ey); return 1;}
    mouse_down |= 1<<button;
    if (mouse) {mouse(button,GLUT_DOWN,ex,ey); return 1;}
    if (motion) return 1;
    break;

  case FL_RELEASE:
    for (button = 0; button < 3; button++) if (mouse_down & (1<<button)) {
      if (mouse) mouse(button,GLUT_UP,ex,ey);
    }
    mouse_down = 0;
    return 1;

  case FL_ENTER:
    if (entry) {entry(GLUT_ENTERED); return 1;}
    if (passivemotion) return 1;
    break;

  case FL_LEAVE:
    if (entry) {entry(GLUT_LEFT); return 1;}
    if (passivemotion) return 1;
    break;

  case FL_DRAG:
    if (motion) {motion(ex, ey); return 1;}
    break;

  case FL_MOVE:
    if (passivemotion) {passivemotion(ex, ey); return 1;}
    break;

  case FL_FOCUS:
    if (keyboard || special) return 1;
    break;

  case FL_KEYBOARD:
    if (Fl::event_text()[0]) {
      if (keyboard) {keyboard(Fl::event_text()[0],ex,ey); return 1;}
      break;
    } else {
      if (special) {
	int i = Fl::event_key();
	if (i > FL_F && i <= FL_F_Last) i = i-FL_F;
	special(i,ex,ey);
	return 1;
      }
      break;
    }

  case FL_HIDE:
    if (visibility) visibility(GLUT_NOT_VISIBLE);
    break;

  case FL_SHOW:
    if (visibility) visibility(GLUT_VISIBLE);
    break;
  }

  return Fl_Gl_Window::handle(event);
}

static int glut_mode = GLUT_RGB | GLUT_SINGLE | GLUT_DEPTH;

void Fl_Glut_Window::_init() {
  for (number=1; number<MAXWINDOWS; number++) if (!windows[number]) break;
  windows[number] = this;
  menu[0] = menu[1] = menu[2] = 0;
  reshape = default_reshape;
  keyboard = 0;
  mouse = 0;
  motion = 0;
  passivemotion = 0;
  entry = 0;
  visibility = 0;
  special = 0;
  mouse_down = 0;
  mode(glut_mode);
}

Fl_Glut_Window::Fl_Glut_Window(int w, int h, const char *t) :
  Fl_Gl_Window(w,h,t) {_init();}

Fl_Glut_Window::Fl_Glut_Window(int x,int y,int w,int h, const char *t) :
  Fl_Gl_Window(x,y,w,h,t) {_init();}

static int initargc;
static char **initargv;

void glutInit(int *argc, char **argv) {
  initargc = *argc;
  initargv = new char*[*argc+1];
  int i,j;
  for (i=0; i<=*argc; i++) initargv[i] = argv[i];
  for (i=j=1; i<*argc; ) {
    if (Fl::arg(*argc,argv,i));
    else argv[j++] = argv[i++];
  }
  argv[j] = 0;
  *argc = j;
}

void glutInitDisplayMode(unsigned int mode) {
  glut_mode = mode;
}

void glutMainLoop() {Fl::run();}

////////////////////////////////////////////////////////////////

static int initx, inity, initw=300, inith=300, initpos;

void glutInitWindowPosition(int x, int y) {
  initx = x; inity = y; initpos = 1;
}

void glutInitWindowSize(int w, int h) {
  initw = w; inith = h;
}

int glutCreateWindow(char *title) {
  Fl_Glut_Window *W;
  if (initpos) {
    W = new Fl_Glut_Window(initx,inity,initw,inith,title);
    initpos = 0;
  } else {
    W = new Fl_Glut_Window(initw,inith,title);
  }
  W->resizable(W);
  if (initargc) {
    W->show(initargc,initargv);
    initargc = 0;
  } else {
    W->show();
  }
  W->make_current();
  return W->number;
}

int glutCreateSubWindow(int win, int x, int y, int w, int h) {
  Fl_Glut_Window *W = new Fl_Glut_Window(x,y,w,h,0);
  windows[win]->add(W);
  if (windows[win]->shown()) W->show();
  W->make_current();
  return W->number;
}

Fl_Glut_Window::~Fl_Glut_Window() {
  if (glut_window == this) glut_window = 0;
  windows[number] = 0;
}

void glutDestroyWindow(int win) {
  // should destroy children!!!
  delete windows[win];
}

void glutSetWindow(int win) {
  windows[win]->make_current();
}

////////////////////////////////////////////////////////////////
#include <FL/Fl_Menu_Item.H>

struct menu {
  void (*cb)(int);
  Fl_Menu_Item *m;
  int size;
  int alloc;
};

#define MAXMENUS 32
static menu menus[MAXMENUS+1];

static void domenu(int n, int ex, int ey) {
  glut_menu = n;
  menu *m = &menus[n];
  if (glut_menustate_function) glut_menustate_function(1);
  if (glut_menustatus_function) glut_menustatus_function(1,ex,ey);
  const Fl_Menu_Item* g = m->m->popup(Fl::event_x(), Fl::event_y(), 0);
  if (g && g->callback_) ((void (*)(int))(g->callback_))(int(g->argument()));
  if (glut_menustatus_function) glut_menustatus_function(0,ex,ey);
  if (glut_menustate_function) glut_menustate_function(0);
}

int glutCreateMenu(void (*cb)(int)) {
  int i;
  for (i=1; i<MAXMENUS; i++) if (!menus[i].cb) break;
  menu *m = &menus[i];
  m->cb = cb;
  return glut_menu = i;
}

void glutDestroyMenu(int n) {
  menu *m = &menus[n];
  delete[] m->m;
  m->m = 0;
  m->cb = 0;
  m->size = m->alloc = 0;
}

static Fl_Menu_Item* additem(menu *m) {
  if (m->size+1 >= m->alloc) {
    m->alloc = m->size*2+10;
    Fl_Menu_Item* nm = new Fl_Menu_Item[m->alloc];
    for (int i=0; i<m->size; i++) nm[i] = m->m[i];
    delete[] m->m;
    m->m = nm;
  }
  int n = m->size++;
  m->m[n+1].text = 0;
  Fl_Menu_Item* i = &(m->m[n]);
  i->shortcut_ = 0;
  i->flags = 0;
  i->labeltype_ = i->labelfont_ = i->labelsize_ = i->labelcolor_ = 0;
  return i;
}

void glutAddMenuEntry(char *label, int value) {
  menu *m = &menus[glut_menu];
  Fl_Menu_Item* i = additem(m);
  i->text = label;
  i->callback_ = (Fl_Callback*)(m->cb);
  i->user_data_ = (void *)value;
}

void glutAddSubMenu(char *label, int submenu) {
  menu *m = &menus[glut_menu];
  Fl_Menu_Item* i = additem(m);
  i->text = label;
  i->callback_ = 0;
  i->user_data_ = (void *)(menus[submenu].m);
  i->flags = FL_PUP_SUBMENU;
}

void glutChangeToMenuEntry(int item, char *label, int value) {
  menu *m = &menus[glut_menu];
  Fl_Menu_Item* i = &m->m[item-1];
  i->text = label;
  i->callback_ = (Fl_Callback*)(m->cb);
  i->user_data_ = (void *)value;
  i->flags = 0;
}

void glutChangeToSubMenu(int item, char *label, int submenu) {
  menu *m = &menus[glut_menu];
  Fl_Menu_Item* i = &m->m[item-1];
  i->text = label;
  i->callback_ = 0;
  i->user_data_ = (void *)(menus[submenu].m);
  i->flags = FL_PUP_SUBMENU;
}

void glutRemoveMenuItem(int item) {
  menu *m = &menus[glut_menu];
  if (item > m->size || item < 1) return;
  for (int i = item-1; i <= m->size; i++) m->m[i] = m->m[i+1];
  m->size--;
}

////////////////////////////////////////////////////////////////

int glutGet(GLenum type) {
  switch (type) {
  case GLUT_RETURN_ZERO: return 0;
  case GLUT_WINDOW_X: return glut_window->x();
  case GLUT_WINDOW_Y: return glut_window->y();
  case GLUT_WINDOW_WIDTH: return glut_window->w();
  case GLUT_WINDOW_HEIGHT: return glut_window->h();
  case GLUT_WINDOW_PARENT:
    if (glut_window->parent())
      return ((Fl_Glut_Window *)(glut_window->parent()))->number;
    else
      return 0;
//case GLUT_WINDOW_NUM_CHILDREN:
//case GLUT_WINDOW_CURSOR: return 
  case GLUT_SCREEN_WIDTH: return Fl::w();
  case GLUT_SCREEN_HEIGHT: return Fl::h();
//case GLUT_SCREEN_WIDTH_MM:
//case GLUT_SCREEN_HEIGHT_MM:
  case GLUT_MENU_NUM_ITEMS: return menus[glut_menu].size;
  case GLUT_DISPLAY_MODE_POSSIBLE: return Fl_Gl_Window::can_do(glut_mode);
  case GLUT_INIT_WINDOW_X: return initx;
  case GLUT_INIT_WINDOW_Y: return inity;
  case GLUT_INIT_WINDOW_WIDTH: return initw;
  case GLUT_INIT_WINDOW_HEIGHT: return inith;
  case GLUT_INIT_DISPLAY_MODE: return glut_mode;
//case GLUT_ELAPSED_TIME:
  case GLUT_WINDOW_BUFFER_SIZE:
    if (glutGet(GLUT_WINDOW_RGBA))
      return glutGet(GLUT_WINDOW_RED_SIZE)+
	glutGet(GLUT_WINDOW_GREEN_SIZE)+
	glutGet(GLUT_WINDOW_BLUE_SIZE)+
	glutGet(GLUT_WINDOW_ALPHA_SIZE);
    else
      return glutGet(GLUT_WINDOW_COLORMAP_SIZE);
  default: {GLint p; glGetIntegerv(type, &p); return p;}
  }
}

int glutLayerGet(GLenum type) {
  switch (type) {
  case GLUT_OVERLAY_POSSIBLE: return glut_window->can_do_overlay();
//case GLUT_LAYER_IN_USE:
//case GLUT_HAS_OVERLAY:
  case GLUT_TRANSPARENT_INDEX: return 0; // true for SGI
  case GLUT_NORMAL_DAMAGED: return glut_window->damage();
  case GLUT_OVERLAY_DAMAGED: return 1; // kind of works...
  default: return 0;
  }
}

#endif

//
// End of "$Id$".
//
