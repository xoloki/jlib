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

// The two numbers a q8 kernel change moves, and nothing else.  bench.cc does
// six model loads and a thousand-step walk; this does one load and two timed
// loops, so it can be run repeatedly while iterating.
#include <jlib/ai/model.hh>
#include <jlib/metal/backend.hh>
#include <chrono>
#include <cstdio>
using namespace jlib::ai;
int main(int c, char** v) {
    gguf g(v[1]);
    jlib::metal::backend<_Float16> b;
    auto cf = model<_Float16>::config::from(g);
    // **The decode model is scoped so it is gone before the prefill one is
    // built.** They used to overlap, and two 8.1 GB copies of Qwen2.5-Coder 7B
    // do not fit in 16 GB: the machine swapped, and every number this prints
    // became a measure of that -- decode included, which no kernel change
    // touches. It reported 1.8 tok/s and a 29s prefill64 for a model that does
    // 11.7 and 1.0s, so the tool was not slow, it was wrong.
    //
    // The comment above always said "one load". Two models is what the code
    // did, and the reason it needs a second one is that the first has a
    // conversation in its KV cache by then.
    double d = 0;
    {
        model<_Float16> m(b, cf); m.load(g); m.enable_cache(160);

        std::vector<int> p(128, 100);
        m.reserve(128); auto l0 = b.make(cf.vocab, 128);
        m.forward(p, l0); b.wait();

        std::vector<int> one(1, 100);
        m.reserve(1); auto l1 = b.make(cf.vocab, 1);
        m.forward(one, l1); b.wait();
        auto t0 = std::chrono::steady_clock::now();
        for(int r = 0; r < 20; r++) { m.forward(one, l1); b.wait(); }
        d = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count() / 20.0;
    }

    model<_Float16> m2(b, cf); m2.load(g);
    std::vector<int> ids(512, 100);
    m2.reserve(512); auto lg = b.make(cf.vocab, 512);
    m2.forward(ids, lg); b.wait();
    auto t0 = std::chrono::steady_clock::now();
    for(int r = 0; r < 3; r++) { m2.forward(ids, lg); b.wait(); }
    const double pf = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count() / 3.0;

    // A short prompt is where the reduction threshold flips: units is
    // N * ceil(ncols/8), so few columns can put a big matrix under it.
    double sp = 0;
    {
        std::vector<int> few(64, 100);
        m2.reserve(64); auto ls = b.make(cf.vocab, 64);
        m2.forward(few, ls); b.wait();
        auto t = std::chrono::steady_clock::now();
        for(int r = 0; r < 10; r++) { m2.forward(few, ls); b.wait(); }
        sp = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t).count() / 10.0;
    }

    printf("  decode %.4fs (%.1f tok/s)   prefill64 %.4fs   prefill512 %.3fs\n",
           d, 1.0/d, sp, pf);
}
