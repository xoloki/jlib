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

#ifndef JLIB_MEDIA_MIXER_HH
#define JLIB_MEDIA_MIXER_HH

#include <jlib/media/source.hh>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace jlib {
namespace media {

/**
 * Sums sources.
 *
 * Three separate things live here and are deliberately not the same thing:
 *
 * **Per-child gain** is the fader.  It is always available, never automatic,
 * and nothing here overrides it.  Balancing a mix is an artistic decision.
 *
 * **Automatic staging** is a default for when nobody is riding faders --
 * jhypermusic generates a voice per vertex from geometry, and there is no
 * engineer.  It is off unless asked for, because a gain that moves when you add
 * a track is hostile to somebody who has just set a level by ear.
 *
 * **The limiter** is a safety net, not a mixing policy.  It catches peaks that
 * get through; it does not decide the balance.
 *
 * And it reports.  peak(), rms() and reduction() are there so a meter can be
 * built on this, which is the difference between a tool and a black box: a
 * limiter working hard means the gain staging is wrong, and that is only
 * actionable if it can be seen.
 */
class mixer : public source {
public:
    /**
     * How the sum is scaled as sources come and go.
     *
     * none leaves it alone: the sum is the sum, and the levels are whatever the
     * faders say.  Right when somebody is mixing.
     *
     * automatic divides by the square root of the number of children, and then
     * by the headroom.  Uncorrelated sources sum in power rather than
     * amplitude, so N of them give sqrt(N) times the RMS -- dividing by N,
     * which is the intuitive choice, is what makes a dense mix quieter the more
     * you put in it.  See set_headroom() for the rest of it.
     */
    enum class staging { none, automatic };

    void add(const std::shared_ptr<source>& s, double gain = 1.0)
    {
        m_children.push_back(child{s, gain, gain});
    }

    void remove(const std::shared_ptr<source>& s)
    {
        for(std::size_t i = 0; i < m_children.size(); i++) {
            if(m_children[i].s == s) {
                m_children.erase(m_children.begin() + i);
                return;
            }
        }
    }

    void clear() { m_children.clear(); }

    std::size_t size() const { return m_children.size(); }

    double get_gain(std::size_t i) const { return m_children.at(i).gain; }

    /**
     * Move a fader.
     *
     * Takes effect over the next render rather than at the start of it: a gain
     * that steps between blocks is a discontinuity in the output, and a
     * discontinuity is a click.  A live controller setting levels every frame
     * would otherwise produce one per frame, which is the zipper noise that
     * gets blamed on the synthesis.
     *
     * Nothing ramps unless a gain actually changed, so a mix whose faders are
     * not being touched -- an offline render, every test here bar one --
     * renders exactly as it did before this existed.  When one is being moved,
     * the ramp spans whatever block it lands in, so the result depends on the
     * block size.  That is inherent to live control rather than a lapse: there
     * is no single stream to be independent of when something outside is
     * changing the mix as it plays.  Offline work uses the timeline, where the
     * gain is a property of the render and not of when someone moved it.
     */
    void set_gain(std::size_t i, double g) { m_children.at(i).gain = g; }

    staging get_staging() const { return m_staging; }
    void set_staging(staging s) { m_staging = s; }

    /**
     * Room left below the ceiling by automatic staging, in dB.
     *
     * Dividing by sqrt(N) holds the RMS and says nothing about peaks, so
     * without this the sum runs over the ceiling as soon as there are three
     * voices, and the limiter becomes a routine participant rather than a
     * safety net.  That costs more than it looks: the limiter engages on dense
     * material and not on sparse, so a chord comes out quieter than a single
     * note and the level consistency staging exists to provide is lost exactly
     * where it was wanted.
     *
     * A constant works here, which is not obvious and is why it is worth
     * measuring rather than reasoning about.  The worst case for N voices is
     * that they align, giving gain*sqrt(N) and growing without limit -- but
     * alignment stops happening as N rises, and the peak that actually occurs
     * plateaus.  Sine voices at the default gain, staged, unlimited:
     *
     *     N        1      3      8     16     32     64
     *     peak  0.666  1.153  1.572  1.777  1.717  1.678
     *     rms   0.471  0.471  0.469  0.471  0.471  0.472
     *
     * So the whole range from a single note to sixty-four of them needs about
     * 5dB, and any voice count needs no more than that.
     *
     * The default is smaller than 5dB deliberately.  It keeps the limiter out
     * of the path for the sparse material where its engaging would be most
     * audible as inconsistency, and leaves it to do its job on dense material,
     * where it is both rarer and better masked.  Headroom is not free -- it is
     * level, given up -- so this is a balance and not a maximum.  Raise it
     * toward 5dB to keep the limiter idle at any voice count; set it to zero
     * for the loudest possible mix and let the limiter earn its keep.
     */
    double get_headroom() const { return m_headroom; }
    void set_headroom(double db) { m_headroom = db; }

    /**
     * Drop the children that have finished.
     *
     * Explicit, and not something render() does for itself, which matters more
     * than it looks.  Automatic staging divides by the number of children, so
     * if render() reaped as it went the gain would depend on how many happened
     * to finish inside a particular block -- and the mix would then come out
     * differently depending on the block size, which is the one thing the
     * source contract promises it will not do.  An offline bounce would stop
     * matching what was heard.
     *
     * So finished children keep their place until the owner says otherwise,
     * between renders.
     */
    void prune()
    {
        for(std::size_t i = m_children.size(); i > 0; i--) {
            if(m_children[i-1].s->done())
                m_children.erase(m_children.begin() + (i-1));
        }
    }

    /**
     * Sound at most this many children at once, loudest first.
     *
     * Zero means all of them.  A D=14 hypercube is 16384 vertices, which is
     * both more than a core can render and more than anyone can hear as
     * anything but noise -- so this is a musical control as much as a
     * computational one.
     */
    void set_max_voices(std::size_t n) { m_max = n; }
    std::size_t get_max_voices() const { return m_max; }

    // --- metering, since the point is to be able to see what is happening ---

    /** Largest absolute sample since the last read.  Reading clears it. */
    Type::scaled peak() { const Type::scaled p = m_peak; m_peak = 0; return p; }

    /** Root mean square since the last read.  Reading clears it. */
    Type::scaled rms()
    {
        const Type::scaled r = (m_rms_n > 0)
            ? static_cast<Type::scaled>(std::sqrt(m_rms_sum / m_rms_n)) : 0;

        m_rms_sum = 0;
        m_rms_n = 0;

        return r;
    }

    /**
     * Most the limiter pulled down since the last read, as a factor.
     *
     * One means it did nothing.  Anything much below one, for long, means the
     * gain staging wants attention rather than the limiter -- which is the
     * thing a meter is for.
     */
    Type::scaled reduction() { const Type::scaled r = m_reduction; m_reduction = 1; return r; }

    // --- the limiter ---

    bool get_limiting() const { return m_limit; }
    void set_limiting(bool on) { m_limit = on; }

    /** Where it starts working.  Just under full scale by default. */
    double get_ceiling() const { return m_ceiling; }
    void set_ceiling(double c) { m_ceiling = c; }

    /**
     * How long the limiter takes to let go, in seconds.
     *
     * This is not a detail.  A limiter that releases quickly follows the
     * envelope it is riding, and if that envelope repeats at a musical rate the
     * gain repeats with it -- which is heard as tremolo, not as limiting.
     *
     * The default is deliberately slow enough not to track anything in the
     * range a beat or a chord's own beating occupies.  See limit().
     */
    double get_release() const { return m_release; }
    void set_release(double seconds) { m_release = seconds; }

    /**
     * Frames per second, which the limiter needs to turn its release time into
     * a per-sample coefficient.  Nothing else here cares.
     */
    double get_rate() const { return m_rate; }
    void set_rate(double r) { m_rate = r; }

    virtual unsigned long render(Type::scaled* out, unsigned long frames,
                                 unsigned int channels)
    {
        if(m_children.empty())
            return 0;

        m_mix.assign(frames * channels, 0);

        // Loudest first when there is a cap, so what gets dropped is what would
        // have been least audible.
        std::vector<child*> play;
        play.reserve(m_children.size());
        for(child& c : m_children) play.push_back(&c);

        if(m_max > 0 && play.size() > m_max) {
            std::partial_sort(play.begin(), play.begin() + m_max, play.end(),
                              [](const child* a, const child* b) {
                                  return a->gain > b->gain;
                              });
            play.resize(m_max);
        }

        unsigned long most = 0;

        for(child* c : play) {
            m_scratch.assign(frames * channels, 0);

            const unsigned long made = c->s->render(m_scratch.data(), frames, channels);
            if(made > most) most = made;

            if(c->applied == c->gain) {
                for(unsigned long i = 0; i < made * channels; i++)
                    m_mix[i] += static_cast<Type::scaled>(m_scratch[i] * c->gain);
            }
            else {
                // Slide from where the fader was to where it now is, across
                // this block, arriving exactly on the new value at its end.
                const double from = c->applied, to = c->gain;

                for(unsigned long f = 0; f < made; f++) {
                    const double g = from + (to - from) * double(f + 1) / made;

                    for(unsigned int ch = 0; ch < channels; ch++)
                        m_mix[f * channels + ch] +=
                            static_cast<Type::scaled>(m_scratch[f * channels + ch] * g);
                }

                if(made > 0)
                    c->applied = to;
            }
        }

        if(most == 0)
            return 0;

        // From how many are being sounded, which is not the same as how many
        // are still making a noise and not the same as how many there are.
        //
        // Finished children still count, and deliberately: reaping them inside
        // a render would make the divisor depend on where block boundaries fell
        // (see prune()).  But a child the voice cap has excluded is not being
        // rendered at all, and counting it divided a mix by voices that
        // contributed nothing -- a 10-cube in jhypermusic has 1024 corners and
        // sounds 64 of them, so it came out four times quieter than the 8-cube
        // beside it, sounding the same 64.  Eight dB between two dimensions of
        // the same figure, from an argument about book-keeping.
        //
        // play.size() is fixed before any rendering happens and does not depend
        // on the block size, so this keeps what prune() is protecting.
        const double stage = (m_staging == staging::automatic && !play.empty())
            ? std::pow(10.0, -m_headroom / 20.0) /
              std::sqrt(static_cast<double>(play.size()))
            : 1.0;

        const double release = release_coefficient();

        for(unsigned long i = 0; i < most * channels; i++) {
            Type::scaled v = static_cast<Type::scaled>(m_mix[i] * stage);

            if(m_limit)
                v = limit(v, release);

            const Type::scaled a = std::fabs(v);
            if(a > m_peak) m_peak = a;

            m_rms_sum += double(v) * v;
            m_rms_n++;

            out[i] += v;
        }

        return most;
    }

    virtual bool done() const
    {
        for(const child& c : m_children)
            if(!c.s->done()) return false;

        return true;
    }

    virtual void reset()
    {
        for(child& c : m_children) c.s->reset();

        m_gain = 1.0;
        m_peak = 0;
        m_rms_sum = 0;
        m_rms_n = 0;
        m_reduction = 1;
    }

protected:
    /**
     * A limiter, not a compressor.
     *
     * The difference matters and is the reason an earlier attempt at this made
     * everything quiet: a compressor pulls the level down across the board and
     * needs makeup gain to put it back, and with a slow release it never
     * recovers between transients, so one loud moment ducks the next several
     * seconds.
     *
     * This only ever acts above the ceiling, so anything already below it comes
     * through untouched and there is nothing to make up.  It grabs immediately,
     * since a peak that is already through cannot be caught later.
     *
     * Letting go is the part that wants care, and the first version got it
     * wrong by releasing in 20ms.  Automatic staging divides by sqrt(N), which
     * holds the RMS steady and says nothing whatever about peaks: three sine
     * voices at the default gain reach 0.666*sqrt(3) = 1.15 when they drift
     * into phase, so the limiter fires every time they do.  For an equal
     * tempered triad that is once per 11.3Hz -- the difference between the
     * chord's two difference tones, 136.0 and 124.7Hz -- and a 20ms release is
     * quick enough to follow it, so the gain wobbles at 11.3Hz and the chord
     * sounds like it has a tremolo pedal on it.  Measured at 0.90dB, which is
     * audible; a sawtooth hides it only because its peak is high and steady, so
     * the reduction is steady too.
     *
     * Turning the instruments up is not the answer, and the numbers are worth
     * having because it is the obvious thing to try.  On that same triad, going
     * from a gain of 0.573 -- where the chord peaks at 0.992 and the limiter
     * never fires -- all the way to 1.0 moves the output RMS from 0.405 to
     * 0.435.  That is 0.6dB of level, bought with 4.2dB of limiting and 0.87dB
     * of wobble.  Past the ceiling, input gain buys distortion and not loudness,
     * because the ceiling is where the output was going to sit regardless.
     *
     * Releasing over a quarter of a second instead puts the gain movement well
     * below anything musical: 0.23dB at 11.3Hz, for 0.5dB of level.  That is
     * not the compressor failure this is written to avoid -- the reduction
     * here is bounded by how far over the ceiling the signal went, 1.3dB in
     * that case, and it is still zero for anything below it.
     */
    Type::scaled limit(Type::scaled v, double release)
    {
        const double a = std::fabs(v);

        // What the gain would have to be for this sample to fit.
        const double want = (a > m_ceiling) ? (m_ceiling / a) : 1.0;

        if(want < m_gain)
            m_gain = want;                       // instantly
        else
            m_gain += (want - m_gain) * release; // gradually

        if(m_gain < m_reduction)
            m_reduction = static_cast<Type::scaled>(m_gain);

        return static_cast<Type::scaled>(v * m_gain);
    }

    /**
     * The release as a per-sample coefficient.
     *
     * Read once per render rather than per sample, and it must not change
     * within one -- the limiter carries gain from sample to sample, and that
     * state has to advance identically however the frames are divided up, or
     * an offline bounce stops matching what was heard.
     */
    double release_coefficient() const
    {
        return (m_release > 0 && m_rate > 0)
            ? 1.0 - std::exp(-1.0 / (m_release * m_rate))
            : 1.0;
    }

    struct child {
        std::shared_ptr<source> s;
        double gain;      /**< where the fader is */
        double applied;   /**< where it was when this last rendered */
    };

    std::vector<child> m_children;
    std::vector<Type::scaled> m_mix, m_scratch;

    staging m_staging = staging::none;
    std::size_t m_max = 0;

    bool m_limit = true;
    double m_ceiling = 0.99;
    double m_gain = 1.0;
    double m_release = 0.25;
    double m_rate = 44100;
    double m_headroom = 3.0;

    Type::scaled m_peak = 0;
    double m_rms_sum = 0;
    unsigned long m_rms_n = 0;
    Type::scaled m_reduction = 1;
};

}
}

#endif // JLIB_MEDIA_MIXER_HH
