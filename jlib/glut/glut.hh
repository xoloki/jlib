/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_GLUT_GLUT_HH
#define JLIB_GLUT_GLUT_HH

/**
 * Platform-neutral GLUT include.
 *
 * Apple ships GLUT as its own framework; elsewhere it is freeglut under
 * <GL/glut.h>.  jlib uses only the GLUT 3.7 API, with none of freeglut's
 * extensions, so both work unmodified.
 *
 * Apple deprecated GLUT in 10.9.  This whole module is scheduled to be
 * replaced by a GLFW backend, which supports Wayland natively and does not
 * depend on a frozen Apple framework.
 */

#include <jlib/gl/opengl.hh>

#ifdef __APPLE__

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION 1
#endif

#include <GLUT/glut.h>

#else

#include <GL/glut.h>

#endif

#endif //JLIB_GLUT_GLUT_HH
