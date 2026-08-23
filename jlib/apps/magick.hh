/* -*- mode: C++ c-basic-offset: 4  -*-
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
