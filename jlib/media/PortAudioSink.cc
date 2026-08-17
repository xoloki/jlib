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

#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/Type.hh>

#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace jlib {
namespace media {

namespace {

/**
 * Pa_Initialize()/Pa_Terminate() are global and reference counted by
 * PortAudio itself, but they still have to be paired.  Tying them to the
 * lifetime of a function-local static means initialization happens once, on
 * first use, and teardown happens at exit -- rather than per sink, which
 * would tear the library down under any other sink still open.
 */
struct PortAudioLibrary {
    PortAudioLibrary() {
        PaError err = Pa_Initialize();
        if(err != paNoError) {
            std::ostringstream o;
            o << "Pa_Initialize failed: " << Pa_GetErrorText(err);
            throw AudioSink::exception(o.str());
        }
    }

    ~PortAudioLibrary() {
        Pa_Terminate();
    }
};

void require_portaudio() {
    static PortAudioLibrary library;
}

}

PortAudioSink::PortAudioSink()
    : m_stream(0),
      m_pa_format(paInt16),
      m_swap(false),
      m_rebias(false)
{
    require_portaudio();
}

PortAudioSink::~PortAudioSink() {
    try {
        close();
    }
    catch(std::exception& e) {
        // A destructor is no place to throw, but staying silent about a
        // device that would not close is worse.
        std::cerr << "jlib::media::PortAudioSink::~PortAudioSink(): "
                  << e.what() << std::endl;
    }
}

void PortAudioSink::throw_pa(const std::string& ctx, PaError err) const {
    std::ostringstream o;
    o << ctx << " failed: " << Pa_GetErrorText(err);

    if(err == paUnanticipatedHostError) {
        const PaHostErrorInfo* info = Pa_GetLastHostErrorInfo();
        if(info != 0 && info->errorText != 0)
            o << " (" << info->errorText << ")";
    }

    throw exception(o.str());
}

void PortAudioSink::config(int samples_per_sec, int channels, int format) {
    // Reconfiguring to the same thing is what jmelody does between every
    // note; tearing the stream down and back up each time would click.
    if(m_configured &&
       samples_per_sec == m_samples_per_sec &&
       channels == m_channels &&
       format == m_format) {
        return;
    }

    close();

    m_configured = false;

    const bool little_endian = (std::endian::native == std::endian::little);

    switch(format) {
    case Type::PCM_U8:
        m_pa_format = paUInt8;  m_swap = false;                 m_rebias = false; break;
    case Type::PCM_S8:
        m_pa_format = paInt8;   m_swap = false;                 m_rebias = false; break;
    case Type::PCM_S16_LE:
        m_pa_format = paInt16;  m_swap = !little_endian;        m_rebias = false; break;
    case Type::PCM_S16_BE:
        m_pa_format = paInt16;  m_swap = little_endian;         m_rebias = false; break;
    // PortAudio has no unsigned 16-bit format, so these become signed with
    // the bias removed.
    case Type::PCM_U16_LE:
        m_pa_format = paInt16;  m_swap = !little_endian;        m_rebias = true;  break;
    case Type::PCM_U16_BE:
        m_pa_format = paInt16;  m_swap = little_endian;         m_rebias = true;  break;
    case Type::PCM_FLOAT32:
        m_pa_format = paFloat32; m_swap = false;                m_rebias = false; break;
    default:
        {
            std::ostringstream o;
            o << "unsupported format 0x" << std::hex << format;
            throw exception(o.str());
        }
    }

    m_samples_per_sec = samples_per_sec;
    m_channels = channels;
    m_format = format;
    m_configured = true;

    start();
}

void PortAudioSink::start() {
    PaDeviceIndex device = Pa_GetDefaultOutputDevice();
    if(device == paNoDevice)
        throw exception("no default output device");

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if(info == 0)
        throw exception("Pa_GetDeviceInfo returned null for the default output device");

    PaStreamParameters out;
    std::memset(&out, 0, sizeof(out));
    out.device = device;
    out.channelCount = m_channels;
    out.sampleFormat = m_pa_format;
    out.suggestedLatency = info->defaultLowOutputLatency;
    out.hostApiSpecificStreamInfo = 0;

    // A null callback selects the blocking interface, which is what lets
    // write() keep the same shape Dsp had.
    PaError err = Pa_OpenStream(&m_stream, 0, &out,
                                static_cast<double>(m_samples_per_sec),
                                m_period, paClipOff, 0, 0);
    if(err != paNoError) {
        m_stream = 0;
        throw_pa("Pa_OpenStream", err);
    }

    err = Pa_StartStream(m_stream);
    if(err != paNoError) {
        Pa_CloseStream(m_stream);
        m_stream = 0;
        throw_pa("Pa_StartStream", err);
    }

    if(std::getenv("JLIB_MEDIA_SINK_DEBUG")) {
        std::cerr << "PortAudioSink: " << info->name
                  << " " << m_samples_per_sec << "Hz"
                  << " " << m_channels << "ch"
                  << " period " << m_period << " frames" << std::endl;
    }
}

const std::string& PortAudioSink::convert(const std::string& pcm, std::string& scratch) const {
    if(!m_swap && !m_rebias)
        return pcm;

    scratch = pcm;

    // Both remaining conversions are 16-bit.
    const std::size_t n = scratch.size() & ~static_cast<std::size_t>(1);
    unsigned char* p = reinterpret_cast<unsigned char*>(&scratch[0]);

    for(std::size_t i = 0; i < n; i += 2) {
        if(m_swap)
            std::swap(p[i], p[i + 1]);

        if(m_rebias) {
            // Unsigned 16 is signed 16 offset by 32768, so flipping the top
            // bit of the host-order value converts between them.
            std::uint16_t v;
            std::memcpy(&v, p + i, 2);
            v ^= 0x8000u;
            std::memcpy(p + i, &v, 2);
        }
    }

    return scratch;
}

void PortAudioSink::write(const std::string& pcm) {
    if(!m_configured)
        throw exception("write() before config()");
    if(m_stream == 0)
        throw exception("write() on a closed sink");

    std::string scratch;
    const std::string& out = convert(pcm, scratch);

    const int frame = get_frame_size();
    const unsigned long frames = out.size() / frame;

    if(frames == 0)
        return;

    PaError err = Pa_WriteStream(m_stream, out.data(), frames);

    // An underrun means we were late with the next period; the audio already
    // played with a gap.  Reporting it as an error would abort playback for
    // something that has already happened and is recoverable.
    if(err == paOutputUnderflowed) {
        if(std::getenv("JLIB_MEDIA_SINK_DEBUG"))
            std::cerr << "PortAudioSink: output underflowed" << std::endl;
        return;
    }

    if(err != paNoError)
        throw_pa("Pa_WriteStream", err);
}

int PortAudioSink::available() const {
    if(m_stream == 0)
        return 0;

    signed long n = Pa_GetStreamWriteAvailable(m_stream);
    if(n < 0)
        throw_pa("Pa_GetStreamWriteAvailable", static_cast<PaError>(n));

    return static_cast<int>(n);
}

int PortAudioSink::queued() const {
    if(m_stream == 0)
        return 0;

    // PortAudio reports free space, not fill.  The device's total buffering
    // is not directly exposed, so derive the queue depth from how much of the
    // window we are holding: anything not free is waiting to be played.
    const int window = m_period * 2;
    const int free = available();

    return (free >= window) ? 0 : (window - free);
}

void PortAudioSink::reset() {
    if(m_stream == 0)
        return;

    // Abort rather than Stop: Stop drains what is queued, which is the
    // opposite of what a seek or a stop button wants.
    PaError err = Pa_AbortStream(m_stream);
    if(err != paNoError && err != paStreamIsStopped)
        throw_pa("Pa_AbortStream", err);

    err = Pa_StartStream(m_stream);
    if(err != paNoError)
        throw_pa("Pa_StartStream", err);
}

void PortAudioSink::drain() {
    if(m_stream == 0)
        return;

    // Pa_StopStream waits for queued audio to finish, which is what
    // SNDCTL_DSP_SYNC did.
    PaError err = Pa_StopStream(m_stream);
    if(err != paNoError && err != paStreamIsStopped)
        throw_pa("Pa_StopStream", err);

    err = Pa_StartStream(m_stream);
    if(err != paNoError)
        throw_pa("Pa_StartStream", err);
}

void PortAudioSink::close() {
    if(m_stream == 0)
        return;

    PaStream* s = m_stream;
    m_stream = 0;   // clear first, so a throw below cannot leave a stale handle

    Pa_StopStream(s);

    PaError err = Pa_CloseStream(s);
    if(err != paNoError)
        throw_pa("Pa_CloseStream", err);
}

bool PortAudioSink::have_output_device() {
    try {
        require_portaudio();
    }
    catch(std::exception&) {
        return false;
    }

    return Pa_GetDefaultOutputDevice() != paNoDevice;
}

std::string PortAudioSink::get_device_name() const {
    PaDeviceIndex device = Pa_GetDefaultOutputDevice();
    if(device == paNoDevice)
        return "none";

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    return (info != 0 && info->name != 0) ? info->name : "unknown";
}

}
}
