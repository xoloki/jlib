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

// The GPU multiply, against the CPU one.
//
// math::operator* is the oracle here.  It is naive and slow, which is exactly
// what makes it a good reference: there is nothing clever in it to be wrong in
// the same way twice.
//
// Not compared exactly.  MPS is free to sum in whatever order suits the
// hardware and does, so the last bits differ from a left-to-right CPU loop.
// The tolerance below scales with the interior dimension because that is what
// the error accumulates over.

#include <jlib/metal/gemm.hh>
#include <jlib/metal/device.hh>

#include <jlib/math/matrix.hh>

#include <cmath>
#include <iostream>
#include <random>
#include <string>

namespace metal = jlib::metal;

using jlib::math::matrix;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static matrix<float> random_matrix(uint m, uint n, std::mt19937& gen) {
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);

    matrix<float> a(m, n);

    for(uint r = 0; r < m; r++)
        for(uint c = 0; c < n; c++)
            a(r, c) = d(gen);

    return a;
}

/** Largest absolute difference, and where. */
static double worst(const matrix<float>& a, const matrix<float>& b) {
    double w = 0;

    for(uint r = 0; r < a.M; r++)
        for(uint c = 0; c < a.N; c++)
            w = std::max(w, std::fabs(double(a(r,c)) - double(b(r,c))));

    return w;
}

static void it_agrees_with_the_cpu(metal::matrix_multiply& mm) {
    std::cout << "\nit agrees with the CPU:\n";

    std::mt19937 gen(20260830);

    // Deliberately not square, and deliberately not all the same, because a
    // row/column mix-up is invisible on a square matrix and this code reads
    // one layout as another on purpose.
    const uint shapes[][3] = {
        {  1,   1,   1 },
        {  2,   3,   4 },
        {  4,   3,   2 },
        {  1,  16,   1 },
        { 16,   1,  16 },
        { 32,  64, 128 },
        {200, 784,   1 },   // the shape jlib/ai's backprop actually runs
    };

    for(const uint* s : shapes) {
        const uint M = s[0], K = s[1], N = s[2];

        const matrix<float> a = random_matrix(M, K, gen);
        const matrix<float> b = random_matrix(K, N, gen);

        const matrix<float> cpu = a * b;
        const matrix<float> gpu = mm(a, b);

        const std::string shape = std::to_string(M) + "x" + std::to_string(K) +
            " * " + std::to_string(K) + "x" + std::to_string(N);

        if(gpu.M != M || gpu.N != N) {
            ok(shape + " has the right shape", false,
               std::to_string(gpu.M) + "x" + std::to_string(gpu.N));
            continue;
        }

        const double w = worst(cpu, gpu);

        // Scaled by the interior dimension: that is how many products are
        // summed, and so how far float rounding can drift between two
        // orderings.  1e-6 per term is loose for float and tight enough that
        // a transposed result or a wrong stride would blow straight through.
        const double tol = 1e-6 * K + 1e-6;

        ok(shape, w <= tol,
           "worst |cpu-gpu| = " + std::to_string(w) + ", tol " + std::to_string(tol));
    }
}

static void it_is_not_secretly_transposed(metal::matrix_multiply& mm) {
    std::cout << "\nit is not secretly transposed:\n";

    // The layout argument in gemm.mm reads a column-major matrix as a
    // row-major transpose and swaps the operands.  If that reasoning is wrong
    // the result comes back transposed, and on a square matrix of random
    // numbers that is exactly as plausible as being right.  So: an asymmetric
    // product whose answer is known by hand.
    matrix<float> a(2, 3);
    a(0,0) = 1; a(0,1) = 2; a(0,2) = 3;
    a(1,0) = 4; a(1,1) = 5; a(1,2) = 6;

    matrix<float> b(3, 2);
    b(0,0) = 7;  b(0,1) = 8;
    b(1,0) = 9;  b(1,1) = 10;
    b(2,0) = 11; b(2,1) = 12;

    const matrix<float> c = mm(a, b);

    // [1 2 3; 4 5 6] * [7 8; 9 10; 11 12] = [58 64; 139 154]
    ok("the shape is 2x2", c.M == 2 && c.N == 2,
       std::to_string(c.M) + "x" + std::to_string(c.N));

    ok("c(0,0) = 58", std::fabs(c(0,0) - 58.0f) < 1e-3, std::to_string(c(0,0)));
    ok("c(0,1) = 64", std::fabs(c(0,1) - 64.0f) < 1e-3, std::to_string(c(0,1)));
    ok("c(1,0) = 139", std::fabs(c(1,0) - 139.0f) < 1e-3, std::to_string(c(1,0)));
    ok("c(1,1) = 154", std::fabs(c(1,1) - 154.0f) < 1e-3, std::to_string(c(1,1)));
}

static void alpha_and_beta(metal::matrix_multiply& mm) {
    std::cout << "\nalpha and beta:\n";

    matrix<float> a(2, 2);
    a(0,0) = 1; a(0,1) = 0;
    a(1,0) = 0; a(1,1) = 1;      // identity, so the arithmetic is obvious

    matrix<float> b(2, 2);
    b(0,0) = 1; b(0,1) = 2;
    b(1,0) = 3; b(1,1) = 4;

    matrix<float> c(2, 2);
    c(0,0) = 10; c(0,1) = 20;
    c(1,0) = 30; c(1,1) = 40;

    // C = 2*A*B + 3*C = 2*B + 3*C
    mm(a, b, c, 2.0f, 3.0f);

    ok("beta reads the existing C", std::fabs(c(0,0) - (2*1 + 3*10)) < 1e-3,
       std::to_string(c(0,0)) + " wanted 32");
    ok("and alpha scales the product", std::fabs(c(1,1) - (2*4 + 3*40)) < 1e-3,
       std::to_string(c(1,1)) + " wanted 128");
}

static void it_refuses_a_mismatch(metal::matrix_multiply& mm) {
    std::cout << "\nit refuses a mismatch:\n";

    matrix<float> a(2, 3), b(4, 5);

    bool threw = false;

    try { mm(a, b); }
    catch(std::exception&) { threw = true; }

    ok("inner dimensions that do not meet are refused", threw);

    matrix<float> good_b(3, 2), wrong_c(5, 5);

    threw = false;

    try { mm(a, good_b, wrong_c); }
    catch(std::exception&) { threw = true; }

    ok("and so is a result of the wrong shape", threw);
}

int main() {
    std::cout << std::unitbuf;

    std::shared_ptr<metal::device> dev;

    try {
        dev = metal::device::shared();
    }
    catch(std::exception& e) {
        // 77 is SKIP.  A machine with no Metal device is not a failure.
        std::cout << "  skip  no Metal device: " << e.what() << "\n";
        return 77;
    }

    std::cout << "  device: " << dev->name()
              << (dev->unified() ? " (unified memory)" : " (discrete)") << "\n";

    metal::matrix_multiply mm(dev);

    it_agrees_with_the_cpu(mm);
    it_is_not_secretly_transposed(mm);
    alpha_and_beta(mm);
    it_refuses_a_mismatch(mm);

    // What a green run does not establish.
    //
    // That this is faster.  Nothing here is timed, and for small matrices it
    // certainly is not: building the MPS kernel and staging two buffers costs
    // more than multiplying a 4x4.  Where the crossover sits is a measurement
    // nobody has taken yet, and until someone does, "GPU" is a claim about
    // where the work happened rather than about how long it took.
    //
    // Not zero copy.  The operands are copied into shared buffers, because
    // newBufferWithBytesNoCopy needs page-aligned memory and math::array
    // allocates with new T[].  Unified memory means that copy is cheap and
    // local, not that it is absent.
    //
    // Not double, which Metal does not have and cannot be made to have.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
