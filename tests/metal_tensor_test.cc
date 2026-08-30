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

// Tensors that stay on the GPU, and the operations over them.
//
// The oracle throughout is the CPU: math::operator* and ai::activate, which
// are naive and therefore hard to be wrong in the same way twice.
//
// The transposed multiplies get the most attention.  math::matrix is
// column-major and MPS is row-major, so every operand is read as its own
// transpose and the operands are swapped -- and on a square matrix of random
// numbers a wrongly-transposed answer looks exactly as plausible as a right
// one.  Everything below is deliberately rectangular.

#include <jlib/metal/tensor.hh>
#include <jlib/metal/device.hh>

#include <jlib/ai/backend.hh>
#include <jlib/math/matrix.hh>

#include <cmath>
#include <iostream>
#include <random>
#include <string>

namespace metal = jlib::metal;
namespace ai = jlib::ai;

using jlib::math::matrix;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::mt19937 gen(20260830);

static matrix<float> random_matrix(uint m, uint n) {
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);

    matrix<float> a(m, n);

    for(uint r = 0; r < m; r++)
        for(uint c = 0; c < n; c++)
            a(r, c) = d(gen);

    return a;
}

static double worst(const matrix<float>& a, const matrix<float>& b) {
    if(a.M != b.M || a.N != b.N) return 1e9;

    double w = 0;

    for(uint r = 0; r < a.M; r++)
        for(uint c = 0; c < a.N; c++)
            w = std::max(w, std::fabs(double(a(r,c)) - double(b(r,c))));

    return w;
}

static void the_enums_agree() {
    std::cout << "\nthe enums agree:\n";

    // metal has its own activation enum so that a GPU backend does not depend
    // on the neural library.  The duplication is only safe while the values
    // match, and the kernels switch on the raw number.
    ok("sigmoid", (int)metal::activation::sigmoid == (int)ai::activation::sigmoid);
    ok("tanh", (int)metal::activation::tanh == (int)ai::activation::tanh);
    ok("relu", (int)metal::activation::relu == (int)ai::activation::relu);
    ok("leaky_relu", (int)metal::activation::leaky_relu == (int)ai::activation::leaky_relu);
    // Compared as float, which is the precision that exists on the GPU.
    // ai::LEAK is a double 0.01 and metal::LEAK is a float 0.01f, and
    // double(0.01f) is not 0.01 -- so the widening comparison is false while
    // the values agree everywhere they are used.  The activation test above
    // is the one that would notice if they really diverged.
    ok("and so does the leak", metal::LEAK == float(ai::LEAK),
       std::to_string(metal::LEAK) + " against " + std::to_string(float(ai::LEAK)));
}

static void a_tensor_round_trips(std::shared_ptr<metal::device> dev) {
    std::cout << "\na tensor round trips:\n";

    // Rectangular, so a row/column mix-up in the upload cannot hide.
    const matrix<float> m = random_matrix(3, 7);

    metal::tensor<float> t(dev, m);

    ok("the shape survives", t.rows() == 3 && t.cols() == 7,
       std::to_string(t.rows()) + "x" + std::to_string(t.cols()));

    ok("and every value", worst(m, t.read()) == 0.0,
       std::to_string(worst(m, t.read())));

    const matrix<float> other = random_matrix(3, 7);

    t.write(other);

    ok("writing replaces it", worst(other, t.read()) == 0.0);

    bool threw = false;
    try { t.write(random_matrix(7, 3)); }
    catch(std::exception&) { threw = true; }

    ok("and a wrong shape is refused", threw);
}

static void the_elementwise_ops(std::shared_ptr<metal::device> dev) {
    std::cout << "\nthe elementwise ops:\n";

    const matrix<float> a = random_matrix(5, 9);
    const matrix<float> b = random_matrix(5, 9);

    metal::tensor<float> ta(dev, a), tb(dev, b), tc(dev, 5, 9);

    metal::stream<float> s(dev);

    s.hadamard(ta, tb, tc);
    s.wait();

    matrix<float> want(5, 9);
    for(uint r = 0; r < 5; r++)
        for(uint c = 0; c < 9; c++)
            want(r,c) = a(r,c) * b(r,c);

    ok("hadamard", worst(want, tc.read()) < 1e-6,
       std::to_string(worst(want, tc.read())));

    s.subtract(ta, tb, tc);
    s.wait();

    for(uint r = 0; r < 5; r++)
        for(uint c = 0; c < 9; c++)
            want(r,c) = a(r,c) - b(r,c);

    ok("subtract", worst(want, tc.read()) < 1e-6);

    // y += alpha * x, which is the weight update.
    metal::tensor<float> ty(dev, b);

    s.add_scaled(0.25f, ta, ty);
    s.wait();

    for(uint r = 0; r < 5; r++)
        for(uint c = 0; c < 9; c++)
            want(r,c) = b(r,c) + 0.25f * a(r,c);

    ok("add_scaled", worst(want, ty.read()) < 1e-6,
       std::to_string(worst(want, ty.read())));
}

static void the_activations_match_the_cpu(std::shared_ptr<metal::device> dev) {
    std::cout << "\nthe activations match the CPU:\n";

    // Spanning zero, since three of the four change behaviour there and the
    // relu family is defined by what it does on each side.
    matrix<float> x(1, 8);
    x(0,0) = -3.0f; x(0,1) = -1.0f; x(0,2) = -0.25f; x(0,3) = 0.0f;
    x(0,4) = 0.25f; x(0,5) = 1.0f;  x(0,6) = 3.0f;   x(0,7) = 8.0f;

    const struct { metal::activation m; ai::activation a; const char* name; } kinds[] = {
        { metal::activation::sigmoid,    ai::activation::sigmoid,    "sigmoid" },
        { metal::activation::tanh,       ai::activation::tanh,       "tanh" },
        { metal::activation::relu,       ai::activation::relu,       "relu" },
        { metal::activation::leaky_relu, ai::activation::leaky_relu, "leaky_relu" },
    };

    metal::stream<float> s(dev);

    for(const auto& k : kinds) {
        metal::tensor<float> in(dev, x), out(dev, 1, 8), sl(dev, 1, 8);

        s.activate(k.m, in, out);
        s.wait();

        const matrix<float> want = ai::activate_matrix(k.a, x);
        const matrix<float> got = out.read();

        // 1e-6 rather than exact: the GPU's exp and tanh are its own, and are
        // allowed to differ from libm in the last bits.
        ok(std::string(k.name), worst(want, got) < 1e-6,
           std::to_string(worst(want, got)));

        s.slope(k.m, out, sl);
        s.wait();

        ok(std::string("  and its slope"),
           worst(ai::slope_matrix(k.a, want), sl.read()) < 1e-6,
           std::to_string(worst(ai::slope_matrix(k.a, want), sl.read())));
    }
}

static void the_multiplies(std::shared_ptr<metal::device> dev) {
    std::cout << "\nthe multiplies:\n";

    // 4x6 by 6x3 -- every dimension different, so a transpose error cannot
    // produce a correctly shaped answer by accident.
    const matrix<float> a = random_matrix(4, 6);
    const matrix<float> b = random_matrix(6, 3);

    metal::tensor<float> ta(dev, a), tb(dev, b), tc(dev, 4, 3);

    metal::stream<float> s(dev);

    s.multiply(ta, tb, tc);
    s.wait();

    ok("a * b", worst(a * b, tc.read()) < 1e-5,
       std::to_string(worst(a * b, tc.read())));

    // a^T * b, without materialising the transpose.
    const matrix<float> at = random_matrix(6, 4);

    metal::tensor<float> tat(dev, at), tc2(dev, 4, 3);

    s.multiply_tn(tat, tb, tc2);
    s.wait();

    ok("a^T * b", worst(at.transpose() * b, tc2.read()) < 1e-5,
       std::to_string(worst(at.transpose() * b, tc2.read())));

    // a * b^T.
    const matrix<float> bt = random_matrix(3, 6);

    metal::tensor<float> tbt(dev, bt), tc3(dev, 4, 3);

    s.multiply_nt(ta, tbt, tc3);
    s.wait();

    ok("a * b^T", worst(a * bt.transpose(), tc3.read()) < 1e-5,
       std::to_string(worst(a * bt.transpose(), tc3.read())));

    // beta, which is what makes an accumulate possible.
    metal::tensor<float> acc(dev, a * b);

    s.multiply(ta, tb, acc, 1.0f, 1.0f);
    s.wait();

    ok("and beta accumulates", worst((a * b) + (a * b), acc.read()) < 1e-5,
       std::to_string(worst((a * b) + (a * b), acc.read())));

    bool threw = false;
    try { metal::tensor<float> bad(dev, 9, 9); s.multiply(ta, tb, bad); }
    catch(std::exception&) { threw = true; }

    ok("a result of the wrong shape is refused", threw);
}

static void many_ops_one_wait(std::shared_ptr<metal::device> dev) {
    std::cout << "\nmany ops, one wait:\n";

    // The reason the module exists.  A forward layer is a multiply and an
    // activation; doing both without a synchronisation between them is what
    // keeps the data on the device.
    const matrix<float> w = random_matrix(6, 4);
    const matrix<float> x = random_matrix(4, 5);

    metal::tensor<float> tw(dev, w), tx(dev, x), tz(dev, 6, 5), ta(dev, 6, 5);

    metal::stream<float> s(dev);

    s.multiply(tw, tx, tz);
    s.activate(metal::activation::relu, tz, ta);

    ok("nothing has run yet", s.pending() == 2, std::to_string(s.pending()));

    s.wait();

    ok("and afterwards nothing is pending", s.pending() == 0,
       std::to_string(s.pending()));

    const matrix<float> want = ai::activate_matrix(ai::activation::relu, w * x);

    ok("the layer is right", worst(want, ta.read()) < 1e-5,
       std::to_string(worst(want, ta.read())));

    // Interleaving matters: MPS encodes into the command buffer and the
    // elementwise kernels into a compute encoder, so the stream has to close
    // and reopen one around the other.  Doing it several times over is the
    // case that would break if it did not.
    metal::tensor<float> t2(dev, 6, 5), t3(dev, 6, 5);

    s.multiply(tw, tx, tz);
    s.activate(metal::activation::sigmoid, tz, t2);
    s.multiply(tw, tx, t3);
    s.hadamard(t2, t3, t3);
    s.wait();

    matrix<float> expect(6, 5);
    const matrix<float> wx = w * x;
    const matrix<float> sig = ai::activate_matrix(ai::activation::sigmoid, wx);

    for(uint r = 0; r < 6; r++)
        for(uint c = 0; c < 5; c++)
            expect(r,c) = sig(r,c) * wx(r,c);

    ok("and so is a multiply/elementwise/multiply/elementwise chain",
       worst(expect, t3.read()) < 1e-5,
       std::to_string(worst(expect, t3.read())));
}

int main() {
    std::cout << std::unitbuf;

    std::shared_ptr<metal::device> dev;

    try {
        dev = metal::device::shared();
    }
    catch(std::exception& e) {
        std::cout << "  skip  no Metal device: " << e.what() << "\n";
        return 77;
    }

    std::cout << "  device: " << dev->name() << "\n";

    the_enums_agree();
    a_tensor_round_trips(dev);
    the_elementwise_ops(dev);
    the_activations_match_the_cpu(dev);
    the_multiplies(dev);
    many_ops_one_wait(dev);

    // What a green run does not establish.
    //
    // That this is faster.  Nothing here is timed; the whole argument for
    // keeping tensors on the device is about not synchronising between
    // operations, and that shows up in a training loop rather than in an
    // assertion.
    //
    // Not the sharp edge: reading a tensor before wait() gives whatever it
    // held before, and nothing detects it.  A test for it would be a test
    // that the GPU is slower than the CPU, which is not a property worth
    // asserting.
    //
    // Not concurrency.  One stream, one thread.  Two streams over the same
    // tensors would need ordering nothing here provides.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
