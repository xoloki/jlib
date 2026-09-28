/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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
 */

#ifndef JLIB_METAL_COMPUTE_HH
#define JLIB_METAL_COMPUTE_HH

#include <jlib/metal/device.hh>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace jlib {
namespace metal {

/**
 * Arbitrary Metal compute: compile MSL source, allocate buffers, dispatch.
 *
 * jlib/metal/tensor.mm already does all of this, but welded to one fixed list
 * of kernels for the inference path.  This is the same machinery with the
 * kernel list taken out, so a caller outside jlib/ai can bring its own source.
 * tensor.mm is deliberately **not** rebased onto this yet: it is 100KB of
 * working, tested code, and mixing that refactor into a performance
 * experiment would leave neither reviewable.
 *
 * **Plain C++ on purpose**, like device.hh: Metal is Objective-C, but a
 * caller in jlib/apps must not have to be, so the ObjC lives behind an opaque
 * pointer and nothing here names it.
 *
 * ## Float, never double
 *
 * MSL has no `double` -- the compiler says so outright.  A caller whose CPU
 * path is `double` is therefore not comparing like with like, and the
 * difference is not noise: see hyper_reduce(), where thirteen chained divides
 * are exactly the shape that compounds a float's shorter mantissa.  Measure
 * the divergence rather than assuming it is small.
 *
 * ## Unified memory, and the ordering that survives it
 *
 * alloc() hands back memory both sides can address, so there is no copy and
 * no explicit transfer.  There is still a synchronisation point: the GPU's
 * writes are visible once the command buffer completes, which is what run()
 * blocks for.  Unified memory removes the copy, not the ordering.
 */
class compute {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::metal::compute exception" + (msg.empty() ? "" : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /**
     * Compile MSL source into a program, or throw with the compiler's text.
     *
     * Compilation is the expensive part -- milliseconds, against microseconds
     * for a dispatch -- so build once and keep the result.  A caller that
     * rebuilds per frame will measure the compiler, not the kernel, and the
     * number will look like the GPU losing.
     */
    static std::shared_ptr<compute> build(const std::string& source);

    ~compute();

    compute(const compute&) = delete;
    compute& operator=(const compute&) = delete;

    /** Memory both the CPU and the GPU can address. */
    class buffer {
    public:
        ~buffer();

        buffer(const buffer&) = delete;
        buffer& operator=(const buffer&) = delete;

        /** Writable from the CPU directly; see the ordering note above. */
        void* data();
        const void* data() const;

        std::size_t size() const;

    private:
        buffer();

        struct impl;
        std::unique_ptr<impl> m_impl;

        friend class compute;
    };

    /** A shared buffer of `bytes`, zeroed. */
    std::shared_ptr<buffer> alloc(std::size_t bytes);

    /**
     * One dispatch, built up by binding arguments in index order.
     *
     * bind() assigns the next buffer index, starting at zero, so the order of
     * the calls must match the `[[buffer(n)]]` attributes in the kernel.  That
     * is a real hazard and Metal will not catch it: binding two same-sized
     * buffers in the wrong order is silently accepted and produces wrong
     * numbers rather than an error.
     */
    class call {
    public:
        ~call();

        // kernel() returns one of these by value, and NRVO still requires an
        // accessible move even where it elides -- which a user-declared
        // destructor suppresses.  Declared, not defaulted: impl is
        // incomplete here.
        call(call&& c);

        call(const call&) = delete;
        call& operator=(const call&) = delete;

        /** Bind a device buffer at the next index. */
        call& bind(const std::shared_ptr<buffer>& b);

        /**
         * Bind a small value inline at the next index, for `constant` args.
         *
         * Copied into the command encoder, so the caller's object need not
         * outlive the call.  Metal caps this at 4KB; anything larger wants a
         * real buffer.
         */
        call& bind(const void* bytes, std::size_t len);

        /**
         * Run over `threads` threads and block until the GPU is done.
         *
         * The grid is not rounded up to the threadgroup size, so a kernel
         * must still guard its own upper bound if `threads` is not a multiple
         * of it -- Metal dispatches the partial group either way.
         */
        void run(std::size_t threads);

    private:
        call();

        struct impl;
        std::unique_ptr<impl> m_impl;

        friend class compute;
    };

    /** A dispatch of the named kernel, or throws if the source has no such. */
    call kernel(const std::string& name);

    /** The device this program was built for. */
    std::shared_ptr<device> gpu() const;

private:
    compute();

    struct impl;
    std::unique_ptr<impl> m_impl;
};

}
}

#endif // JLIB_METAL_COMPUTE_HH
