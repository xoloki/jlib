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
#include <jlib/sys/ringbuffer.hh>

#include <atomic>
#include <memory>

#include <portaudio.h>

#include <string>

namespace jlib {
namespace media {

/**
 * An AudioSink over PortAudio's callback API.
 *
 * This replaces Dsp, which drove OSS through /dev/dsp -- an interface that no
 * longer exists on modern Linux and never existed on macOS.
 *
 *      write()   ->  a ring buffer
 *      the ring  ->  a callback the device runs when it wants samples
 *
 * It used the blocking interface first, which mapped more directly onto what
 * Dsp did, and was abandoned for a reason worth recording: that interface
 * exposes nothing waitable.  Pa_WriteStream blocks until the device has taken
 * everything, Pa_GetStreamWriteAvailable says what could be written without
 * blocking, and there is no way to wait until that becomes true -- no
 * descriptor, no event.  So the only wait on offer was the write itself, which
 * meant either blocking whichever thread called it, or not blocking and
 * needing a timer to replace the pacing that was lost.  See #60.
 *
 * The callback inverts it.  The device asks when it needs audio, write() fills
 * a ring buffer ahead of it, and when the ring is full write() waits on a
 * counter the callback bumps -- an event the device itself generates, so there
 * is no polling and no sleeping anywhere in the path.
 *
 * The callback runs under a realtime deadline and may not allocate, take a
 * lock, or block.  It does exactly three things: read from the ring, pad with
 * silence if the ring came up short, and bump the counter.
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

    /**
     * Frames waiting in the ring, and room for more, both exact.
     *
     * queued() used to guess.  PortAudio reports free space rather than fill
     * and does not expose the device's total buffering, so it assumed a total
     * and subtracted -- see #37.  The ring is ours, so its fill is a fact.
     *
     * The device holds a little beyond the ring, bounded by the latency it was
     * opened with, which is not included here and cannot be.
     */
    virtual int queued() const;
    virtual int available() const;

    /**
     * Whether the callback has had to emit silence since this was last asked.
     *
     * An underrun means the ring ran dry while the device wanted samples, so
     * there is a gap in what was played.  Reading it clears it.
     */
    bool underran();

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

    /** Open the device.  Does not start it; see resume(). */
    void start();

    /** Start the device if it is not already running.  Idempotent. */
    void resume();
    [[noreturn]] void throw_pa(const std::string& ctx, PaError err) const;

    /** What PortAudio calls; forwards to process(). */
    static int trampoline(const void* in, void* out, unsigned long frames,
                          const PaStreamCallbackTimeInfo* time,
                          PaStreamCallbackFlags status, void* user);

    /**
     * Fill one buffer for the device.  Realtime: no allocation, no locks, no
     * blocking, and it cannot throw.
     */
    int process(void* out, unsigned long frames) noexcept;

    /** Block until the callback has taken something, or the ring is empty. */
    void wait_for_callback() const;

    PaStream* m_stream;
    PaSampleFormat m_pa_format;
    bool m_swap;    // incoming endianness differs from the host
    bool m_rebias;  // incoming is unsigned 16, PortAudio wants signed

    /**
     * Bytes per frame, cached.
     *
     * get_frame_size() computes it from the format, and the callback needs it
     * on every buffer -- cheap, but the callback's budget is the one place
     * worth not spending anything avoidable.
     */
    int m_frame;

    /** Between write() and the callback. */
    std::unique_ptr< jlib::sys::ringbuffer<char> > m_ring;

    /**
     * Bumped by the callback every time it runs.
     *
     * This is what write() and drain() wait on, using C++20's atomic wait,
     * which parks in the kernel rather than spinning or sleeping.  Notifying
     * costs almost nothing when nobody is waiting, which is the common case.
     */
    mutable std::atomic<unsigned> m_ticks;

    /** Set by the callback when it padded with silence. */
    std::atomic<bool> m_underran;
};

}
}

#endif //JLIB_MEDIA_PORTAUDIOSINK_HH
