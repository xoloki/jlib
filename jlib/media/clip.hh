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

#ifndef JLIB_MEDIA_CLIP_HH
#define JLIB_MEDIA_CLIP_HH

#include <jlib/media/Type.hh>
#include <jlib/media/stream.hh>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace jlib {
namespace media {

/**
 * Recorded audio, decoded once and held in memory.
 *
 * The split between this and sampler is the same one as between wavetable and
 * voice, for the same reasons.  A clip is immutable and shared; a sampler has a
 * position, a gain and a speed.  Sounding one drum hit eight times in a bar is
 * eight samplers over one clip, not eight decodes -- and they can overlap,
 * which is the thing a single stream cannot do, since a stream has one position
 * and rewinding it for the second hit abandons the first.
 *
 * Decoding up front also keeps stream::get_scaled() out of the render path.  It
 * is a virtual call, a switch over the format and a read, per sample per
 * channel, which is not what should be happening while audio is due.
 */
class clip {
public:
    class exception : public std::runtime_error {
    public:
        exception(const std::string& msg)
            : std::runtime_error("jlib::media::clip::exception: " + msg) {}
    };

    clip() {}

    /**
     * Decode the whole of a stream.
     *
     * The stream is rewound first and read to its end; nothing is kept but the
     * samples, so it may be closed or destroyed afterwards.
     */
    explicit clip(stream& s) { load(s); }

    void load(stream& s)
    {
        const int bytes = s.get_bits_per_sample() / 8;
        if(bytes <= 0)
            throw exception("load(): the format says a sample is no bytes");

        m_channels = s.get_channels() > 0 ? s.get_channels() : 1;
        m_rate = s.get_samples_per_sec() > 0 ? s.get_samples_per_sec() : 44100;

        // get_length() is bytes, and a sample is one channel's worth, so this
        // is values rather than frames.
        const unsigned long values =
            static_cast<unsigned long>(s.get_length()) / bytes;

        s.rewind();

        m_data.clear();
        m_data.reserve(values);

        // Bounded by the length *and* by the stream going bad, because a
        // truncated file should give a short clip rather than a stuck loop.
        for(unsigned long i = 0; i < values && s; i++)
            m_data.push_back(s.get_scaled());

        // A partial frame at the end cannot be played and would misalign every
        // channel after it.
        m_data.resize((m_data.size() / m_channels) * m_channels);
    }

    unsigned int channels() const { return m_channels; }
    double rate() const { return m_rate; }
    unsigned long frames() const { return m_data.size() / m_channels; }
    bool empty() const { return m_data.empty(); }

    /** Seconds, at the rate it was recorded. */
    double seconds() const { return m_rate > 0 ? frames() / m_rate : 0; }

    /** One sample.  Out of range reads as silence rather than throwing. */
    Type::scaled at(unsigned long frame, unsigned int channel) const
    {
        if(frame >= frames() || channel >= m_channels)
            return 0;

        return m_data[frame * m_channels + channel];
    }

    /**
     * A sample between two others, for playing at a speed the recording was not
     * made at.
     *
     * Catmull-Rom, matching wavetable, and for the same reason: four
     * multiply-adds buys about two orders of magnitude over linear.  An exact
     * frame index short-circuits, so playing at the recorded speed returns the
     * recorded samples rather than an interpolation of them -- which is what
     * lets a clip round-trip bit-exactly.
     */
    Type::scaled at(double frame, unsigned int channel) const
    {
        const double whole = std::floor(frame);
        const double t = frame - whole;

        if(t == 0)
            return at(static_cast<unsigned long>(whole), channel);

        const long i = static_cast<long>(whole);

        const double a = held(i - 1, channel);
        const double b = held(i,     channel);
        const double c = held(i + 1, channel);
        const double d = held(i + 2, channel);

        return static_cast<Type::scaled>(
            b + 0.5 * t * (c - a +
                 t * (2*a - 5*b + 4*c - d +
                 t * (3*(b - c) + d - a))));
    }

protected:
    /**
     * at(), with the ends held rather than wrapped.
     *
     * A wavetable wraps because a cycle is periodic.  A recording is not: its
     * last sample is not followed by its first, and interpolating as though it
     * were puts a discontinuity at both ends of every clip.
     */
    double held(long frame, unsigned int channel) const
    {
        const long last = static_cast<long>(frames()) - 1;
        if(last < 0)
            return 0;

        return at(static_cast<unsigned long>(
                      frame < 0 ? 0 : (frame > last ? last : frame)), channel);
    }

    std::vector<Type::scaled> m_data;      /**< interleaved */
    unsigned int m_channels = 1;
    double m_rate = 44100;
};

}
}

#endif // JLIB_MEDIA_CLIP_HH
