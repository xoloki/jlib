/* -*- mode: ObjC++ c-basic-offset: 4 -*-
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

#include <jlib/metal/device.hh>
#include <jlib/metal/impl.hh>

namespace jlib {
namespace metal {

device::device()
    : m_impl(new impl)
{
    m_impl->gpu = MTLCreateSystemDefaultDevice();

    if(m_impl->gpu == nil)
        throw exception("no Metal device");

    m_impl->queue = [m_impl->gpu newCommandQueue];

    if(m_impl->queue == nil)
        throw exception("could not create a command queue");
}

device::~device() = default;

std::shared_ptr<device> device::shared() {
    // Built on first use and kept.  A device and a queue are not free to make
    // and there is no reason for a process to hold two.  weak_ptr rather than
    // a plain static, so a caller that drops the last reference really does
    // release the GPU objects rather than holding them until exit.
    static std::weak_ptr<device> held;

    std::shared_ptr<device> d = held.lock();

    if(!d) {
        d.reset(new device);
        held = d;
    }

    return d;
}

std::string device::name() const {
    return std::string([[m_impl->gpu name] UTF8String]);
}

bool device::unified() const {
    return [m_impl->gpu hasUnifiedMemory];
}

}
}
