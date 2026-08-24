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

#ifndef JLIB_MEDIA_VOICE_HH
#define JLIB_MEDIA_VOICE_HH

#include <jlib/media/Type.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/source.hh>
#include <jlib/media/wavetable.hh>

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
class voice : public source {
public:
    /**
     * Exact, or fast.
     *
     * exact sums the harmonics every sample: no interpolation, no banding,
     * nothing approximated.  Right for an offline render, where the cost is
     * paid once and quality is the whole point.
     *
     * fast reads a band-limited table -- about a thousand times cheaper, which
     * is the difference between fifty voices and sixteen thousand, at the cost
     * of interpolation error 117dB down and a slightly soft top octave.  Right
     * for anything generating under a deadline.
     *
     * Same instrument, same envelope, same band limiting either way; the only
     * difference is whether the sum is done now or was done earlier.
     */
    enum class quality { exact, fast };

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
        partials_from_pitch();

        // The envelope segments, in samples, worked out once.  gain_at() ran
        // this arithmetic per sample, and it depends on nothing that changes.
        m_attack  = ramp_samples(m_envelope.attack);
        m_decay   = ramp_samples(m_envelope.decay);
        m_release = ramp_samples(m_envelope.release);
    }

    /**
     * A voice with no end, which sounds until it is released.
     *
     * The other constructor makes a note of a known length, which is what a
     * score is made of.  This one is what a keyboard is made of, and what
     * jhypermusic wants: something held while a vertex exists, its pitch and
     * level moving underneath it.
     *
     * The envelope runs attack, decay, then holds at sustain for as long as it
     * takes.  release() starts the last segment, from wherever the level had
     * got to.
     */
    voice(const instrument& i, double freq, double rate)
        : m_instrument(i),
          m_freq(freq),
          m_samples(0),
          m_rate(rate),
          m_envelope(i.get_envelope()),
          m_pos(0),
          m_sustaining(true)
    {
        partials_from_pitch();

        // Not ramp_samples: that clamps to half the note, and there is no note.
        m_attack  = seconds_to_samples(m_envelope.attack);
        m_decay   = seconds_to_samples(m_envelope.decay);
        m_release = seconds_to_samples(m_envelope.release);

        // The ramps still have to reach silence, or releasing clicks.
        if(m_attack  == 0) m_attack  = seconds_to_samples(instrument::MIN_RAMP);
        if(m_release == 0) m_release = seconds_to_samples(instrument::MIN_RAMP);
    }

    unsigned long size() const { return m_samples; }

    bool is_sustaining() const { return m_sustaining; }
    bool is_released() const { return m_released; }

    /**
     * Let a held voice go, starting its release.
     *
     * From wherever the envelope had reached, rather than from the sustain
     * level -- releasing during the attack would otherwise jump up to sustain
     * first, which is a click.  Ignored if it is not held, or already let go.
     */
    void release()
    {
        if(!m_sustaining || m_released)
            return;

        m_release_from = gain_at(m_pos);
        m_released_at = m_pos;
        m_released = true;
    }

    double get_freq() const { return m_freq; }

    /**
     * Retune, while it is sounding.
     *
     * The phase carries across, so this does not click: what changes is how
     * fast the phase advances from here, not where it is.  That is why the
     * oscillator accumulates phase rather than deriving it from the sample
     * index -- see next().
     *
     * The partial count and, in fast quality, the table both depend on the
     * pitch, so both are worked out again here.  In fast quality that is a
     * mutex and a map lookup, which is fine once a frame and would not be once
     * a sample.
     */
    void set_freq(double freq)
    {
        if(freq == m_freq)
            return;

        m_freq = freq;

        partials_from_pitch();
        resolve_table();
    }

    quality get_quality() const { return m_quality; }

    /**
     * @param q     exact or fast
     * @param set   where fast gets its tables; not owned, and it must outlive
     *              this.  Building a table allocates and takes milliseconds, so
     *              a realtime caller should prime() it first.
     */
    void set_quality(quality q, wavetable_set* set = 0)
    {
        m_quality = q;
        m_set = set;
        m_table = 0;

        if(q != quality::fast || set == 0) {
            m_quality = quality::exact;
            m_set = 0;
            return;
        }

        // Resolved once, here, rather than per sample.  Which table serves a
        // voice depends on its waveform and its pitch, and neither changes
        // after construction -- looking it up in oscillator() meant taking a
        // mutex and walking a map for every sample, which cost most of what the
        // table was supposed to save: 45 times faster than summing instead of
        // about a thousand.
        //
        // Building it may allocate, so this is also the point a realtime caller
        // must reach before its deadline starts.
        resolve_table();
    }

    virtual bool done() const
    {
        if(m_sustaining)
            return m_released && m_pos >= m_released_at + m_release;

        return m_pos >= m_samples;
    }

    virtual void reset()
    {
        m_pos = 0;
        m_phase = 0;
        m_released = false;
        m_released_at = 0;
        m_release_from = 0;
    }

    /**
     * Add this voice to a buffer.
     *
     * Block-size independent by construction: everything comes from m_pos,
     * which counts frames since the voice started, so a call boundary is not
     * something the output can notice.  See the note on source.
     */
    virtual unsigned long render(Type::scaled* out, unsigned long frames,
                                 unsigned int channels)
    {
        unsigned long made = 0;

        while(made < frames && !done()) {
            const Type::scaled s = next();

            // Mono, spread across the frame.  A voice has no stereo image of
            // its own; the mixer places it.
            for(unsigned int c = 0; c < channels; c++)
                out[made * channels + c] += s;

            ++made;
        }

        return made;
    }

    /** The next frame, in [-1,1].  Silence once the note is over. */
    Type::scaled next()
    {
        if(done())
            return 0;

        const unsigned long i = m_pos++;

        // Accumulated rather than derived from the sample index.  The index
        // form could not drift, which is why it was written that way, but it
        // also pins the phase to one frequency for the life of the voice --
        // retuning would restart fmod(f*i/rate) somewhere unrelated to where
        // the waveform had got to, and jump.  Accumulating is what makes
        // set_freq() continuous, and the drift it admits is a rounding error
        // per frame: about 1e-13 of a cycle after a minute, in a double.
        const double phase = m_phase;

        if(m_rate > 0) {
            m_phase += m_freq / m_rate;

            if(m_phase >= 1.0)
                m_phase -= std::floor(m_phase);
        }

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

        if(m_table)
            return m_table->at(phase);

        // Summed here rather than duplicated: wavetable::shape is what fills a
        // table, so the exact path and the cached one cannot drift apart.
        return wavetable::shape(m_instrument.get_wave(), phase, m_partials);
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
        static const double pi = 3.14159265358979323846;

        if(m_sustaining) {
            if(m_released) {
                const unsigned long since = i - m_released_at;

                if(m_release == 0 || since >= m_release)
                    return 0;

                return m_release_from * 0.5 * (1.0 + std::cos(pi * since / m_release));
            }

            if(m_attack > 0 && i < m_attack)
                return 0.5 * (1.0 - std::cos(pi * i / m_attack));

            if(m_decay > 0 && i < m_attack + m_decay) {
                const double through = double(i - m_attack) / m_decay;

                return 1.0 + through * (m_envelope.sustain - 1.0);
            }

            return m_envelope.sustain;
        }

        if(m_samples == 0)
            return 0;

        const unsigned long last = m_samples - 1;

        const unsigned long attack  = m_attack;
        const unsigned long decay   = m_decay;
        const unsigned long release = m_release;

        if(attack > 0 && i < attack)
            return 0.5 * (1.0 - std::cos(pi * i / attack));

        // from the end, so the last sample is exactly silent
        const unsigned long from_end = last - i;

        if(release > 0 && from_end < release)
            return m_envelope.sustain * 0.5 * (1.0 - std::cos(pi * from_end / release));

        if(decay > 0 && i < attack + decay) {
            const double through = double(i - attack) / decay;

            return 1.0 + through * (m_envelope.sustain - 1.0);
        }

        return m_envelope.sustain;
    }

    /** Harmonics up to Nyquist for the current pitch, and no further. */
    void partials_from_pitch()
    {
        const double nyquist = m_rate / 2;

        m_partials = (m_freq > 0) ? static_cast<unsigned int>(nyquist / m_freq) : 0;

        const unsigned int asked = m_instrument.get_harmonics();
        if(asked > 0 && asked < m_partials)
            m_partials = asked;
    }

    /** Which band-limited table serves the current pitch, if any does. */
    void resolve_table()
    {
        m_table = (m_quality == quality::fast && m_set)
            ? &m_set->get(m_instrument.get_wave(), m_freq, m_rate)
            : 0;
    }

    unsigned long seconds_to_samples(double seconds) const
    {
        return (seconds > 0 && m_rate > 0)
            ? static_cast<unsigned long>(seconds * m_rate) : 0;
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

    instrument m_instrument;
    double m_freq;
    unsigned long m_samples;
    double m_rate;
    instrument::envelope m_envelope;
    unsigned int m_partials = 0;
    unsigned long m_attack = 0, m_decay = 0, m_release = 0;
    unsigned long m_pos;

    double m_phase = 0;

    bool m_sustaining = false;
    bool m_released = false;
    unsigned long m_released_at = 0;
    double m_release_from = 0;

    quality m_quality = quality::exact;
    wavetable_set* m_set = 0;

    /** Resolved by set_quality; not owned, and null when summing directly. */
    const wavetable* m_table = 0;
};

}
}

#endif // JLIB_MEDIA_VOICE_HH
