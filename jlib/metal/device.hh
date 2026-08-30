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

#ifndef JLIB_METAL_DEVICE_HH
#define JLIB_METAL_DEVICE_HH

#include <cstddef>
#include <exception>
#include <memory>
#include <string>

namespace jlib {
namespace metal {

/**
 * The GPU, and a queue to give it work.
 *
 * **This header is plain C++ on purpose.**  Metal's API is Objective-C, so
 * everything behind here is compiled as Objective-C++ -- but a caller in
 * jlib/ai or jlib/math must not have to be.  So the Objective-C objects live
 * behind an opaque pointer and nothing in this file mentions them.
 *
 * ## What Metal buys and what it does not
 *
 * Float and half.  **Not double**: Metal Shading Language has no such type,
 * and the compiler says so outright -- "'double' is not supported in Metal".
 * That is why gemm.hh offers float and stops, where jlib/cuda/gemm.hh has a
 * double specialisation on top of cuBLAS.  Any caller that needs fp64 stays
 * on the CPU, and no amount of API design changes that.
 *
 * ## Unified memory
 *
 * On Apple silicon the CPU and GPU address the same physical memory, so a
 * buffer allocated shared is readable from both without a copy or an explicit
 * transfer.  That is what makes small and medium work worth offloading here
 * when it would not be across a PCIe bus, and it is why buffer() hands back a
 * pointer you can simply write to.
 *
 * There is still a synchronisation point: the GPU's writes are visible after
 * the command buffer completes, which is what wait() is for.  Unified memory
 * removes the copy, not the ordering.
 */
class device {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::metal::device exception" + (msg.empty() ? "" : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /**
     * The system default device, or throws if there is none.
     *
     * Shared: a process wants one of these, not one per caller.  Building a
     * device and a command queue is not free and neither is worth repeating.
     */
    static std::shared_ptr<device> shared();

    ~device();

    device(const device&) = delete;
    device& operator=(const device&) = delete;

    /** The GPU's name, for a caller that wants to say which one it found. */
    std::string name() const;

    /** Whether this device shares memory with the CPU rather than copying. */
    bool unified() const;

private:
    device();

    // The Objective-C objects, which this header will not name.  See device.mm.
    struct impl;
    std::unique_ptr<impl> m_impl;

    friend class matrix_multiply;
};

}
}

#endif // JLIB_METAL_DEVICE_HH
