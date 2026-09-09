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

#include <jlib/ai/model.hh>
#include <jlib/metal/backend.hh>
#include <chrono>
#include <iostream>
using namespace jlib::ai;
int main(int c, char** v) {
    gguf g(v[1]);
    jlib::metal::backend<_Float16> b;
    auto base = model<_Float16>::config::from(g);

    std::cout << "  decode step, per layer:\n";
    for(unsigned L : { 1u, 8u, 22u }) {
        auto cf = base; cf.layers = L;
        model<_Float16> m(b, cf); m.load(g); m.enable_cache(256);
        std::vector<int> p(64, 100);
        m.reserve(64); auto l0 = b.make(cf.vocab, 64); m.forward(p, l0); b.wait();
        std::vector<int> one(1, 100);
        m.reserve(1); auto l1 = b.make(cf.vocab, 1); m.forward(one, l1); b.wait();
        const auto t0 = std::chrono::steady_clock::now();
        for(int r = 0; r < 20; r++) { m.forward(one, l1); b.wait(); }
        const double s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count() / 20.0;
        printf("    %2u layers  %.4fs   %.4fs/layer\n", L, s, s / L);
    }

    std::cout << "  decode at depth, full model:\n";
    {
        model<_Float16> m(b, base); m.load(g); m.enable_cache(1200);
        std::vector<int> p(16, 100);
        m.reserve(16); auto l0 = b.make(base.vocab, 16); m.forward(p, l0); b.wait();
        unsigned at = 16;
        for(unsigned target : { 32u, 512u, 1024u }) {
            while(at < target) { std::vector<int> o(1,100); m.reserve(1);
                auto l = b.make(base.vocab,1); m.forward(o,l); b.wait(); at++; }
            std::vector<int> o(1,100); m.reserve(1); auto l = b.make(base.vocab,1);
            const auto t0 = std::chrono::steady_clock::now();
            for(int r = 0; r < 5; r++) { m.forward(o, l); b.wait(); at++; }
            const double s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count() / 5.0;
            printf("    at ctx %4u  %.4fs  (%.1f tok/s)\n", target, s, 1.0/s);
        }
    }

    std::cout << "  prefill:\n";
    for(unsigned n : { 512u, 2048u }) {
        std::vector<int> ids(n, 100);
        model<_Float16> m(b, base); m.load(g);
        m.reserve(n); auto lg = b.make(base.vocab, n);
        m.forward(ids, lg); b.wait();
        const auto t0 = std::chrono::steady_clock::now();
        for(int r = 0; r < 3; r++) { m.forward(ids, lg); b.wait(); }
        printf("    %4u  %.3fs\n", n, std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count() / 3.0);
    }
}
