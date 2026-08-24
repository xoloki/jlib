/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#ifndef JLIB_GL_OPENGL_HH
#define JLIB_GL_OPENGL_HH

/**
 * Platform-neutral OpenGL and GLU includes.
 *
 * Apple ships both inside OpenGL.framework, under different paths from the
 * <GL/*.h> everyone else uses.  Route every GL include through this header
 * rather than spelling the paths at each site.
 *
 * Apple deprecated OpenGL in 10.14.  It still works, but the headers warn
 * loudly, so quiet them here -- jlib's rendering is fixed-function 1.1/1.2 and
 * needs the legacy 2.1 context that Apple still provides.  See the note in
 * jlib/glut/main.cc about never requesting a 3.2+ core profile.
 */

#ifdef __APPLE__

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION 1
#endif

#include <OpenGL/gl.h>
#include <OpenGL/glu.h>

#else

#include <GL/gl.h>
#include <GL/glu.h>

#endif

#endif //JLIB_GL_OPENGL_HH
