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

#ifndef JLIB_MEDIA_SAMPLER_HH
#define JLIB_MEDIA_SAMPLER_HH

#include <jlib/media/clip.hh>
#include <jlib/media/source.hh>

#include <cmath>
#include <memory>

namespace jlib {
namespace media {

/**
 * Plays a clip.
 *
 * A source like any other, which is the point of it: once a recording and a
 * synthesized voice are both sources, a mixer does not know or care which it
 * has, and neither does anything downstream.  That is the whole of what makes
 * samples and instruments mixable.
 *
 * The clip is shared and never modified, so any number of these can play the
 * same recording at once, at different positions, speeds and gains.
 *
 * Starting at a particular moment is delayed's job rather than this one's --
 * see delayed.hh.  It lived here first, which was enough for patterns made of
 * recordings and no use for a pattern with notes in it.
 */
class sampler : public source {
public:
    sampler(const std::shared_ptr<const clip>& c, double gain = 1.0)
        : m_clip(c),
          m_gain(gain)
    {
    }

    const std::shared_ptr<const clip>& get_clip() const { return m_clip; }

    double get_gain() const { return m_gain; }
    void set_gain(double g) { m_gain = g; }

    /**
     * Playback speed, where 1 is as recorded.
     *
     * Pitch and duration together, as on tape and for the same reason: there is
     * no time stretching here, so twice the speed is an octave up and half the
     * length.
     */
    double get_speed() const { return m_speed; }
    void set_speed(double s) { m_speed = s; }

    bool get_looping() const { return m_looping; }
    void set_looping(bool l) { m_looping = l; }

    /**
     * Frames per second wanted out.
     *
     * A clip records its own rate, so playing a 22kHz recording into a 44.1kHz
     * stream is a resample rather than an octave drop.  Nothing else here
     * needs it.
     */
    double get_rate() const { return m_rate; }
    void set_rate(double r) { m_rate = r; }

    /** Where playback has reached, in clip frames. */
    double get_position() const { return m_pos; }
    void set_position(double frame) { m_pos = frame; }

    virtual unsigned long render(Type::scaled* out, unsigned long frames,
                                 unsigned int channels)
    {
        if(!m_clip || m_clip->empty() || channels == 0)
            return 0;

        const double step = advance();
        const unsigned long len = m_clip->frames();

        unsigned long made = 0;

        while(made < frames) {
            if(m_pos >= len) {
                if(!m_looping)
                    break;

                // Subtract rather than reset, so a fractional position carries
                // across the seam instead of being rounded off once a cycle.
                m_pos -= std::floor(m_pos / len) * len;
            }

            for(unsigned int c = 0; c < channels; c++)
                out[made * channels + c] +=
                    static_cast<Type::scaled>(m_gain * sample(c, channels));

            m_pos += step;
            ++made;
        }

        return made;
    }

    virtual bool done() const
    {
        if(!m_clip || m_clip->empty())
            return true;

        return !m_looping && m_pos >= m_clip->frames();
    }

    virtual void reset()
    {
        m_pos = 0;
    }

protected:
    /** Clip frames per output frame: speed, and any difference in rate. */
    double advance() const
    {
        const double r = (m_rate > 0) ? m_rate : 44100;
        return m_speed * (m_clip->rate() / r);
    }

    /**
     * One output channel's worth, whatever the clip happens to have.
     *
     * Fewer channels out than in has to fold rather than drop, or a stereo clip
     * played in mono loses one side entirely -- silently, and only for material
     * that happens to be stereo, which is the sort of thing that gets found
     * late.  More channels out than in repeats, so a mono clip is centred.
     */
    Type::scaled sample(unsigned int channel, unsigned int channels) const
    {
        const unsigned int have = m_clip->channels();

        if(channels >= have)
            return m_clip->at(m_pos, channel % have);

        double sum = 0;
        unsigned int n = 0;
        for(unsigned int c = channel; c < have; c += channels) {
            sum += m_clip->at(m_pos, c);
            ++n;
        }

        return static_cast<Type::scaled>(n ? sum / n : 0);
    }

    std::shared_ptr<const clip> m_clip;

    double m_gain = 1.0;
    double m_speed = 1.0;
    double m_rate = 44100;
    bool m_looping = false;
    double m_pos = 0;
};

}
}

#endif // JLIB_MEDIA_SAMPLER_HH
