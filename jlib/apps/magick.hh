/* -*- mode: C++ c-basic-offset: 4  -*-
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

#ifndef JLIB_APPS_MAGICK_HH
#define JLIB_APPS_MAGICK_HH

#include <Magick++.h>

namespace jlib {
namespace apps {

/**
 * How bright a pixel is, on the quantum scale, as Magick::Color::intensity()
 * used to say.
 *
 * That method is gone in ImageMagick 7.1.2, and the three jneural apps all
 * called it in the same line to turn a pixel into a network input.  Shared
 * rather than pasted into each of them, because the three have to agree: they
 * read the same images and a net trained by one is fed by another, so a
 * difference here would show up as a quietly worse classifier rather than as
 * anything that looks like a bug.
 *
 * These are the coefficients MagickCore still uses in GetPixelLuma and in
 * GetPixelIntensity's default Rec709Luminance case
 * (MagickCore/pixel-accessor.h), which is what the old method was computing.
 * Copied from the library rather than recalled, since being a few percent off
 * would be invisible.
 *
 * No gamma encoding: GetPixelLuma does not apply any, and neither did
 * intensity().  Rec709Luma, the sibling that does, is a different measure.
 */
inline double luma(const Magick::Color& c)
{
    return 0.212656 * c.quantumRed() +
           0.715158 * c.quantumGreen() +
           0.072186 * c.quantumBlue();
}

}
}

#endif // JLIB_APPS_MAGICK_HH
