/* -*- mode: C++ c-basic-offset: 4  -*-
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

#ifndef JLIB_MEDIA_SOURCE_HH
#define JLIB_MEDIA_SOURCE_HH

#include <jlib/media/Type.hh>

namespace jlib {
namespace media {

/**
 * Something that produces audio.
 *
 * A synthesized voice and a recorded sample are the same kind of thing to
 * whatever is mixing them; only their insides differ.  This is that thing.
 *
 * ## Float, not PCM
 *
 * Sources deal in Type::scaled, nominally [-1,1].  Summing in an integer format
 * overflows, and converting at every hop of a graph loses precision at every
 * hop -- so the format is an output concern, handled once by source_stream.
 * PlayList::render has always worked this way; this generalizes it.
 *
 * ## Additive
 *
 * render() *adds* into the buffer rather than overwriting it, so mixing costs
 * nothing beyond the sources themselves.  The caller zeroes first.
 *
 * ## Block-size independence, which is a requirement and not an accident
 *
 * Rendering N frames in one call must produce exactly the same samples as
 * rendering them in any sequence of smaller calls.  Not nearly the same:
 * identical, bit for bit.
 *
 * That is what makes an offline render reproducible -- bouncing a project has
 * to give the same file every time, and has to match what was heard while it
 * played, even though the live path used whatever block size the device asked
 * for and the render used whatever was convenient.  It is easy to preserve by
 * driving everything from a running frame count, and painful to recover once
 * something has been written that carries state across a block boundary.
 *
 * media_source_test asserts it.
 *
 * ## Channels
 *
 * Interleaved, and the source is told how many to write.  A mono source writes
 * the same value across the frame; a stereo one writes its own.  Carrying it
 * here rather than assuming mono means a stereo sampler is not a special case
 * bolted on later.
 */
class source {
public:
    virtual ~source() {}

    /**
     * Add up to frames frames of interleaved audio into out.
     *
     * @param out       at least frames*channels values, already zeroed or
     *                  holding something to add to
     * @param frames    frames wanted
     * @param channels  values per frame
     * @return          frames actually produced, which is fewer at the end
     */
    virtual unsigned long render(Type::scaled* out, unsigned long frames,
                                 unsigned int channels) = 0;

    /** Nothing further will be produced.  A mixer drops these. */
    virtual bool done() const { return false; }

    /** Back to the beginning, as if nothing had been rendered. */
    virtual void reset() {}
};

}
}

#endif // JLIB_MEDIA_SOURCE_HH
