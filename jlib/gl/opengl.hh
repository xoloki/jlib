/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
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
