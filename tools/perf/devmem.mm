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

// What a model costs the GPU, which `/usr/bin/time -l` cannot tell you.
//
// **Peak RSS does not see Metal allocations.** They are device allocations,
// not host pages, so a change that moved 1.4 GB of scratch onto the GPU
// showed up in maximum resident set as a difference of 4.6 MB (#286), and a
// model that shed 3.1 GB of device memory read as 0.24 GB *worse* (#196).
// Both numbers were quoted before anyone noticed the instrument could not
// answer the question.
//
// `currentAllocatedSize` can. This prints it after the model is loaded and
// again after a prefill, because the second number includes whatever scratch
// the multiply path allocated along the way and the first does not.
//
// Usage: jperf-devmem <model.gguf> [columns]
#import <Metal/Metal.h>

#include <jlib/ai/model.hh>
#include <jlib/metal/backend.hh>

#include <cstdio>
#include <cstdlib>

using namespace jlib::ai;

int main(int argc, char** argv)
{
    if(argc < 2) {
        std::fprintf(stderr, "usage: %s <model.gguf> [columns]\n", argv[0]);

        return 2;
    }

    const unsigned int cols = argc > 2 ? unsigned(std::atoi(argv[2])) : 512u;

    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();

    if(gpu == nil) {
        std::fprintf(stderr, "no Metal device\n");

        return 77;
    }

    // Before anything of ours, so the figures are this model's and not the
    // process's.
    const double base = double([gpu currentAllocatedSize]);

    gguf g(argv[1]);

    jlib::metal::backend<_Float16> b;

    auto cf = model<_Float16>::config::from(g);

    model<_Float16> m(b, cf);

    m.load(g);

    const double loaded = double([gpu currentAllocatedSize]) - base;

    std::vector<int> ids(cols, 100);

    m.reserve(cols);

    auto logits = b.make(cf.vocab, cols);

    m.forward(ids, logits);
    b.wait();

    const double after = double([gpu currentAllocatedSize]) - base;

    std::printf("  %s\n", argv[1]);
    std::printf("    loaded                 %6.2f GB\n", loaded / 1e9);
    std::printf("    after a %4u prefill   %6.2f GB   (+%.2f of scratch)\n",
                cols, after / 1e9, (after - loaded) / 1e9);
    std::printf("    device working set     %6.2f GB\n",
                double([gpu recommendedMaxWorkingSetSize]) / 1e9);

    return 0;
}
