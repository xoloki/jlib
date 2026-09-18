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

// Where a decode step's GPU time actually goes, kernel by kernel.
//
// Instruments cannot answer this on an M5: the device exposes only a
// timestamp counter set, dispatch-boundary sampling is unavailable, and
// "Metal GPU Counters" reports "Selected counter profile is not supported on
// target device". A trace gives command-buffer intervals, and jlib puts ~20
// dispatches in each, so nothing there attributes to a kernel.
//
// So each operation is run on its own at the shapes one TinyLlama layer uses
// while decoding, and timed. Not a profile -- the ops overlap in a real step
// and these do not -- but it is the breakdown, which is the question.
#include <jlib/ai/backend.hh>
#include <jlib/metal/backend.hh>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
using namespace jlib;

static const unsigned D = 2048, HEADS = 32, KV = 4, DH = 64, FF = 5632, CTX = 128;

/**
 * The best of several batches, not the mean of one.
 *
 * A single mean is unusable here: the same unchanged kernel measured 4.6us,
 * 22.1us and 16.0us across three runs, and a mean over one batch cannot tell
 * a real regression from the first-run warm-up.  The minimum is the least
 * contaminated estimate of what the kernel costs -- everything above it is
 * some other load.
 */
template<typename F>
static double timed(F&& f, metal::backend<_Float16>& b, int reps = 200) {
    for(int i = 0; i < 50; i++) f();
    b.wait();

    double best = 1e30;

    for(int batch = 0; batch < 5; batch++) {
        const auto t0 = std::chrono::steady_clock::now();

        for(int i = 0; i < reps; i++) f();

        b.wait();

        const double s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count() / reps;

        if(s < best) best = s;
    }

    return best;
}

int main() {
    metal::backend<_Float16> b;
    std::vector<std::pair<std::string, double> > out;

    auto x1   = b.make(D, 1);
    auto w1   = b.make(D, 1);
    auto y1   = b.make(D, 1);
    auto qs   = b.make(HEADS * DH, 1);
    auto ks   = b.make(KV * DH, 1);
    auto kc   = b.make(KV * DH, CTX);
    auto vc   = b.make(KV * DH, CTX);
    auto sc   = b.make(CTX, HEADS);
    auto pr   = b.make(CTX, HEADS);
    auto ho   = b.make(HEADS * DH, 1);
    auto h1   = b.make(FF, 1);
    auto h3   = b.make(FF, 1);

    out.push_back({ "rms_norm  d_model", timed([&]{ b.rms_norm(x1, w1, y1, 1e-5f); }, b) });
    out.push_back({ "rope      q",       timed([&]{ b.rope(qs, 0, 10000.0f,
                                          ai::rope_layout::interleaved, DH); }, b) });
    out.push_back({ "copy_columns k",    timed([&]{ b.copy_columns(ks, kc, 0); }, b) });
    out.push_back({ "attention_scores",  timed([&]{ b.attention_scores(qs, kc, sc,
                                          HEADS, KV, DH, _Float16(0.125f)); }, b) });
    out.push_back({ "causal_mask",       timed([&]{ b.causal_mask(sc, 0, 1); }, b) });
    out.push_back({ "softmax",           timed([&]{ b.softmax(sc, pr); }, b) });
    out.push_back({ "attention_weighted",timed([&]{ b.attention_weighted(vc, pr, ho,
                                          HEADS, KV, DH); }, b) });
    out.push_back({ "activate  silu ff", timed([&]{ b.activate(ai::activation::silu,
                                          h1, h1); }, b) });
    out.push_back({ "hadamard  ff",      timed([&]{ b.hadamard(h1, h3, h1); }, b) });
    out.push_back({ "add_scaled d_model",timed([&]{ b.add_scaled(_Float16(1), x1, y1); }, b) });
    out.push_back({ "assign    d_model", timed([&]{ b.assign(x1, y1); }, b) });

    // How many columns a prefill pass carries; see the batch table below.
    //
    // Overridable because the crossover between the two q8 multiply paths is a
    // column count, and the path is chosen by a threshold read once per
    // process (`q8_dequant_above()`).  So the sweep that finds the crossover is
    // this program run at each width with the threshold pinned either side --
    // which is also why the widths are not hard-coded into a loop here.
    const unsigned int PREFILL_COLS = [] {
        const char* e = std::getenv("JLIB_PERF_COLS");

        return e ? unsigned(std::atoi(e)) : 512u;
    }();

    // The q8 multiplies, at the seven shapes a layer uses.
    struct { const char* name; unsigned K, N; } mats[] = {
        { "q8 wq   2048->2048", D, D },     { "q8 wk   2048->256",  D, KV*DH },
        { "q8 wv   2048->256",  D, KV*DH }, { "q8 wo   2048->2048", D, D },
        { "q8 gate 2048->5632", D, FF },    { "q8 up   2048->5632", D, FF },
        { "q8 down 5632->2048", FF, D },

        // **The output head, which is the widest matrix a model has and was
        // the one shape this table never carried.** A layer's matrices stop
        // at 5632 columns; a head has one per vocabulary entry -- 32000 for
        // TinyLlama and 152064 for Qwen2.5 -- and it is read through the same
        // multiply_tn as everything else. Nothing here had measured whether
        // the two paths behave the same way 27 times wider.
        { "q8 head 2048->32000",  D, 32000 },
        { "q8 head 2048->152064", D, 152064 }
    };
    for(auto& m : mats) {
        std::vector<char> raw((std::size_t(m.K) * m.N / 32) * 34, 1);
        auto q = b.make_q8_0(m.K, m.N, raw.data(), raw.size());
        auto in = b.make(m.K, 1), o = b.make(m.N, 1);
        out.push_back({ m.name, timed([&]{ b.multiply_tn(q, in, o); }, b) });
    }

    // **The same shapes at a prefill batch, which nothing measured.**
    //
    // Everything above is one column: what a decode step does. Prefill sends
    // hundreds at once, and whether that is faster per token is the whole
    // question behind #286 -- measured, prefill throughput is flat from 64
    // columns to 512, which a batch is supposed to improve.
    //
    // Two rates are printed because they answer different questions. GB/s is
    // against the weights read **once**, which is the least any implementation
    // could move; `jperf-bwmax` says the device does ~130. TFLOP/s is the
    // arithmetic actually required. A kernel far below both is bound by
    // neither, and that is the interesting answer.
    std::printf("\n  q8 multiplies at a prefill batch of %u:\n", PREFILL_COLS);

    for(auto& m : mats) {
        std::vector<char> raw((std::size_t(m.K) * m.N / 32) * 34, 1);
        auto q = b.make_q8_0(m.K, m.N, raw.data(), raw.size());
        auto in = b.make(m.K, PREFILL_COLS), o = b.make(m.N, PREFILL_COLS);


        // timed() answers in seconds -- the table below multiplies by 1e6 to
        // print microseconds, and taking its number for one is how this first
        // reported petaflops.  Fewer reps because one of these is five
        // hundred times the work of the decode shape above.
        const double sec = timed([&]{ b.multiply_tn(q, in, o); }, b, 20);

        const double weights = double(m.K) * m.N * 34.0 / 32.0;
        const double moved = weights + 2.0 * PREFILL_COLS * (m.K + m.N);
        const double flops = 2.0 * m.K * m.N * PREFILL_COLS;

        std::printf("    %-22s %9.1f us   %6.1f GB/s   %5.2f TFLOP/s\n",
                    m.name, sec * 1e6, moved / sec / 1e9, flops / sec / 1e12);
    }

    // **The same shapes unquantised, which is the control.**
    //
    // The q8 kernel does two things a plain one does not: it unpacks
    // thirty-two int8s and a scale per block, and it carries a column tile in
    // registers. If the plain multiply lands at the same rate then neither is
    // the cost and the loop shape is -- which decides whether a rewrite has
    // to stay clever about quantisation or merely has to be a GEMM (#286).
    std::printf("\n  the same shapes unquantised, as a control:\n");

    for(auto& m : mats) {
        auto w = b.make(m.K, m.N);
        auto in = b.make(m.K, PREFILL_COLS), o = b.make(m.N, PREFILL_COLS);

        const double sec = timed([&]{ b.multiply_tn(w, in, o); }, b, 20);

        const double moved = 2.0 * double(m.K) * m.N +
                             2.0 * PREFILL_COLS * (m.K + m.N);
        const double flops = 2.0 * m.K * m.N * PREFILL_COLS;

        std::printf("    %-22s %9.1f us   %6.1f GB/s   %5.2f TFLOP/s\n",
                    m.name, sec * 1e6, moved / sec / 1e9, flops / sec / 1e12);
    }

    double tot = 0;
    for(auto& p : out) tot += p.second;
    printf("  one layer's operations at decode shapes (seq 1, ctx %u)\n\n", CTX);
    printf("  %-22s %10s %8s\n", "kernel", "us", "%");
    for(auto& p : out)
        printf("  %-22s %10.1f %7.1f%%\n", p.first.c_str(), p.second * 1e6,
               100.0 * p.second / tot);
    printf("  %-22s %10.1f\n", "sum", tot * 1e6);
    printf("\n  best of five batches of %d, after fifty warm-up calls.  These do\n"
           "  not overlap and a real step does, so the sum is an upper bound.\n",
           200);
}
