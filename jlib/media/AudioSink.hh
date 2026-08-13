/* -*- mode: C++ c-basic-offset: 4 -*-
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

#ifndef JLIB_MEDIA_AUDIOSINK_HH
#define JLIB_MEDIA_AUDIOSINK_HH

#include <jlib/media/stream.hh>

#include <exception>
#include <memory>
#include <string>

namespace jlib {
namespace media {

/**
 * An output device that accepts interleaved PCM.
 *
 * This is the contract Dsp grew organically against OSS, restated so a
 * backend other than /dev/dsp can satisfy it.  Two things changed in the
 * restatement:
 *
 *  - Buffering is counted in *frames*, not bytes.  A frame is one sample
 *    across every channel.  Dsp's fragments were a byte count with no
 *    relation to frame size, so a fragment boundary could fall in the middle
 *    of a frame and shear the channels apart.
 *
 *  - bits_per_sample is not a parameter.  It is implied by the format, and
 *    Dsp::config() took it only to ignore it.
 */
class AudioSink {
public:
    class exception : public std::exception {
    public:
        exception(std::string msg = "") {
            m_msg = "jlib::media::AudioSink exception" + (msg != "" ? (": " + msg) : "");
        }
        virtual ~exception() noexcept {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }

    protected:
        std::string m_msg;
    };

    typedef std::shared_ptr<AudioSink> ptr;

    AudioSink();
    virtual ~AudioSink();

    /**
     * Configure to match a stream's declared format.
     */
    void config(stream& s);

    virtual void config(int samples_per_sec, int channels, int format) = 0;

    bool is_configured() const;

    int get_samples_per_sec() const;
    int get_channels() const;
    int get_format() const;

    /**
     * Bytes in one frame: one sample for each channel.
     */
    int get_frame_size() const;

    /**
     * Frames handed to the device per write.  Playback latency is roughly
     * this times the number of periods the device keeps queued.
     */
    int get_period() const;
    void set_period(int frames);

    /**
     * Play s to completion.  Blocks until the stream is drained.
     */
    void play(stream& s);

    /**
     * Read n periods from s and write them.  Short reads at end of stream
     * are written as-is.
     */
    void play_frag(stream& s, int n = 1);

    /**
     * Write interleaved PCM in the configured format.  Blocks until the
     * device has taken all of it.
     */
    virtual void write(const std::string& pcm) = 0;

    /**
     * Frames the device has buffered but not yet played.  This is the
     * backpressure signal: keep it near a couple of periods and neither
     * starve nor block.
     */
    virtual int queued() const = 0;

    /**
     * Frames that can be written without blocking.
     */
    virtual int available() const = 0;

    /**
     * Discard whatever is queued and stop immediately.
     */
    virtual void reset() = 0;

    /**
     * Block until everything queued has actually been played.
     */
    virtual void drain() = 0;

    virtual void close() = 0;

protected:
    /**
     * Bytes per sample for a Type::PCM_* value.
     */
    static int format_size(int format);

    int m_samples_per_sec;
    int m_channels;
    int m_format;
    int m_period;
    bool m_configured;
};

}
}

#endif //JLIB_MEDIA_AUDIOSINK_HH
