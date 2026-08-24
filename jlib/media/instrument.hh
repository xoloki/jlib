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

#ifndef JLIB_MEDIA_INSTRUMENT_HH
#define JLIB_MEDIA_INSTRUMENT_HH

#include <stdexcept>
#include <string>

namespace jlib {
namespace media {

/**
 * What a note sounds like, separately from which note it is.
 *
 * Pitch and duration belong to the note; timbre belongs here, and is meant to
 * be set up once and used by many notes.  A note string can override any of it
 * for itself -- see basic_notebuf::set_note -- but the instrument is where the
 * settings live.
 */
class instrument {
public:
    class exception : public std::runtime_error {
    public:
        exception(const std::string& msg)
            : std::runtime_error("jlib::media::instrument::exception: " + msg) {}
    };

    /**
     * The waveforms, all built by adding harmonics rather than by drawing the
     * shape directly.  See voice.hh for why.
     */
    enum class wave { sine, saw, square, triangle };

    /**
     * Attack, decay, sustain, release: how loud the note is over its life.
     *
     * Attack, decay and release are seconds; sustain is the level held between
     * decay and release, from 0 to 1.  The whole thing replaces the fixed 5ms
     * fade that used to be hardcoded in create_data(), and inherits its job:
     * a note that stops mid-swing clicks, so the envelope has to reach exact
     * silence at both ends whatever the settings say.  See clamped().
     */
    struct envelope {
        double attack  = 0.005;
        double decay   = 0.0;
        double sustain = 1.0;
        double release = 0.005;
    };

    /**
     * Shortest attack or release that still removes the click.
     *
     * The fade this replaced was 5ms, chosen because it is short enough to be
     * inaudible as a volume change and long enough to remove the discontinuity
     * at any frequency.  A zero-length attack or release is honoured up to this
     * and no further.
     */
    static constexpr double MIN_RAMP = 0.005;

    instrument() {}

    wave get_wave() const { return m_wave; }
    void set_wave(wave w) { m_wave = w; }

    const envelope& get_envelope() const { return m_envelope; }
    void set_envelope(const envelope& e) { m_envelope = e; }

    double get_gain() const { return m_gain; }
    void set_gain(double g) { m_gain = g; }

    /**
     * Harmonics to build a waveform from, or 0 for as many as will fit.
     *
     * Fewer is duller: this is the brightness knob, and it falls out of the
     * additive generation for nothing.  The default asks for everything below
     * Nyquist, which is what makes the result band-limited.
     */
    unsigned int get_harmonics() const { return m_harmonics; }
    void set_harmonics(unsigned int n) { m_harmonics = n; }

    /**
     * The envelope with its ramps clamped and, if the note is too short to
     * hold all four segments, its segments scaled to fit.
     *
     * A note shorter than attack+decay+release has no sustain at all, and the
     * segments have to give way proportionally rather than run past the end or
     * be silently truncated.  The ramps keep their minimum in either case,
     * because reaching silence at the ends matters more than the shape.
     */
    envelope clamped(double seconds) const {
        envelope e = m_envelope;

        if(e.attack  < MIN_RAMP) e.attack  = MIN_RAMP;
        if(e.release < MIN_RAMP) e.release = MIN_RAMP;
        if(e.decay   < 0)        e.decay   = 0;

        if(e.sustain < 0) e.sustain = 0;
        if(e.sustain > 1) e.sustain = 1;

        const double want = e.attack + e.decay + e.release;

        if(seconds > 0 && want > seconds) {
            const double scale = seconds / want;

            e.attack  *= scale;
            e.decay   *= scale;
            e.release *= scale;
        }

        return e;
    }

    /** The name a note string uses, e.g. the "saw" in A#@4:2/saw. */
    static wave wave_from_name(const std::string& name) {
        if(name == "sine")     return wave::sine;
        if(name == "saw")      return wave::saw;
        if(name == "square")   return wave::square;
        if(name == "triangle") return wave::triangle;

        throw exception("unknown waveform '" + name +
                        "', must be sine, saw, square or triangle");
    }

    static std::string name_of(wave w) {
        switch(w) {
        case wave::sine:     return "sine";
        case wave::saw:      return "saw";
        case wave::square:   return "square";
        case wave::triangle: return "triangle";
        }

        return "sine";
    }

protected:
    wave m_wave = wave::sine;
    envelope m_envelope;

    /**
     * Overall level.
     *
     * Was a local called vol in create_data(), at 0.666 -- chosen to leave
     * headroom rather than for any musical reason, and not settable.
     */
    double m_gain = 0.666;

    unsigned int m_harmonics = 0;
};

}
}

#endif // JLIB_MEDIA_INSTRUMENT_HH
