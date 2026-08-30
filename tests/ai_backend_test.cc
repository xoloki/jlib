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

// One sequence of operations, every backend, every element type.
//
// This is the test the abstraction exists for.  jlib/cuda/neural.hh is a fork
// of the network that silently missed three fixes because nothing compared it
// against the original (#138); the point of ai::backend is that there is one
// algorithm and the device is a parameter, and the point of this file is to
// keep the parameters honest about each other.
//
// The host backend is the oracle: it is naive, immediate, and made of
// math::matrix, so there is nothing clever in it to be wrong about twice.

#include <jlib/ai/backend.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ai = jlib::ai;

using jlib::math::matrix;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

template<typename T>
static matrix<T> random_matrix(uint m, uint n, std::mt19937& gen) {
    // A modest range on purpose.  fp16 has about three decimal digits and
    // tops out near 65504; a comparison that overflowed one type and not the
    // other would be measuring the range rather than the arithmetic.
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);

    matrix<T> a(m, n);

    for(uint r = 0; r < m; r++)
        for(uint c = 0; c < n; c++)
            a(r, c) = T(d(gen));

    return a;
}

template<typename T>
static double worst(const matrix<T>& a, const matrix<T>& b) {
    if(a.M != b.M || a.N != b.N) return 1e9;

    double w = 0;

    for(uint r = 0; r < a.M; r++)
        for(uint c = 0; c < a.N; c++)
            w = std::max(w, std::fabs(double(float(a(r,c))) - double(float(b(r,c)))));

    return w;
}

/**
 * A forward layer and the start of a backward one, which between them use
 * every operation the interface has.
 */
template<typename T>
static matrix<T> exercise(ai::backend<T>& b, const matrix<T>& w,
                          const matrix<T>& x, const matrix<T>& target)
{
    typename ai::backend<T>::tensor_ptr tw = b.make(w);
    typename ai::backend<T>::tensor_ptr tx = b.make(x);
    typename ai::backend<T>::tensor_ptr tt = b.make(target);

    typename ai::backend<T>::tensor_ptr z = b.make(w.M, x.N);
    typename ai::backend<T>::tensor_ptr a = b.make(w.M, x.N);
    typename ai::backend<T>::tensor_ptr e = b.make(w.M, x.N);
    typename ai::backend<T>::tensor_ptr s = b.make(w.M, x.N);
    typename ai::backend<T>::tensor_ptr g = b.make(w.M, w.N);

    b.multiply(tw, tx, z);                          // z = w x
    b.activate(ai::activation::sigmoid, z, a);      // a = f(z)
    b.subtract(tt, a, e);                           // e = target - a
    b.slope(ai::activation::sigmoid, a, s);         // s = f'(a)
    b.hadamard(e, s, e);                            // e = e * s
    b.multiply_nt(e, tx, g);                        // g = e x^T
    b.add_scaled(T(0.5), g, tw);                    // w += 0.5 g

    b.wait();

    return tw->read();
}

template<typename T>
static void one_type(const char* name, std::vector<ai::backend<T>*>& backends) {
    std::cout << "\n" << name << ":\n";

    std::mt19937 gen(20260830);

    // Rectangular and all different, because a transpose error cannot make a
    // correctly shaped answer by accident that way.
    const matrix<T> w = random_matrix<T>(5, 3, gen);
    const matrix<T> x = random_matrix<T>(3, 7, gen);
    const matrix<T> t = random_matrix<T>(5, 7, gen);

    std::vector<matrix<T> > results;

    for(ai::backend<T>* b : backends) {
        results.push_back(exercise(*b, w, x, t));

        ok(std::string("  ") + b->name() + " ran the sequence",
           results.back().M == 5 && results.back().N == 3,
           std::to_string(results.back().M) + "x" + std::to_string(results.back().N));
    }

    for(std::size_t i = 1; i < results.size(); i++) {
        const double d = worst(results[0], results[i]);

        // Loose for fp16, which carries about three decimal digits, and the
        // sequence above has a multiply, a sigmoid and another multiply in it.
        // The bound is there to catch a wrong operation, not to measure
        // rounding: a transposed multiply or a swapped operand moves this by
        // whole numbers.
        const double tol = (sizeof(T) == 2) ? 2e-2 : 1e-5;

        ok(std::string("  ") + backends[i]->name() + " agrees with " + backends[0]->name(),
           d < tol, "worst " + std::to_string(d) + ", tolerance " + std::to_string(tol));
    }
}

static void a_tensor_from_the_wrong_backend_is_refused() {
    std::cout << "\na tensor from the wrong backend is refused:\n";

#ifdef HAVE_METAL
    ai::host_backend<float> host;

    std::shared_ptr<jlib::metal::backend<float> > gpu;

    try { gpu.reset(new jlib::metal::backend<float>); }
    catch(std::exception&) { std::cout << "  skip  no device\n"; return; }

    ai::backend<float>::tensor_ptr h = host.make(2, 2);
    ai::backend<float>::tensor_ptr d = gpu->make(2, 2);

    bool threw = false;

    // The two are the same static type, so nothing but a runtime check stands
    // between this and reading a GPU buffer as host memory.
    try { ai::backend<float>::tensor_ptr o = gpu->make(2,2); gpu->multiply(h, d, o); }
    catch(std::exception&) { threw = true; }

    ok("a host tensor handed to the GPU backend", threw);

    threw = false;

    try { ai::backend<float>::tensor_ptr o = host.make(2,2); host.multiply(d, h, o); }
    catch(std::exception&) { threw = true; }

    ok("and a device tensor handed to the host backend", threw);
#else
    std::cout << "  skip  no Metal in this build, so there is only one backend\n";
#endif
}

int main() {
    std::cout << std::unitbuf;

    {
        ai::host_backend<float> h;
        std::vector<ai::backend<float>*> b{ &h };

#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<float> > g;
        try { g.reset(new jlib::metal::backend<float>); b.push_back(g.get()); }
        catch(std::exception& e) { std::cout << "  (no Metal device: " << e.what() << ")\n"; }
        one_type<float>("float", b);
#else
        one_type<float>("float", b);
#endif
    }

    {
        ai::host_backend<_Float16> h;
        std::vector<ai::backend<_Float16>*> b{ &h };

#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<_Float16> > g;
        try { g.reset(new jlib::metal::backend<_Float16>); b.push_back(g.get()); }
        catch(std::exception&) {}
#endif
        one_type<_Float16>("_Float16", b);
    }

    a_tensor_from_the_wrong_backend_is_refused();

    // What a green run does not establish.
    //
    // That either backend is right, only that they agree.  The host one is
    // written to be obvious rather than fast for that reason, but two
    // implementations of the same misunderstanding would still pass this.
    // metal_tensor_test compares the GPU against hand-written CPU loops, which
    // is the other half of the argument.
    //
    // Not fp16's suitability for training.  It is exercised here because the
    // interface carries it, and gradients underflow in fp16 -- training in it
    // needs loss scaling and an fp32 master copy of the weights, neither of
    // which exists.  fp16 is for inference.
    //
    // Not double, which Metal has no type for and cannot be given one.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
