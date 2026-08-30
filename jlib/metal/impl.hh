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

namespace jlib {
namespace metal {

struct device::impl {
    id<MTLDevice> gpu = nil;
    id<MTLCommandQueue> queue = nil;
};

}
}

#endif // JLIB_METAL_IMPL_HH
