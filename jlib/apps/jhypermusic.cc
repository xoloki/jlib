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

  What is here now is both halves, live rather than muxed to a file.  Each vertex
  of an n-cube holds a sustaining voice; its pitch comes from its hue and its
  level from how near it is, and both move every control block as the shape
  turns.  The same corners are drawn in the same hues, sized by the same levels,
  so what is heard is what is seen -- a bright fat corner is a loud near one.

  The audio owns the clock, because it is the one that cannot be late: geometry
  advances inside render(), on the thread feeding the device, and the window
  draws whatever pose it finds.  A dropped video frame is invisible; a dropped
  audio block is a hole.

  The pieces it needs are the ones the last few branches added: voices that can
  be retuned while sounding without clicking, a mixer whose faders can move
  without zipper noise, gain staging that holds the level as the voice count
  changes, and a limiter that stays out of the way.
 */

#include <jlib/apps/color.hh>

#include <jlib/glfw/Plot.hh>

#include "Hyper.hh"

#include <jlib/math/matrix.hh>

#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/mixer.hh>
#include <jlib/media/source_stream.hh>
#include <jlib/media/voice.hh>
#include <jlib/media/wavetable.hh>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
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

    hypermusic(unsigned int d, double rate, std::size_t voices, double spin,
               double doppler)
        : m_shape(d),
          m_rate(rate),
          m_spin(math::matrix<double>::identity(d + 1))
    {
        // Rate first; the planes are built from it below.
        m_step = spin * CONTROL / m_rate;
        m_doppler = doppler;

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

        // Less headroom than the mixer's default, which is unusual and is
        // because these levels are already attenuated: distance puts the
        // typical corner near 0.4 rather than near 1, so the sum arrives some
        // 8dB below what the default is holding room for.  Not none, though --
        // every pitch here comes from one pentatonic scale, so the voices are
        // harmonically related and sum closer to linearly than 1/sqrt(N)
        // assumes.  Measured idle at every dimension from four to twelve.
        m_mix.set_headroom(1);
        m_mix.set_max_voices(voices);

        m_at.reserve(m_shape.size());
        m_was.reserve(m_shape.size());

        for(uint i = 0; i < m_shape.size(); i++) {
            // The same hue the wireframe is drawn in.  HyperPlot colours
            // vertex i with golden_hue(i), so taking the pitch from the same
            // function is what makes "pitch is reflective of colour" true of
            // this program rather than merely intended: the corner you hear
            // high is the corner you see violet, because it is one number.
            const double hue = jlib::apps::golden_hue<double>(i);

            auto v = std::make_shared<media::voice>(inst, pitch(hue), rate);

            // Tables, not summing.  A 10-cube is 1024 voices, which is 19.5% of
            // a core from tables and hopeless without them.  Built up front:
            // building one allocates, and the feeder thread should not.
            v->set_quality(media::voice::quality::fast, &m_tables);

            m_hue.push_back(hue);
            m_voices.push_back(v);
            m_mix.add(v, 0.0);

            // Constructed fresh, then assigned -- never copy-constructed from
            // the shape.  See own() below: a copied vertex shares its storage,
            // and two corners sharing one buffer is how the Doppler came out as
            // exactly zero.
            m_at.push_back(math::vertex<double>(d));
            m_was.push_back(math::vertex<double>(d));
            m_shown.push_back(math::vertex<double>(d));

            m_at.back() = m_shape[i];
            m_was.back() = m_shape[i];
            m_shown.back() = m_shape[i];

            m_level.push_back(0);
            m_level_shown.push_back(0);
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

        // How near a corner can ever get, which is what the level is referred
        // to.  A rotation preserves length, so every corner stays on a sphere
        // of the shape's radius, and the closest approach is the listener's
        // distance less that radius -- reached when a corner swings onto the
        // axis they stand on.
        //
        // This was the nearest corner in the *starting* pose, which is a good
        // deal further out, so anything that came closer later had its gain
        // clamped at full: a fifth of every level was pinned to 1.0, and the
        // corners that should have been the most dramatic were the flattest.
        m_near = std::max(0.1, m_listen - radius);

        // Every plane, as the other hyper apps do.  r and f change it.
        set_planes(d);

        std::cout << "  " << m_shape.size() << " vertices in " << d
                  << " dimensions, sounding at most " << voices
                  << ", turning in " << (m_planes * (m_planes - 1)) / 2
                  << " planes\n"
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

    virtual bool done() const
    {
        return m_stop.load(std::memory_order_acquire) ||
               (m_total > 0 && m_made >= m_total);
    }

    /** Finish early, so closing the window ends the piece. */
    void stop() { m_stop.store(true, std::memory_order_release); }

    uint get_planes() const { return m_planes; }

    /**
     * Turn in every plane spanned by the first r axes.
     *
     * Two planes was not enough and was the reason a high-dimensional figure
     * looked flat: rotating only (0,1) and (2,3) leaves every axis from the
     * fifth up fixed, so an 8-cube tumbled as though it were a 4-cube with
     * sixteen extra corners painted on.  Every plane below r turns instead,
     * each at its own rate, which is how HyperPlot::initialize_rotation has
     * always done it -- sin(d++) gives each plane a different angle, so nothing
     * moves in step with anything else and the figure everts.
     *
     * The audio holds the geometry, so this is where r and f have to land.  The
     * matrix is rebuilt here rather than per block: it is C(r,2) multiplies,
     * which is 45 of them at ten dimensions and none of them per frame.
     */
    void set_planes(uint r)
    {
        m_planes = (r > m_shape.D) ? m_shape.D : r;

        m_spin = math::matrix<double>::identity(m_shape.D + 1);

        int d = 1;

        for(uint i = 0; i < m_planes; i++) {
            for(uint j = i + 1; j < m_planes; j++) {
                math::plane p;
                p.i = i;
                p.j = j;

                m_spin *= math::matrix<double>::rotate(m_shape.D, p,
                                                       std::sin(d++) * m_step);
            }
        }
    }

    /**
     * The current pose, for whoever is drawing it.
     *
     * Copied under a lock rather than read in place.  The geometry is written
     * on the thread feeding the device and read on the one drawing, at
     * different rates -- 86 poses a second against 60 frames -- so without this
     * a frame could be drawn from half of one pose and half of the next.  The
     * lock is held for a copy of a few thousand doubles, by a feeder thread
     * that may block; the device callback is nowhere near it.
     */
    void pose(std::vector<math::vertex<double> >& at,
              std::vector<double>& level) const
    {
        std::lock_guard<std::mutex> lock(m_pose);

        // Sized with vertices of their own before anything is assigned, for
        // the same reason: at = m_shown would share, and the drawing thread
        // would be reading the buffers the audio thread is writing.
        while(at.size() < m_shown.size())
            at.push_back(math::vertex<double>(m_shape.D));

        for(std::size_t k = 0; k < m_shown.size(); k++)
            at[k] = m_shown[k];

        level = m_level_shown;
    }

    double hue(std::size_t i) const { return m_hue[i]; }

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
            const double shift = (m_doppler > 0)
                ? m_doppler / (m_doppler + radial) : 1.0;

            m_voices[i]->set_freq(pitch(m_hue[i]) * shift);

            // Nearer is louder, falling off with distance as sound does.  The
            // mixer ramps this rather than stepping it, which is what keeps a
            // corner swinging past from ticking once a control block.
            // Inverse distance, not inverse square.  Intensity falls as
            // 1/d*d; amplitude, which is what a gain multiplies, falls as 1/d,
            // and squaring it here was applying the law to the wrong quantity.
            // It showed up as everything being too quiet -- gains ran 0.05 to
            // 0.9 and the mix peaked at 0.084 -- because the far corners, which
            // are most of them, were attenuated twice over.
            //
            // Referred to how near a corner can ever come rather than to an
            // absolute distance: what carries the sense of something moving
            // past is the ratio between near and far, and the ratio is what
            // stays the same as the figure grows with D.
            const double g = (now > 0) ? (m_near / now) : 1.0;

            m_mix.set_gain(i, (g > 1.0) ? 1.0 : g);
            m_level[i] = (g > 1.0) ? 1.0 : g;

            // What the geometry is actually worth, reported at the end.  The
            // question "is the Doppler doing anything" has a number.
            m_dmin = std::min(m_dmin, now);
            m_dmax = std::max(m_dmax, now);
            m_gmin = std::min(m_gmin, m_level[i]);
            m_gmax = std::max(m_gmax, m_level[i]);
            m_smin = std::min(m_smin, shift);
            m_smax = std::max(m_smax, shift);
            if(g > 1.0) m_clamped++;
            m_counted++;
        }

        {
            std::lock_guard<std::mutex> lock(m_pose);

            // Element by element.  m_shown = m_at assigns the vector, which
            // copy-constructs its elements, which shares their storage -- so
            // the drawn pose would have been the live one under another name
            // and this lock would have been guarding nothing.
            for(std::size_t k = 0; k < m_shown.size() && k < m_at.size(); k++)
                m_shown[k] = m_at[k];

            m_level_shown = m_level;
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
    double m_step = 0;
    uint m_planes = 0;

    /**
     * Sets how much Doppler a given radial speed is worth.
     *
     * Not the speed of sound in anything: the corners move at whatever the spin
     * makes them, so this is the number that decides whether the shift is a
     * shimmer or a siren.  Taste, so it is on the command line -- the default
     * gives about four semitones at four dimensions and ten at eight, where
     * more planes turn and the corners move faster.
     */
    double m_doppler = 12.0;

public:
    double m_dmin = 1e9, m_dmax = 0;
    double m_gmin = 1e9, m_gmax = 0;
    double m_smin = 1e9, m_smax = 0;
    unsigned long m_clamped = 0, m_counted = 0;
protected:

    std::vector<double> m_level;

    mutable std::mutex m_pose;
    std::vector<math::vertex<double> > m_shown;
    std::vector<double> m_level_shown;

    std::atomic<bool> m_stop{false};

    unsigned long m_until = 0;
    unsigned long m_total = 0;
    unsigned long m_made = 0;
    unsigned long m_release_at = 0;
    unsigned long m_fade = 22050;      /**< half a second */
    bool m_letting_go = false;
};


/**
 * The same corners, drawn.
 *
 * A HyperPlot, so the projection, the clip volume, the camera and the keys all
 * come from the code the other hyper apps use -- the first attempt at this
 * subclassed glfw::Plot directly and crashed on the first frame, because
 * setting up an N-dimensional projection is most of what HyperPlot::initialize
 * does and none of it is optional.
 *
 * What it overrides is where the geometry comes from.  HyperPlot turns the
 * shape itself on a timer; here the audio has already turned it, and this draws
 * whatever pose it finds.  Corners are sized and brightened by the level they
 * are sounding at, which is the entire reason to look at it: a fat bright
 * corner is a loud near one.
 */
class hyperview : public HyperPlot<double, jlib::glfw::Plot<double> > {
public:
    typedef HyperPlot<double, jlib::glfw::Plot<double> > base;

    hyperview(hypermusic& music, uint d, uint w, uint h)
        : base(d, std::vector< std::pair<double,double> >(), w, h),
          m_music(music)
    {
        key_press.connect([this](const std::string& k, int x, int y) {
            if(k.empty())
                return;

            // Not HyperPlot's q, which calls exit() and would leave the device
            // being fed by a thread nobody joined.
            if(k[0] == 'q' || k[0] == 27) {
                this->set_should_close(true);
                return;
            }

            // r and f belong to the audio, which owns the geometry.  Left to
            // HyperPlot they rebuild a rotation matrix that nothing applies,
            // since every vertex is overwritten from the pose each frame --
            // which is why they appeared to do nothing at all.
            if(k[0] == 'r' || k[0] == 'f') {
                const uint was = m_music.get_planes();
                const uint now = (k[0] == 'r') ? was + 1 : (was ? was - 1 : 0);

                if(now <= this->D) {
                    m_music.set_planes(now);

                    std::cout << "  turning in "
                              << (now * (now - 1)) / 2 << " planes\n";
                }

                return;
            }

            this->key_pressed(k[0], x, y);
        });

        button_press.connect([this](int b, int x, int y) {
            this->button_pressed(b, 0, x, y);
        });

        timeout.connect([this]() { this->on_timeout(); });

        set_timeout(16000);          // about sixty a second
    }

    /** Take the pose the audio has reached, and show it. */
    void on_timeout()
    {
        m_music.pose(m_at, m_level);

        if(!objects.empty() && !m_at.empty()) {
            math::object<double>& o = **objects.begin();

            for(uint i = 0; i < o.size() && i < m_at.size(); i++)
                o[i] = m_at[i];
        }

        draw();

        // The piece decides when it is over, not the window.
        if(m_music.done())
            set_should_close(true);
    }

    virtual void set_color(const jlib::apps::triple<double>& c)
    {
        glColor3d(c.r, c.g, c.b);
    }

    virtual void draw_point(std::pair<uint,uint> p, uint index)
    {
        shade(index, 1.0);

        // Size as well as brightness: a corner going quiet against a dark
        // background is easy to lose track of, and the point of this is to be
        // able to follow one.
        glPointSize(static_cast<GLfloat>(2.0 + 10.0 * level(index)));

        jlib::glfw::Plot<double>::draw_point(p, index);
    }

    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                           uint i1, uint i2)
    {
        // Dimmer than the corners, so the edges read as the structure holding
        // them and the things actually sounding stay the bright part.
        shade(i1, 0.4);

        jlib::glfw::Plot<double>::draw_line(p1, p2, i1, i2);
    }

protected:
    double level(uint i) const
    {
        return (i < m_level.size()) ? m_level[i] : 0.0;
    }

    /** The vertex's own hue, at a brightness set by how loud it is. */
    void shade(uint i, double scale)
    {
        if(i >= hues.size())
            return;

        const jlib::apps::triple<double> c = jlib::apps::hsv(hues[i]);

        // A floor, so a distant corner dims rather than vanishing: it is still
        // there, and going quiet should look different from being gone.
        const double b = scale * (0.25 + 0.75 * level(i));

        glColor3d(c.r * b, c.g * b, c.b * b);
    }

    hypermusic& m_music;

    std::vector<math::vertex<double> > m_at;
    std::vector<double> m_level;
};

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    unsigned int d = 4;
    double seconds = 20;
    std::size_t voices = 64;
    double spin = 0.6;
    double doppler = 12;
    bool silent = false;          /**< no window, just the sound */
    unsigned int side = 700;

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
        else if((a == "-p" || a == "--doppler") && i + 1 < argc)
            doppler = std::atof(argv[++i]);
        else if(a == "-q" || a == "--silent")
            silent = true;
        else if((a == "-w" || a == "--window") && i + 1 < argc)
            side = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: " << argv[0]
                      << " [-d DIMENSION] [-t SECONDS] [-v VOICES] [-s SPIN]"
                      << " [-p DOPPLER] [-w PIXELS] [-q]\n"
                      << "  an n-cube turning, with a voice at every corner:\n"
                      << "  pitch from its colour, level from how near it is,\n"
                      << "  and both shifted by how fast it is moving away.\n"
                      << "  the same corners are drawn in the same hues, sized\n"
                      << "  by the same levels.  -q leaves the window out.\n"
                      << "  q or escape closes it.\n";
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

        hypermusic music(d, rate, voices, spin, doppler);

        media::source_stream live(&music);
        live.set_samples_per_sec(static_cast<unsigned int>(rate));
        live.set_format(media::Type::PCM_FLOAT32);
        live.set_channels(2);

        media::PortAudioSink dsp;

        music.set_length(static_cast<unsigned long>(rate * seconds));

        std::cout << "  playing for " << seconds << "s\n";

        if(silent) {
            // Exactly what jnote and jmelody do: the sink takes its format from
            // the stream and pulls until the stream says it is finished.
            dsp.play(live);
        }
        else {
            hyperview view(music, d, side, side);

            // The device is fed on its own thread so the main one can draw.
            // play() pulls until the stream ends, and would otherwise hold the
            // thread that has to be running the window.
            std::thread audio([&]() {
                try {
                    dsp.play(live);
                }
                catch(std::exception& e) {
                    std::cerr << "  audio stopped: " << e.what() << std::endl;
                }

                // However it ended -- finished, or failed for want of a device
                // -- the window should not outlive it.
                music.stop();
            });

            view.run();

            // And the other way round: closing the window ends the piece, which
            // ends the stream, which returns play().
            music.stop();

            audio.join();
        }

        std::cout << "  distance " << std::fixed << std::setprecision(2)
                  << music.m_dmin << " to " << music.m_dmax
                  << "   gain " << std::setprecision(3)
                  << music.m_gmin << " to " << music.m_gmax
                  << " (" << (100.0 * music.m_clamped / (music.m_counted ? music.m_counted : 1))
                  << "% clamped)\n"
                  << "  doppler " << std::setprecision(4)
                  << music.m_smin << " to " << music.m_smax
                  << "  = " << std::setprecision(2)
                  << 12 * std::log2(music.m_smax / music.m_smin) << " semitones\n";

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
