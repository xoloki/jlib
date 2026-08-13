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

#ifndef JLIB_MEDIA_PORTAUDIOSINK_HH
#define JLIB_MEDIA_PORTAUDIOSINK_HH

#include <jlib/media/AudioSink.hh>

#include <portaudio.h>

#include <string>

namespace jlib {
namespace media {

/**
 * An AudioSink over PortAudio's blocking API.
 *
 * This replaces Dsp, which drove OSS through /dev/dsp -- an interface that no
 * longer exists on modern Linux and never existed on macOS.  PortAudio's
 * blocking mode maps almost one-to-one onto what Dsp did:
 *
 *      Dsp::write()            ->  Pa_WriteStream()
 *      Dsp::get_frags_used()   ->  queued(), from Pa_GetStreamWriteAvailable()
 *      Dsp::reset()            ->  Pa_AbortStream() + restart
 *      SNDCTL_DSP_SYNC         ->  drain(), via Pa_StopStream()
 *
 * PortAudio has no unsigned 16-bit format and no big-endian formats, so the
 * PCM_U16_* and PCM_*_BE variants are converted on the way through; see
 * write().  Everything else is handed over untouched.
 */
class PortAudioSink : public AudioSink {
public:
    PortAudioSink();
    virtual ~PortAudioSink();

    // Declaring the three-argument override would otherwise hide the
    // inherited config(stream&).
    using AudioSink::config;
    virtual void config(int samples_per_sec, int channels, int format);

    virtual void write(const std::string& pcm);

    virtual int queued() const;
    virtual int available() const;

    virtual void reset();
    virtual void drain();
    virtual void close();

    /**
     * Name of the output device in use, for diagnostics.
     */
    std::string get_device_name() const;

    /**
     * Whether the host has any output device at all.
     *
     * A headless container has none, and there is nothing jlib can do about
     * that -- so tests check this and skip rather than reporting a failure
     * that says more about the machine than the code.
     */
    static bool have_output_device();

protected:
    /**
     * Turn PCM in the configured format into whatever was handed to
     * Pa_OpenStream, byte-swapping and re-biasing as needed.  Returns a
     * reference to pcm itself when no conversion is required.
     */
    const std::string& convert(const std::string& pcm, std::string& scratch) const;

    void start();
    [[noreturn]] void throw_pa(std::string ctx, PaError err) const;

    PaStream* m_stream;
    PaSampleFormat m_pa_format;
    bool m_swap;    // incoming endianness differs from the host
    bool m_rebias;  // incoming is unsigned 16, PortAudio wants signed
};

}
}

#endif //JLIB_MEDIA_PORTAUDIOSINK_HH
