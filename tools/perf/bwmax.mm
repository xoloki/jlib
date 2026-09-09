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
 *
 */

// What will this device actually move?  Standalone, so nothing about jlib's
// dispatch or tensor shapes is in the way.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <chrono>
#include <cstdio>
#include <vector>

static const char* SRC = R"MSL(
#include <metal_stdlib>
using namespace metal;

// One thread per element, scalar.  This is the shape jlib's kernels use.
kernel void copy_scalar(device const half* a [[buffer(0)]],
                        device half* b [[buffer(1)]],
                        constant uint& n [[buffer(2)]],
                        uint i [[thread_position_in_grid]])
{ if(i < n) b[i] = a[i]; }

// Four at a time.
kernel void copy_vec4(device const half4* a [[buffer(0)]],
                      device half4* b [[buffer(1)]],
                      constant uint& n4 [[buffer(2)]],
                      uint i [[thread_position_in_grid]])
{ if(i < n4) b[i] = a[i]; }

// Read only, so no write traffic at all -- the closest thing to what a
// weight sweep does.
kernel void read_vec4(device const half4* a [[buffer(0)]],
                      device float* out [[buffer(1)]],
                      constant uint& n4 [[buffer(2)]],
                      uint i [[thread_position_in_grid]])
{
    if(i >= n4) return;
    half4 v = a[i];
    // Kept, or the compiler removes the load.
    if(v.x + v.y + v.z + v.w == half(12345.0)) out[0] = 1.0f;
}

// Read only, each thread striding so a threadgroup covers a contiguous run.
kernel void read_grid(device const half4* a [[buffer(0)]],
                      device float* out [[buffer(1)]],
                      constant uint& n4 [[buffer(2)]],
                      uint i [[thread_position_in_grid]],
                      uint w [[threads_per_grid]])
{
    float s = 0.0f;
    for(uint k = i; k < n4; k += w) { half4 v = a[k]; s += float(v.x + v.w); }
    if(s == 12345.0f) out[0] = s;
}
)MSL";

int main() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:SRC]
                                               options:nil error:&e];
        if(!lib) { printf("  compile failed: %s\n", [[e localizedDescription] UTF8String]); return 1; }

        const size_t N = 128u * 1024 * 1024;              // 128 M halves = 256 MB
        id<MTLBuffer> a = [dev newBufferWithLength:N*2 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b = [dev newBufferWithLength:N*2 options:MTLResourceStorageModeShared];
        id<MTLBuffer> o = [dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        printf("  buffer %.0f MB, device %s\n", N*2/1e6, [[dev name] UTF8String]);
        printf("  max threadgroup %lu\n", (unsigned long)[dev maxThreadsPerThreadgroup].width);

        struct { const char* name; double bytes_per_elem; bool vec; bool grid; } runs[] = {
            { "copy_scalar", 4.0, false, false },   // read 2 + write 2
            { "copy_vec4",   4.0, true,  false },
            { "read_vec4",   2.0, true,  false },
            { "read_grid",   2.0, true,  true  },
        };

        for(auto& r : runs) {
            id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:r.name]];
            id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:f error:&e];
            if(!p) { printf("  %s: pipeline failed\n", r.name); continue; }

            const uint32_t n = uint32_t(r.vec ? N/4 : N);
            const NSUInteger tg = p.maxTotalThreadsPerThreadgroup;
            const NSUInteger grid = r.grid ? (256 * tg) : n;

            double best = 1e30;
            for(int rep = 0; rep < 5; rep++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:p];
                [enc setBuffer:a offset:0 atIndex:0];
                [enc setBuffer:(r.bytes_per_elem > 2.0 ? b : o) offset:0 atIndex:1];
                [enc setBytes:&n length:4 atIndex:2];
                [enc dispatchThreads:MTLSizeMake(grid,1,1)
                       threadsPerThreadgroup:MTLSizeMake(tg,1,1)];
                [enc endEncoding];
                const auto t0 = std::chrono::steady_clock::now();
                [cb commit]; [cb waitUntilCompleted];
                const double s = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
                if(s < best) best = s;
            }
            printf("  %-12s %.4fs   %6.0f GB/s\n", r.name, best,
                   double(N) * r.bytes_per_elem / best / 1e9);
        }
    }
    return 0;
}
