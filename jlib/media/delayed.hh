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

#ifndef JLIB_MEDIA_DELAYED_HH
#define JLIB_MEDIA_DELAYED_HH

#include <jlib/media/source.hh>

#include <algorithm>
#include <memory>

namespace jlib {
namespace media {

/**
 * A source that starts later.
 *
 * This is the whole of what it takes to put a sound at a moment, and therefore
 * the whole of what a timeline needs from the sources beneath it: a mixer sums
 * things that all begin at once, so without this the only way to place a note
 * on the third beat is to start rendering it when the third beat arrives, and
 * a mixer has no way to do that.
 *
 * sampler carried its own start offset for a while, which was enough for
 * patterns of recordings and no use at all for a pattern with notes in it,
 * since a voice had no such thing.  One wrapper that delays anything is both
 * smaller and more useful than the same field on each source, and it is why a
 * playlist can now sound an instrument as easily as a drum.
 *
 * The wait counts as rendered.  Those frames exist -- they are simply empty --
 * and a caller told otherwise would stop early and lose everything after them.
 */
class delayed : public source {
public:
    /**
     * @param s       what to sound, once the wait is over
     * @param frames  how long to wait, in output frames
     */
    delayed(const std::shared_ptr<source>& s, unsigned long frames = 0)
        : m_source(s),
          m_start(frames)
    {
    }

    const std::shared_ptr<source>& get_source() const { return m_source; }

    unsigned long get_start() const { return m_start; }
    void set_start(unsigned long frames) { m_start = frames; }

    virtual unsigned long render(Type::scaled* out, unsigned long frames,
                                 unsigned int channels)
    {
        if(!m_source)
            return 0;

        unsigned long made = 0;

        if(m_waited < m_start) {
            // Nothing is written here.  render() adds, so silence is what the
            // buffer already holds, and zeroing it would erase whatever else
            // has been mixed in.
            const unsigned long wait = std::min(frames, m_start - m_waited);

            m_waited += wait;
            made += wait;
        }

        if(made < frames)
            made += m_source->render(out + made * channels, frames - made, channels);

        return made;
    }

    virtual bool done() const
    {
        return m_waited >= m_start && (!m_source || m_source->done());
    }

    virtual void reset()
    {
        m_waited = 0;

        if(m_source)
            m_source->reset();
    }

protected:
    std::shared_ptr<source> m_source;

    unsigned long m_start = 0;
    unsigned long m_waited = 0;
};

}
}

#endif // JLIB_MEDIA_DELAYED_HH
