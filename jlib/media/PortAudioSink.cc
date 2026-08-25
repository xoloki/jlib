/* -*- mode: C++ c-basic-offset: 4 -*-
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
      m_rebias(false),
      m_frame(0),
      m_ticks(0),
      m_underran(false),
      m_interrupt(false),
      m_silence(0)
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

    m_frame = get_frame_size();

    // Unsigned 8-bit puts silence at the middle of the range, not at zero.
    m_silence = (m_pa_format == paUInt8) ? 0x80 : 0x00;

    // Eight periods of slack between write() and the device.
    //
    // The ring is the whole of the tolerance for the feeding thread being late:
    // however long it is, that is how long the writer may be delayed before
    // the device runs dry and the callback has to pad with silence.  Eight
    // periods at the device's low-latency default is tens of milliseconds,
    // which absorbs a scheduling hiccup or a slow read without adding latency
    // anyone notices, since audio only sits in it when the writer is ahead.
    m_ring.reset(new jlib::sys::ringbuffer<char>(8 * m_period * m_frame));

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

    // A callback rather than a null one, which is what makes the device drive
    // the transfer instead of a blocking write; see the note on the class.
    PaError err = Pa_OpenStream(&m_stream, 0, &out,
                                static_cast<double>(m_samples_per_sec),
                                m_period, paClipOff, &PortAudioSink::trampoline,
                                this);
    if(err != paNoError) {
        m_stream = 0;
        throw_pa("Pa_OpenStream", err);
    }

    // Deliberately not started here.
    //
    // The device would begin asking for samples with the ring still empty, so
    // the first callbacks would pad with silence -- a real underrun on every
    // single playback, which is both a small gap at the start and enough to
    // make underran() useless as a signal.  write() starts it once there is
    // something to play; see resume().

    if(std::getenv("JLIB_MEDIA_SINK_DEBUG")) {
        std::cerr << "PortAudioSink: " << info->name
                  << " " << m_samples_per_sec << "Hz"
                  << " " << m_channels << "ch"
                  << " period " << m_period << " frames" << std::endl;
    }
}

int PortAudioSink::trampoline(const void*, void* out, unsigned long frames,
                              const PaStreamCallbackTimeInfo*,
                              PaStreamCallbackFlags, void* user) {
    return static_cast<PortAudioSink*>(user)->process(out, frames);
}

int PortAudioSink::process(void* out, unsigned long frames) noexcept {
    char* dst = static_cast<char*>(out);
    const std::size_t want = static_cast<std::size_t>(frames) * m_frame;

    // Everything here has to be safe under a realtime deadline: no allocation,
    // no lock, nothing that can block, and no exception.  A ring read is a
    // couple of atomic loads and a memcpy, and memset cannot fail.
    const std::size_t got = m_ring ? m_ring->read(dst, want) : 0;

    if(got < want) {
        // Silence for the rest.  The ring ran dry while the device wanted
        // samples, so there is a gap either way; filling it with zeroes is the
        // only choice that does not also play whatever was in the buffer.
        std::memset(dst + got, m_silence, want - got);
        m_underran.store(true, std::memory_order_relaxed);
    }

    // Wake anyone waiting for room.  Nearly free when nobody is -- the wait is
    // registered in the atomic, so notify checks and returns.
    m_ticks.fetch_add(1, std::memory_order_release);
    m_ticks.notify_one();

    return paContinue;
}

void PortAudioSink::wait_for_callback() const {
    // Read the counter before deciding to wait, so a callback that runs in
    // between cannot be missed: wait() returns at once when the value has
    // already moved on.
    const unsigned seen = m_ticks.load(std::memory_order_acquire);

    if(m_ring->writable() == 0)
        m_ticks.wait(seen, std::memory_order_acquire);
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

void PortAudioSink::interrupt() {
    m_interrupt.store(true, std::memory_order_release);

    // Bump the counter as well as setting the flag, so a writer already parked
    // in the wait comes back to look at it.
    m_ticks.fetch_add(1, std::memory_order_release);
    m_ticks.notify_all();
}

void PortAudioSink::resume() {
    if(m_stream == 0)
        return;

    // Idempotent: this runs on every write, and only the first one after a
    // stop or an abort has anything to do.
    if(Pa_IsStreamActive(m_stream) == 1)
        return;

    PaError err = Pa_StartStream(m_stream);
    if(err != paNoError && err != paStreamIsNotStopped)
        throw_pa("Pa_StartStream", err);
}

void PortAudioSink::write(const std::string& pcm) {
    if(!m_configured)
        throw exception("write() before config()");
    if(m_stream == 0)
        throw exception("write() on a closed sink");

    std::string scratch;
    const std::string& out = convert(pcm, scratch);

    // Whole frames only, so the ring never holds a fraction of one and the
    // callback can never hand the device a torn frame.
    const std::size_t n = (out.size() / m_frame) * m_frame;

    // A fresh call is not interrupted; only one already in progress can be.
    m_interrupt.store(false, std::memory_order_release);

    std::size_t sent = 0;
    while(sent < n) {
        if(m_interrupt.load(std::memory_order_acquire))
            return;

        sent += m_ring->write(out.data() + sent, n - sent);

        // Start the device once it has something to play, and before waiting on
        // it -- waiting first would wait for a callback that is not running.
        resume();

        if(sent < n)
            wait_for_callback();
    }
}

int PortAudioSink::available() const {
    if(!m_ring)
        return 0;

    return static_cast<int>(m_ring->writable() / m_frame);
}

int PortAudioSink::queued() const {
    if(!m_ring)
        return 0;

    return static_cast<int>(m_ring->readable() / m_frame);
}

bool PortAudioSink::underran() {
    return m_underran.exchange(false, std::memory_order_relaxed);
}

void PortAudioSink::reset() {
    if(m_stream == 0)
        return;

    // Abort rather than Stop: Stop drains what is queued, which is the
    // opposite of what a seek or a stop button wants.
    PaError err = Pa_AbortStream(m_stream);
    if(err != paNoError && err != paStreamIsStopped)
        throw_pa("Pa_AbortStream", err);

    // Only now.  ringbuffer::clear() moves the read index, which belongs to
    // the consumer, and the consumer is the callback -- so it is safe here and
    // nowhere else, because the abort above has stopped it running.
    m_ring->clear();
    m_underran.store(false, std::memory_order_relaxed);

    // Whoever was writing is now writing into a ring nobody is draining.
    interrupt();

    // Left stopped.  Restarting on an empty ring is the startup underrun all
    // over again; the next write() resumes it.
}

void PortAudioSink::drain() {
    if(m_stream == 0)
        return;

    // Two things hold audio now, and both have to finish.
    //
    // First the ring: wait for the callback to take the rest of it, on the same
    // event write() waits on.  Guarded by the stream being active, or a stopped
    // or aborted stream would leave this waiting for a callback that is never
    // going to run.
    while(Pa_IsStreamActive(m_stream) == 1 && !m_ring->empty()) {
        const unsigned seen = m_ticks.load(std::memory_order_acquire);

        if(!m_ring->empty())
            m_ticks.wait(seen, std::memory_order_acquire);
    }

    // Then the device's own buffer, which is what Pa_StopStream waits for.
    PaError err = Pa_StopStream(m_stream);
    if(err != paNoError && err != paStreamIsStopped)
        throw_pa("Pa_StopStream", err);

    // Left stopped, as in reset(): the next write() resumes it.
}

void PortAudioSink::close() {
    if(m_stream == 0)
        return;

    // Let the ring finish before stopping.
    //
    // Pa_StopStream waits for the device but knows nothing about the ring, so
    // stopping with samples still in it would cut the tail off -- which is the
    // last note of a melody, since AudioSink::play() leaves the drain to
    // close() precisely so there is no gap between notes.
    try {
        while(Pa_IsStreamActive(m_stream) == 1 && !m_ring->empty()) {
            const unsigned seen = m_ticks.load(std::memory_order_acquire);

            if(!m_ring->empty())
                m_ticks.wait(seen, std::memory_order_acquire);
        }
    }
    catch(...) {
        // Closing has to happen regardless.
    }

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
