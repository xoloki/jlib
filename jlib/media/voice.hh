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

#ifndef JLIB_MEDIA_VOICE_HH
#define JLIB_MEDIA_VOICE_HH

#include <jlib/media/Type.hh>
#include <jlib/media/instrument.hh>

#include <cmath>

namespace jlib {
namespace media {

/**
 * One note being played by one instrument.
 *
 * Produces samples in [-1,1] a frame at a time, which is deliberate: rendering
 * a whole note into a buffer is then a loop over this, and so is mixing several
 * of them live.  Nothing here knows about sample formats, channels or streams.
 *
 * ## Why the waveforms are built by adding sine waves
 *
 * The obvious way to make a sawtooth is a ramp, and a square is sign(sin).
 * Both alias badly.  A ramp has energy at every harmonic of the fundamental,
 * without limit, and everything above half the sample rate cannot be
 * represented -- it folds back down and comes out as an inharmonic tone that
 * has nothing to do with the note.  It is quiet at the bottom of the keyboard
 * and unmistakable at the top, and no amount of filtering afterwards removes it,
 * because by then the aliases are indistinguishable from real signal.
 *
 * Adding harmonics up to Nyquist and stopping cannot alias, because nothing
 * above Nyquist is ever generated.  It costs a sin() per harmonic per sample,
 * which is why it is not what a real-time synthesiser does -- but a note here is
 * rendered ahead of being played, so the cost is paid once and is not felt.
 *
 * The harmonic count is also a brightness control, for free: ask for four and
 * the saw is dull, ask for everything and it is bright.
 */
class voice {
public:
    /**
     * @param i        the instrument, copied
     * @param freq     Hz; zero or less is a rest and produces silence
     * @param samples  how long the note is, in frames
     * @param rate     frames per second
     */
    voice(const instrument& i, double freq, unsigned long samples, double rate)
        : m_instrument(i),
          m_freq(freq),
          m_samples(samples),
          m_rate(rate),
          m_envelope(i.clamped(rate > 0 ? samples / rate : 0)),
          m_pos(0)
    {
        // Harmonics up to Nyquist, and no further.  This is the whole of the
        // band limiting: an aliased partial is one that was generated in the
        // first place.
        const double nyquist = m_rate / 2;

        m_partials = (m_freq > 0) ? static_cast<unsigned int>(nyquist / m_freq) : 0;

        const unsigned int asked = m_instrument.get_harmonics();
        if(asked > 0 && asked < m_partials)
            m_partials = asked;
    }

    unsigned long size() const { return m_samples; }
    bool done() const { return m_pos >= m_samples; }
    void reset() { m_pos = 0; }

    /** The next frame, in [-1,1].  Silence once the note is over. */
    Type::scaled next()
    {
        if(done())
            return 0;

        const unsigned long i = m_pos++;

        // Phase from the sample index rather than accumulated, so it cannot
        // drift and so a voice can be restarted exactly.
        const double phase = (m_rate > 0) ? std::fmod(m_freq * i / m_rate, 1.0) : 0.0;

        return static_cast<Type::scaled>(
            gain_at(i) * m_instrument.get_gain() * oscillator(phase));
    }

protected:
    /**
     * The waveform at a phase in [0,1).
     *
     * Each is its Fourier series truncated at m_partials, scaled so that all
     * four have the same RMS as a unit sine -- otherwise changing waveform
     * changes loudness, since a square at a given peak is 3dB louder than a
     * sine at the same peak.  Matching RMS means the peaks differ instead: a
     * saw reaches about 1.22, and with the default gain of 0.666 that is 0.81,
     * still inside full scale with room for the ~9% Gibbs overshoot that a
     * truncated series adds at the discontinuity.
     */
    double oscillator(double phase) const
    {
        if(m_freq <= 0 || m_partials == 0)
            return 0;

        static const double pi = 3.14159265358979323846;
        const double w = 2 * pi * phase;

        switch(m_instrument.get_wave()) {
        case instrument::wave::sine:
            return std::sin(w);

        case instrument::wave::saw: {
            // (2/pi) * sum sin(k w)/k, alternating, peak 1 -> scaled to sine RMS
            double sum = 0;
            for(unsigned int k = 1; k <= m_partials; k++)
                sum += ((k % 2) ? 1.0 : -1.0) * std::sin(k * w) / k;

            return SAW_RMS * (2.0 / pi) * sum;
        }

        case instrument::wave::square: {
            // (4/pi) * sum over odd k of sin(k w)/k
            double sum = 0;
            for(unsigned int k = 1; k <= m_partials; k += 2)
                sum += std::sin(k * w) / k;

            return SQUARE_RMS * (4.0 / pi) * sum;
        }

        case instrument::wave::triangle: {
            // (8/pi^2) * sum over odd k of (-1)^((k-1)/2) sin(k w)/k^2
            double sum = 0;
            for(unsigned int k = 1; k <= m_partials; k += 2) {
                const double sign = (((k - 1) / 2) % 2) ? -1.0 : 1.0;
                sum += sign * std::sin(k * w) / (double(k) * k);
            }

            return TRIANGLE_RMS * (8.0 / (pi * pi)) * sum;
        }
        }

        return 0;
    }

    /**
     * The envelope at a sample index.
     *
     * Worked out in samples rather than seconds so that it is exactly zero at
     * the first and the last one.  That is not tidiness: a note that stops
     * mid-swing drops straight to silence and clicks, which is why the fade
     * this replaced existed, and tests/audio_test.hh asserts first frame equals
     * last frame because of it.
     *
     * Attack and release are raised cosines rather than lines -- same reason
     * the old fade was, no corner at either end.  Decay is linear, as an ADSR's
     * usually is.
     */
    double gain_at(unsigned long i) const
    {
        if(m_samples == 0)
            return 0;

        const double last = (m_samples > 1) ? double(m_samples - 1) : 1.0;

        const unsigned long attack  = ramp_samples(m_envelope.attack);
        const unsigned long decay   = ramp_samples(m_envelope.decay);
        const unsigned long release = ramp_samples(m_envelope.release);

        static const double pi = 3.14159265358979323846;

        if(attack > 0 && i < attack)
            return 0.5 * (1.0 - std::cos(pi * i / attack));

        // from the end, so the last sample is exactly silent
        const unsigned long from_end = static_cast<unsigned long>(last) - i;

        if(release > 0 && from_end < release)
            return m_envelope.sustain * 0.5 * (1.0 - std::cos(pi * from_end / release));

        if(decay > 0 && i < attack + decay) {
            const double through = double(i - attack) / decay;

            return 1.0 + through * (m_envelope.sustain - 1.0);
        }

        return m_envelope.sustain;
    }

    unsigned long ramp_samples(double seconds) const
    {
        if(seconds <= 0 || m_rate <= 0)
            return 0;

        const unsigned long n = static_cast<unsigned long>(seconds * m_rate);

        // Never longer than half the note, or attack and release overlap and
        // neither reaches where it was going.
        return (n > m_samples / 2) ? m_samples / 2 : n;
    }

    /** Peak scaling that gives each waveform the RMS of a unit sine. */
    static constexpr double SAW_RMS      = 1.2247448713915890;   // sqrt(3/2)
    static constexpr double SQUARE_RMS   = 0.7071067811865476;   // 1/sqrt(2)
    static constexpr double TRIANGLE_RMS = 1.2247448713915890;   // sqrt(3/2)

    instrument m_instrument;
    double m_freq;
    unsigned long m_samples;
    double m_rate;
    instrument::envelope m_envelope;
    unsigned int m_partials = 0;
    unsigned long m_pos;
};

}
}

#endif // JLIB_MEDIA_VOICE_HH
