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

#ifndef JLIB_METAL_IMPL_HH
#define JLIB_METAL_IMPL_HH

// Objective-C++ only, and deliberately not installed: this is the one place
// the Metal types are named, so that device.hh and gemm.hh can stay plain C++
// and be included from anywhere.  Anything including this must be a .mm.

#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include <jlib/metal/device.hh>

#include <string>

namespace jlib {
namespace metal {

struct device::impl {
    id<MTLDevice> gpu = nil;
    id<MTLCommandQueue> queue = nil;
};

/**
 * Why a command buffer failed, as a sentence.
 *
 * `[cmd status] == Error` says only that something went wrong; `[cmd error]`
 * says what, and the two callers used to throw the first and discard the
 * second.  "the command buffer failed" is what an out-of-memory looks like
 * from the outside, and it looks identical to every other cause.
 *
 * Must be called *before* the command buffer is released, which is the reason
 * this is a function rather than a line at each throw: the caller that gets
 * it wrong reads a nil error and reports nothing.
 */
inline std::string command_buffer_error(id<MTLCommandBuffer> cmd) {
    NSError* e = [cmd error];

    if(!e) return "the command buffer failed, and Metal gave no reason";

    std::string what = "the command buffer failed: ";

    what += [[e localizedDescription] UTF8String];

    // The code is worth having as well as the text: MTLCommandBufferError
    // names the limits, and 8 is out-of-memory.
    what += " (" + std::string([[e domain] UTF8String]) + " "
         +  std::to_string(long([e code])) + ")";

    return what;
}

}
}

#endif // JLIB_METAL_IMPL_HH
