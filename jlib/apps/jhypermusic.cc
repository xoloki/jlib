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

/*
  The note this file used to be, kept because it is the specification and it
  waited a long time:

    To convert a jhyper into music, model each vertex as an emmitter of sound at
    a certain frequency (depending on the color assigned to the vertex).  Do so
    in N dimensions.  On render, project down to 3 dimensions.  For each frame,
    consider not only the new position of the vertex, but also its relative
    position in regards to the past position.  That will give us a velocity
    towards the viewer, which will be used for doppler effect corrections to the
    3d sound.

    I never implemented a full analog digital instrument.  I will do that in
    order to make this music full featured, not just crappy sounds sequenced.

    Attack, decay, sustain, release.  Each vertex gets a full ASDR instrument.
    Pitch is random and is reflective of color; purple is high pitch and red is
    low pitch.  Volume and pitch will be determined positionally with Doppler
    corrections.

  What is here now is the audio half of that.  Each vertex of an n-cube holds a
  sustaining voice; its pitch comes from its hue and its level from how near it
  is, and both are moved every control block as the shape turns.  The video half
  -- grabbing frames and muxing a stream -- is not here, and jhardhyper already
  draws the same geometry.

  The pieces it needs are the ones the last few branches added: voices that can
  be retuned while sounding without clicking, a mixer whose faders can move
  without zipper noise, gain staging that holds the level as the voice count
  changes, and a limiter that stays out of the way.
 */

#include <jlib/apps/color.hh>

#include <jlib/math/matrix.hh>

#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/mixer.hh>
#include <jlib/media/source_stream.hh>
#include <jlib/media/voice.hh>
#include <jlib/media/wavetable.hh>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace jlib;

/**
 * An n-cube, turning, heard rather than seen.
 *
 * A source, so it reaches the device the way everything else does: through
 * source_stream and the sink, with the feeder thread pulling on it.  The
 * animation therefore runs from the audio pipeline rather than beside it, which
 * is what the note meant by the two pipelines being driven together -- except
 * that with no video half there is only one, and it is this one.
 */
class hypermusic : public media::source {
public:
    /** Geometry moves this often.  Fine enough to hear as motion, coarse
     *  enough that the trigonometry is nothing next to the synthesis. */
    static const unsigned long CONTROL = 512;

    hypermusic(unsigned int d, double rate, std::size_t voices, double spin)
        : m_shape(d),
          m_rate(rate),
          m_spin(math::matrix<double>::identity(d + 1))
    {
        // Turn in two planes at once, at rates that do not divide each other,
        // so the figure never repeats exactly.
        math::plane a; a.i = 0; a.j = 1;
        math::plane b; b.i = (d > 3) ? 2 : 0; b.j = (d > 3) ? 3 : 1;

        const double per_block = spin * CONTROL / m_rate;

        m_spin = math::matrix<double>::rotate(d, a, per_block) *
                 math::matrix<double>::rotate(d, b, per_block * 0.61803398875);

        media::instrument inst;
        inst.set_wave(media::instrument::wave::saw);

        media::instrument::envelope e;
        e.attack = 0.4;          // slow, so nothing lands as a click
        e.decay = 0.6;
        e.sustain = 0.7;
        e.release = 0.5;
        inst.set_envelope(e);

        m_mix.set_staging(media::mixer::staging::automatic);
        m_mix.set_rate(rate);
        m_mix.set_max_voices(voices);

        m_at.reserve(m_shape.size());
        m_was.reserve(m_shape.size());

        for(uint i = 0; i < m_shape.size(); i++) {
            // Hue spread around the wheel, so the corners are all different
            // colours and therefore all different pitches.
            const double hue = double(i) / m_shape.size();

            auto v = std::make_shared<media::voice>(inst, pitch(hue), rate);

            // Tables, not summing.  A 10-cube is 1024 voices, which is 19.5% of
            // a core from tables and hopeless without them.  Built up front:
            // building one allocates, and the feeder thread should not.
            v->set_quality(media::voice::quality::fast, &m_tables);

            m_hue.push_back(hue);
            m_voices.push_back(v);
            m_mix.add(v, 0.0);

            m_at.push_back(m_shape[i]);
            m_was.push_back(m_shape[i]);
        }

        // The shape's own scale, so the geometry reads the same at any D.
        double radius = 0;
        for(uint i = 0; i < m_shape.size(); i++) {
            double sum = 0;
            for(uint j = 0; j < m_shape.D; j++)
                sum += double(m_shape[i][j]) * double(m_shape[i][j]);
            radius = std::max(radius, std::sqrt(sum));
        }

        m_listen = radius * 1.6;

        // How near a corner actually gets, which is what the level is referred
        // to.  Taken from the starting pose; a corner that swings nearer later
        // simply reaches full gain, which is what the clamp is for.
        const math::vertex<double> here = ear();

        m_near = distance(m_at[0], here);
        for(std::size_t i = 1; i < m_at.size(); i++)
            m_near = std::min(m_near, distance(m_at[i], here));

        std::cout << "  " << m_shape.size() << " vertices in " << d
                  << " dimensions, sounding at most " << voices << "\n"
                  << "  radius " << std::fixed << std::setprecision(2) << radius
                  << ", listener at " << m_listen
                  << ", nearest corner " << m_near << "\n";

        step();
    }

    /**
     * How long to play for, in frames.
     *
     * Given a length so the stream ends, which is what lets the sink drive this
     * the way it drives a note: play() configures itself from the stream and
     * pulls until EOF.  Without one there is nothing to configure the device
     * from and nothing to stop it.
     */
    void set_length(unsigned long frames)
    {
        m_total = frames;

        // Let the voices go early enough that the release finishes inside the
        // length, so the piece fades rather than being cut off.  The envelope
        // has had a release segment since the first branch of this work and
        // nothing has used it until now.
        m_release_at = (m_total > m_fade) ? m_total - m_fade : 0;
    }

    virtual unsigned long render(media::Type::scaled* out, unsigned long frames,
                                 unsigned int channels)
    {
        unsigned long made = 0;

        if(m_total > 0 && m_made >= m_total)
            return 0;

        if(m_total > 0)
            frames = std::min(frames, m_total - m_made);

        while(made < frames) {
            if(m_until == 0) {
                step();
                m_until = CONTROL;
            }

            const unsigned long want = std::min(frames - made, m_until);
            const unsigned long got =
                m_mix.render(out + made * channels, want, channels);

            if(got == 0)
                break;

            made += got;
            m_until -= got;
        }

        m_made += made;

        return made;
    }

    virtual bool done() const { return m_total > 0 && m_made >= m_total; }

    media::mixer& mix() { return m_mix; }

protected:
    /**
     * Hue to pitch: red low, purple high, as the note asks.
     *
     * Snapped to a minor pentatonic across three octaves.  A continuous mapping
     * is the obvious reading of "pitch is reflective of colour" and sounds like
     * a swarm rather than a chord -- sixteen corners at arbitrary frequencies
     * beat against each other at every interval at once.  On a scale the same
     * sixteen are consonant however they land, which is the difference between
     * this being music and being an effect.
     */
    double pitch(double hue) const
    {
        static const int scale[] = { 0, 3, 5, 7, 10 };
        static const int steps = 5;

        const int octaves = 3;
        const int n = int(hue * steps * octaves);

        const int semitone = scale[n % steps] + 12 * (n / steps);

        return 55.0 * std::pow(2.0, semitone / 12.0);      // from A1
    }

    /**
     * Where the listener stands, on the third axis if there is one.
     *
     * Set from the size of the shape rather than fixed.  A cuboid's corners are
     * at +-1 on every axis, so its radius is sqrt(D) and grows without bound --
     * at a fixed distance of 2.5 a 4-cube sits comfortably in front of the
     * listener and a 10-cube, radius 3.16, has swallowed them.
     */
    math::vertex<double> ear() const
    {
        math::vertex<double> e(m_shape.D);

        for(uint i = 0; i < m_shape.D; i++)
            e[i] = 0;

        e[(m_shape.D > 2) ? 2 : m_shape.D - 1] = m_listen;

        return e;
    }

    static double distance(const math::vertex<double>& a,
                           const math::vertex<double>& b)
    {
        double sum = 0;

        for(uint i = 0; i < a.D && i < b.D; i++) {
            const double d = double(a[i]) - double(b[i]);
            sum += d * d;
        }

        return std::sqrt(sum);
    }

    /** Turn the shape, then retune and re-level every voice from where it is. */
    void step()
    {
        if(m_release_at > 0 && m_made >= m_release_at && !m_letting_go) {
            m_letting_go = true;

            for(std::size_t i = 0; i < m_voices.size(); i++)
                m_voices[i]->release();
        }

        const math::vertex<double> here = ear();
        const double seconds = CONTROL / m_rate;

        for(uint i = 0; i < m_shape.size(); i++) {
            m_was[i] = m_at[i];
            m_at[i] = turn(m_at[i]);

            const double now = distance(m_at[i], here);
            const double before = distance(m_was[i], here);

            // Away from the listener is positive, so it flattens.
            const double radial = (now - before) / seconds;

            // Doppler.  SPEED is not the speed of sound in anything; it is
            // whatever makes the shift audible at the rate these corners move.
            const double shift = SPEED / (SPEED + radial);

            m_voices[i]->set_freq(pitch(m_hue[i]) * shift);

            // Nearer is louder, falling off with distance as sound does.  The
            // mixer ramps this rather than stepping it, which is what keeps a
            // corner swinging past from ticking once a control block.
            // Inverse square, referred to how near the nearest corner gets
            // rather than to an absolute distance.  1/(1+d*d) is the tidier
            // formula and it made everything quiet: every corner of a 4-cube is
            // between 2.3 and 3.9 away, so every gain came out near 0.1 and the
            // mix peaked at 0.111 with the limiter idle and nothing to do.
            // What matters here is the ratio between near and far, which is
            // what carries the sense of something moving past.
            const double g = (now > 0) ? (m_near / now) * (m_near / now) : 1.0;

            m_mix.set_gain(i, (g > 1.0) ? 1.0 : g);
        }
    }

    math::vertex<double> turn(const math::vertex<double>& v) const
    {
        const uint d = m_shape.D;

        math::vertex<double> out(d);

        // Homogeneous: the rotation is (d+1) square with the last row and
        // column carrying the translation this has none of.
        for(uint r = 0; r < d; r++) {
            double sum = m_spin(r, d);

            for(uint c = 0; c < d; c++)
                sum += m_spin(r, c) * double(v[c]);

            out[r] = sum;
        }

        return out;
    }


    /** Sets how much Doppler a given radial speed is worth. */
    static constexpr double SPEED = 12.0;

    math::cuboid<double> m_shape;
    double m_rate;
    math::matrix<double> m_spin;

    std::vector<math::vertex<double> > m_at, m_was;
    std::vector<double> m_hue;

    media::wavetable_set m_tables;
    media::mixer m_mix;
    std::vector<std::shared_ptr<media::voice> > m_voices;

    double m_listen = 0;
    double m_near = 1;

    unsigned long m_until = 0;
    unsigned long m_total = 0;
    unsigned long m_made = 0;
    unsigned long m_release_at = 0;
    unsigned long m_fade = 22050;      /**< half a second */
    bool m_letting_go = false;
};

int main(int argc, char** argv) {
    unsigned int d = 4;
    double seconds = 20;
    std::size_t voices = 64;
    double spin = 0.6;

    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if((a == "-d" || a == "--dimension") && i + 1 < argc)
            d = std::atoi(argv[++i]);
        else if((a == "-t" || a == "--seconds") && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if((a == "-v" || a == "--voices") && i + 1 < argc)
            voices = std::atol(argv[++i]);
        else if((a == "-s" || a == "--spin") && i + 1 < argc)
            spin = std::atof(argv[++i]);
        else {
            std::cerr << "usage: " << argv[0]
                      << " [-d DIMENSION] [-t SECONDS] [-v VOICES] [-s SPIN]\n"
                      << "  an n-cube turning, with a voice at every corner:\n"
                      << "  pitch from its colour, level from how near it is,\n"
                      << "  and both shifted by how fast it is moving away.\n";
            return (a == "-h" || a == "--help") ? 0 : 1;
        }
    }

    if(d < 2 || d > 16) {
        std::cerr << "dimension must be between 2 and 16\n";
        return 1;
    }

    try {
        const double rate = 44100;

        std::cout << "jhypermusic\n";

        hypermusic music(d, rate, voices, spin);

        media::source_stream live(&music);
        live.set_samples_per_sec(static_cast<unsigned int>(rate));
        live.set_format(media::Type::PCM_FLOAT32);
        live.set_channels(2);

        media::PortAudioSink dsp;

        music.set_length(static_cast<unsigned long>(rate * seconds));

        std::cout << "  playing for " << seconds << "s\n";

        // Exactly what jnote and jmelody do: the sink takes its format from the
        // stream and pulls until the stream says it is finished.
        dsp.play(live);

        std::cout << "  peak " << std::fixed << std::setprecision(3)
                  << music.mix().peak()
                  << ", limiter " << 20 * std::log10(music.mix().reduction())
                  << " dB\n";
    }
    catch(std::exception& e) {
        std::cerr << argv[0] << ": " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
