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
#include <jlib/ai/attention.hh>

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

/** Softmax and RMS norm, which are the first operations that reduce. */
template<typename T>
static void the_reductions(const char* name, std::vector<ai::backend<T>*>& backends) {
    std::cout << "\nreductions, " << name << ":\n";

    std::mt19937 gen(31);

    // Rectangular, and taller than it is wide, because these reduce down a
    // column: a square would let a rows/cols mix-up produce a plausible answer.
    const matrix<T> x = random_matrix<T>(7, 4, gen);

    matrix<T> w(7, 1);
    for(uint r = 0; r < 7; r++) w(r,0) = T(0.5f + 0.1f * r);

    std::vector<matrix<T> > soft, rms;

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr tx = b->make(x);
        typename ai::backend<T>::tensor_ptr tw = b->make(w);
        typename ai::backend<T>::tensor_ptr ts = b->make(7, 4);
        typename ai::backend<T>::tensor_ptr tr = b->make(7, 4);

        b->softmax(tx, ts);
        b->rms_norm(tx, tw, tr, 1e-5f);
        b->wait();

        soft.push_back(ts->read());
        rms.push_back(tr->read());
    }

    // Each column sums to one, which is the defining property and does not
    // need a second implementation to check.
    for(std::size_t i = 0; i < backends.size(); i++) {
        double furthest = 0;

        for(uint c = 0; c < 4; c++) {
            double sum = 0;

            for(uint r = 0; r < 7; r++) sum += double(float(soft[i](r,c)));

            furthest = std::max(furthest, std::fabs(sum - 1.0));
        }

        ok(std::string("  ") + backends[i]->name() + ": every softmax column sums to one",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-6),
           "furthest from 1 was " + std::to_string(furthest));
    }

    // And RMS norm leaves each column with unit root-mean-square, once the
    // per-feature weight is divided back out.
    for(std::size_t i = 0; i < backends.size(); i++) {
        double furthest = 0;

        for(uint c = 0; c < 4; c++) {
            double ss = 0;

            for(uint r = 0; r < 7; r++) {
                const double v = double(float(rms[i](r,c))) / double(float(w(r,0)));
                ss += v * v;
            }

            furthest = std::max(furthest, std::fabs(std::sqrt(ss / 7.0) - 1.0));
        }

        // Just *under* one, not exactly one, and deliberately: eps sits in the
        // denominator, so the result is scaled by 1/sqrt(1 + eps/ms).  For
        // values in [-1,1] that is about 1.4e-5 low, which is what this
        // measures -- the first version of this assertion used 1e-5 and caught
        // eps rather than a bug.  A real error here is of order one.
        ok(std::string("  ") + backends[i]->name() + ": rms_norm leaves unit RMS",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-3),
           "furthest from 1 was " + std::to_string(furthest));
    }

    for(std::size_t i = 1; i < backends.size(); i++) {
        const double tol = (sizeof(T) == 2) ? 2e-2 : 1e-5;

        ok(std::string("  ") + backends[i]->name() + " agrees on softmax",
           worst(soft[0], soft[i]) < tol, std::to_string(worst(soft[0], soft[i])));

        ok(std::string("  ") + backends[i]->name() + " agrees on rms_norm",
           worst(rms[0], rms[i]) < tol, std::to_string(worst(rms[0], rms[i])));
    }
}

/** The reason softmax subtracts the column maximum. */
template<typename T>
static void softmax_survives_a_large_score(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nsoftmax survives a large score, " << name << ":\n";

    // 800, not 90.  exp(90) overflows a float and this test was written with
    // that in mind -- but the host reference accumulates in double, where
    // exp(90) is about 1.2e39 and perfectly finite, so removing the
    // max-subtraction from the host failed to fail.  exp(800) overflows every
    // type involved, so one value covers both backends.
    //
    // The input itself stays representable: 800 fits in fp16, whose ceiling is
    // 65504.  It is exp() that cannot survive it.
    const float big = 800.0f;

    matrix<T> x(3, 2);
    x(0,0) = T(big);   x(0,1) = T(0.0f);
    x(1,0) = T(0.0f);  x(1,1) = T(big);
    x(2,0) = T(-big);  x(2,1) = T(0.0f);

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr tx = b->make(x);
        typename ai::backend<T>::tensor_ptr ts = b->make(3, 2);

        b->softmax(tx, ts);
        b->wait();

        const matrix<T> got = ts->read();

        bool finite = true;
        double sum0 = 0;

        for(uint r = 0; r < 3; r++) {
            for(uint c = 0; c < 2; c++)
                if(!std::isfinite(float(got(r,c)))) finite = false;

            sum0 += double(float(got(r,0)));
        }

        ok(std::string("  ") + b->name() + ": no nan or inf", finite);

        ok(std::string("  ") + b->name() + ": and the column still sums to one",
           std::fabs(sum0 - 1.0) < ((sizeof(T) == 2) ? 5e-3 : 1e-6),
           std::to_string(sum0));

        // The largest score should take essentially all of the mass.
        ok(std::string("  ") + b->name() + ": with the mass on the largest score",
           double(float(got(0,0))) > 0.99, std::to_string(float(got(0,0))));
    }
}

/** Run one head of attention and bring the result back. */
template<typename T>
static matrix<T> run_attention(ai::backend<T>& b, const matrix<T>& q,
                               const matrix<T>& k, const matrix<T>& v, bool causal)
{
    typename ai::backend<T>::tensor_ptr tq = b.make(q);
    typename ai::backend<T>::tensor_ptr tk = b.make(k);
    typename ai::backend<T>::tensor_ptr tv = b.make(v);

    typename ai::backend<T>::tensor_ptr sc = b.make(k.N, q.N);
    typename ai::backend<T>::tensor_ptr pr = b.make(k.N, q.N);
    typename ai::backend<T>::tensor_ptr out = b.make(v.M, q.N);

    ai::attention(b, tq, tk, tv, sc, pr, out, causal);
    b.wait();

    return out->read();
}

/** Attention: the first composite, built from primitives rather than a virtual. */
template<typename T>
static void the_attention(const char* name, std::vector<ai::backend<T>*>& backends) {
    std::cout << "\nattention, " << name << ":\n";

    const unsigned int d = 6;
    const unsigned int n = 5;

    std::mt19937 gen(101);

    const matrix<T> q = random_matrix<T>(d, n, gen);
    const matrix<T> k = random_matrix<T>(d, n, gen);
    const matrix<T> v = random_matrix<T>(d, n, gen);

    std::vector<matrix<T> > got;

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr tq = b->make(q);
        typename ai::backend<T>::tensor_ptr tk = b->make(k);
        typename ai::backend<T>::tensor_ptr tv = b->make(v);

        typename ai::backend<T>::tensor_ptr sc = b->make(n, n);
        typename ai::backend<T>::tensor_ptr pr = b->make(n, n);
        typename ai::backend<T>::tensor_ptr out = b->make(d, n);

        ai::attention(*b, tq, tk, tv, sc, pr, out, true);
        b->wait();

        const matrix<T> p = pr->read();

        double furthest = 0;

        for(unsigned int c = 0; c < n; c++) {
            double sum = 0;

            for(unsigned int r = 0; r < n; r++) sum += double(float(p(r,c)));

            furthest = std::max(furthest, std::fabs(sum - 1.0));
        }

        ok(std::string("  ") + b->name() + ": every masked column still sums to one",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-6), std::to_string(furthest));

        // Strictly below the diagonal, which is where a key later than the
        // query lives in this layout.  Exactly zero, not merely small:
        // exp(-inf - m) is 0 for finite m.
        bool clean = true;

        for(unsigned int c = 0; c < n; c++)
            for(unsigned int r = c + 1; r < n; r++)
                if(float(p(r,c)) != 0.0f) clean = false;

        ok(std::string("  ") + b->name() + ": with exactly zero weight on later keys",
           clean);

        got.push_back(out->read());
    }

    for(std::size_t i = 1; i < backends.size(); i++)
        ok(std::string("  ") + backends[i]->name() + " agrees on the output",
           worst(got[0], got[i]) < ((sizeof(T) == 2) ? 2e-2 : 1e-5),
           std::to_string(worst(got[0], got[i])));
}

/**
 * The property the mask exists for, and the one that catches it upside down.
 *
 * A shape check and a sums-to-one check both pass with the triangle inverted.
 * This does not: change a key and a value at the last position, and every
 * *earlier* output has to be untouched.
 */
template<typename T>
static void attention_looks_only_backwards(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nattention looks only backwards, " << name << ":\n";

    const unsigned int d = 4;
    const unsigned int n = 6;

    std::mt19937 gen(7);

    const matrix<T> q = random_matrix<T>(d, n, gen);
    const matrix<T> k = random_matrix<T>(d, n, gen);
    const matrix<T> v = random_matrix<T>(d, n, gen);

    matrix<T> k2 = k;
    matrix<T> v2 = v;

    for(unsigned int r = 0; r < d; r++) {
        k2(r, n - 1) = T(float(k2(r, n - 1)) + 3.0f);
        v2(r, n - 1) = T(float(v2(r, n - 1)) - 2.5f);
    }

    for(ai::backend<T>* b : backends) {
        const matrix<T> base = run_attention(*b, q, k,  v,  true);
        const matrix<T> pert = run_attention(*b, q, k2, v2, true);

        // Bit-identical, not merely close.  The masked score contributes
        // exp(-inf) = 0 to the sum and never wins the max, so the arithmetic
        // for an earlier column is the same arithmetic on the same values.
        bool same = true;

        for(unsigned int c = 0; c + 1 < n; c++)
            for(unsigned int r = 0; r < d; r++)
                if(float(base(r,c)) != float(pert(r,c))) same = false;

        ok(std::string("  ") + b->name() +
           ": changing the last key and value leaves every earlier output alone",
           same);

        // The control.  Without the mask the same change must reach backwards,
        // or the assertion above is passing for some reason of its own.
        const matrix<T> open_base = run_attention(*b, q, k,  v,  false);
        const matrix<T> open_pert = run_attention(*b, q, k2, v2, false);

        bool moved = false;

        for(unsigned int c = 0; c + 1 < n; c++)
            for(unsigned int r = 0; r < d; r++)
                if(float(open_base(r,c)) != float(open_pert(r,c))) moved = true;

        ok(std::string("  ") + b->name() + ": and without the mask it does not",
           moved);
    }
}

/**
 * Zero queries make every score zero, so softmax is uniform and the output is
 * a plain mean -- of the keys a query can see, which under the mask is a
 * running prefix mean.  An exact expected value that owes nothing to a second
 * implementation.
 */
template<typename T>
static void flat_scores_average_the_values(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nflat scores average the values, " << name << ":\n";

    const unsigned int d = 3;
    const unsigned int n = 4;

    std::mt19937 gen(19);

    const matrix<T> q(d, n);            // zeros
    const matrix<T> k = random_matrix<T>(d, n, gen);
    const matrix<T> v = random_matrix<T>(d, n, gen);

    for(ai::backend<T>* b : backends) {
        const matrix<T> got = run_attention(*b, q, k, v, true);

        double furthest = 0;

        for(unsigned int c = 0; c < n; c++) {
            for(unsigned int r = 0; r < d; r++) {
                double mean = 0;

                for(unsigned int j = 0; j <= c; j++) mean += double(float(v(r,j)));

                mean /= double(c + 1);

                furthest = std::max(furthest, std::fabs(mean - double(float(got(r,c)))));
            }
        }

        ok(std::string("  ") + b->name() +
           ": each output is the mean of the values up to it",
           furthest < ((sizeof(T) == 2) ? 2e-2 : 1e-5), std::to_string(furthest));
    }
}

/**
 * The 1/sqrt(d) scale, which nothing else here pins down.
 *
 * Every other attention assertion survives its removal: the prefix-mean test
 * uses zero queries, so the scores are zero at any scale, and the host/GPU
 * comparison drops it on both sides at once.  This is an exact expected value
 * that the scale changes.
 *
 * One query and two keys, chosen so the arithmetic is short.  k_0 is the zero
 * vector and k_1 is all ones against a query of all ones, so the dot products
 * are 0 and d.  Scaled by 1/sqrt(d) with d = 4 the scores are 0 and 2, and the
 * weight on the second key is e^2 / (1 + e^2).  Unscaled they would be 0 and 4,
 * giving 0.982 instead of 0.881.
 *
 * Not causal: with one query at position 0 the mask would hide key 1, which is
 * the whole experiment.
 */
template<typename T>
static void the_scale_is_applied(const char* name,
                                 std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe scale is applied, " << name << ":\n";

    const unsigned int d = 4;

    matrix<T> q(d, 1);
    matrix<T> k(d, 2);
    matrix<T> v(1, 2);

    for(unsigned int r = 0; r < d; r++) {
        q(r,0) = T(1.0f);
        k(r,0) = T(0.0f);
        k(r,1) = T(1.0f);
    }

    v(0,0) = T(0.0f);
    v(0,1) = T(1.0f);

    // exp(2) / (1 + exp(2)), which is the weight the scaled score puts on k_1,
    // and v picks it out because v_0 is 0 and v_1 is 1.
    const double want = std::exp(2.0) / (1.0 + std::exp(2.0));

    for(ai::backend<T>* b : backends) {
        const matrix<T> got = run_attention(*b, q, k, v, false);

        const double diff = std::fabs(double(float(got(0,0))) - want);

        ok(std::string("  ") + b->name() + ": the score is divided by sqrt(d)",
           diff < ((sizeof(T) == 2) ? 5e-3 : 1e-5),
           std::to_string(double(float(got(0,0)))) + " vs " + std::to_string(want));
    }
}

/** The inner product of two columns, in double. */
template<typename T>
static double dot(const matrix<T>& a, uint ca, const matrix<T>& b, uint cb) {
    double d = 0;

    for(uint r = 0; r < a.M; r++)
        d += double(float(a(r,ca))) * double(float(b(r,cb)));

    return d;
}

/**
 * RoPE is a rotation, so it preserves length and is the identity at zero.
 *
 * Both are exact statements about what the operation *is*, and neither needs a
 * second implementation to check.
 */
template<typename T>
static void rope_is_a_rotation(const char* name, std::vector<ai::backend<T>*>& backends) {
    std::cout << "\nrope is a rotation, " << name << ":\n";

    const uint d = 8;
    const uint n = 4;

    std::mt19937 gen(53);

    const matrix<T> x = random_matrix<T>(d, n, gen);

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr t = b->make(x);

        b->rope(t, 5);
        b->wait();

        const matrix<T> got = t->read();

        double furthest = 0;

        for(uint c = 0; c < n; c++)
            furthest = std::max(furthest,
                                std::fabs(std::sqrt(dot(got,c,got,c)) -
                                          std::sqrt(dot(x,c,x,c))));

        ok(std::string("  ") + b->name() + ": every column keeps its length",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-5), std::to_string(furthest));

        // Position 0 turns through zero radians, so it is exactly the input --
        // and base_pos 0 means column c is at position c, so only the first
        // column is untouched.
        typename ai::backend<T>::tensor_ptr z = b->make(x);

        b->rope(z, 0);
        b->wait();

        const matrix<T> zero = z->read();

        bool identical = true;

        for(uint r = 0; r < d; r++)
            if(float(zero(r,0)) != float(x(r,0))) identical = false;

        ok(std::string("  ") + b->name() + ": and position zero is untouched",
           identical);

        bool moved = false;

        for(uint r = 0; r < d; r++)
            if(float(zero(r,3)) != float(x(r,3))) moved = true;

        ok(std::string("  ") + b->name() + ": while a later position is not",
           moved);
    }
}

/**
 * The property RoPE exists for: the score depends only on the distance.
 *
 * <RoPE(q, m), RoPE(k, n)> is a function of n - m alone, so shifting both
 * positions by the same amount must leave every inner product where it was.
 * This is the one assertion that would fail for almost any other way of mixing
 * position into a vector, and it is why RoPE is used instead of adding a
 * positional vector to the input.
 */
template<typename T>
static void rope_makes_the_score_relative(const char* name,
                                          std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nrope makes the score relative, " << name << ":\n";

    const uint d = 8;
    const uint n = 3;

    std::mt19937 gen(59);

    const matrix<T> q = random_matrix<T>(d, n, gen);
    const matrix<T> k = random_matrix<T>(d, n, gen);

    for(ai::backend<T>* b : backends) {
        std::vector<double> base;
        double furthest = 0;

        // The same pair of columns, rotated from four different starting
        // positions.  Every relative offset is the same in each, so every dot
        // product between them should be too.
        for(unsigned int shift : { 0u, 1u, 7u, 40u }) {
            typename ai::backend<T>::tensor_ptr tq = b->make(q);
            typename ai::backend<T>::tensor_ptr tk = b->make(k);

            b->rope(tq, shift);
            b->rope(tk, shift);
            b->wait();

            const matrix<T> rq = tq->read();
            const matrix<T> rk = tk->read();

            std::vector<double> now;

            for(uint i = 0; i < n; i++)
                for(uint j = 0; j < n; j++)
                    now.push_back(dot(rq, i, rk, j));

            if(shift == 0) {
                base = now;
                continue;
            }

            for(std::size_t at = 0; at < now.size(); at++)
                furthest = std::max(furthest, std::fabs(base[at] - now[at]));
        }

        ok(std::string("  ") + b->name() +
           ": shifting both positions leaves every score alone",
           furthest < ((sizeof(T) == 2) ? 5e-2 : 1e-4), std::to_string(furthest));
    }
}

template<typename T>
static void the_two_rope_layouts_differ(const char* name,
                                        std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe two rope layouts differ, " << name << ":\n";

    std::mt19937 gen(61);

    const matrix<T> x = random_matrix<T>(8, 3, gen);

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr a = b->make(x);
        typename ai::backend<T>::tensor_ptr c = b->make(x);

        b->rope(a, 2, 10000.0f, ai::rope_layout::interleaved);
        b->rope(c, 2, 10000.0f, ai::rope_layout::split);
        b->wait();

        const matrix<T> ia = a->read();
        const matrix<T> sp = c->read();

        ok(std::string("  ") + b->name() + ": interleaved is not split",
           worst(ia, sp) > 1e-3, std::to_string(worst(ia, sp)));

        // And both are rotations, which is what makes them indistinguishable
        // by any property test.
        double la = 0, ls = 0;

        for(uint r = 0; r < 8; r++) {
            la += double(float(ia(r,1))) * double(float(ia(r,1)));
            ls += double(float(sp(r,1))) * double(float(sp(r,1)));
        }

        ok(std::string("  ") + b->name() + ": and both preserve the length",
           std::fabs(std::sqrt(la) - std::sqrt(ls)) < ((sizeof(T) == 2) ? 5e-3 : 1e-5),
           std::to_string(std::fabs(std::sqrt(la) - std::sqrt(ls))));

        typename ai::backend<T>::tensor_ptr odd = b->make(7, 2);

        bool threw = false;
        try { b->rope(odd); b->wait(); }
        catch(std::exception&) { threw = true; }

        ok(std::string("  ") + b->name() + ": an odd number of rows is refused",
           threw);
    }
}

/** The embedding lookup: pick columns out of a table by index. */
template<typename T>
static void the_gather(const char* name, std::vector<ai::backend<T>*>& backends) {
    std::cout << "\ngather, " << name << ":\n";

    const uint d = 5;
    const uint vocab = 7;

    matrix<T> table(d, vocab);

    // Distinct per column, so a gather that took the wrong one is visible.
    for(uint c = 0; c < vocab; c++)
        for(uint r = 0; r < d; r++)
            table(r,c) = T(float(c) * 10.0f + float(r));

    // Out of order and with a repeat, because a token can occur twice and
    // nothing says the ids ascend.
    const std::vector<int> ids{ 3, 0, 6, 3 };

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr t = b->make(table);
        typename ai::backend<T>::tensor_ptr out = b->make(d, uint(ids.size()));

        b->gather(t, ids, out);
        b->wait();

        const matrix<T> got = out->read();

        double furthest = 0;

        for(std::size_t i = 0; i < ids.size(); i++)
            for(uint r = 0; r < d; r++)
                furthest = std::max(furthest,
                                    std::fabs(double(float(got(r, uint(i)))) -
                                              double(float(table(r, uint(ids[i]))))));

        ok(std::string("  ") + b->name() + ": every column is the one asked for",
           furthest == 0.0, std::to_string(furthest));

        // An id off the end reads whatever is next in the buffer if nothing
        // checks it, which is a wrong token rather than a crash -- so it is
        // checked, and on the GPU it has to be checked before the kernel runs.
        bool threw = false;

        try { b->gather(t, std::vector<int>{ 0, 7 }, out); b->wait(); }
        catch(std::exception&) { threw = true; }

        ok(std::string("  ") + b->name() + ": an id past the end is refused", threw);

        threw = false;

        try { b->gather(t, std::vector<int>{ 0, -1 }, out); b->wait(); }
        catch(std::exception&) { threw = true; }

        ok(std::string("  ") + b->name() + ": and so is a negative one", threw);
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
        catch(std::exception& e) {
            // A failure, not a note.  HAVE_METAL means Metal was found when
            // this was configured, so the backend not coming up is a bug here
            // -- most likely a kernel that no longer compiles.  Reported as a
            // note, this test went on to pass with only the host backend and
            // exit 0, which is how a broken kernel reaches a green run.
            ok("  the Metal backend comes up", false, e.what());
        }
        one_type<float>("float", b);
        the_reductions<float>("float", b);
        softmax_survives_a_large_score<float>("float", b);
        the_attention<float>("float", b);
        attention_looks_only_backwards<float>("float", b);
        flat_scores_average_the_values<float>("float", b);
        the_scale_is_applied<float>("float", b);
        rope_is_a_rotation<float>("float", b);
        rope_makes_the_score_relative<float>("float", b);
        the_two_rope_layouts_differ<float>("float", b);
        the_gather<float>("float", b);
#else
        one_type<float>("float", b);
        the_reductions<float>("float", b);
        softmax_survives_a_large_score<float>("float", b);
        the_attention<float>("float", b);
        attention_looks_only_backwards<float>("float", b);
        flat_scores_average_the_values<float>("float", b);
        the_scale_is_applied<float>("float", b);
        rope_is_a_rotation<float>("float", b);
        rope_makes_the_score_relative<float>("float", b);
        the_two_rope_layouts_differ<float>("float", b);
        the_gather<float>("float", b);
#endif
    }

    {
        ai::host_backend<_Float16> h;
        std::vector<ai::backend<_Float16>*> b{ &h };

#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<_Float16> > g;
        try { g.reset(new jlib::metal::backend<_Float16>); b.push_back(g.get()); }
        catch(std::exception& e) { ok("  the Metal backend comes up", false, e.what()); }
#endif
        one_type<_Float16>("_Float16", b);
        the_reductions<_Float16>("_Float16", b);
        softmax_survives_a_large_score<_Float16>("_Float16", b);
        the_attention<_Float16>("_Float16", b);
        attention_looks_only_backwards<_Float16>("_Float16", b);
        flat_scores_average_the_values<_Float16>("_Float16", b);
        the_scale_is_applied<_Float16>("_Float16", b);
        rope_is_a_rotation<_Float16>("_Float16", b);
        rope_makes_the_score_relative<_Float16>("_Float16", b);
        the_two_rope_layouts_differ<_Float16>("_Float16", b);
        the_gather<_Float16>("_Float16", b);
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
    //
    // For attention specifically: that a fused kernel would agree.  This is a
    // composite of primitives, and the checks below are against the properties
    // of the operation rather than against a reference implementation, so a
    // FlashAttention-style kernel would have to be re-verified rather than
    // assumed.  Nor multi-head anything -- heads here are separate tensors and
    // separate calls, and nothing tests that a caller splits them correctly.
    //
    // The agreement assertions above would not, on their own, have caught a
    // missing max-subtraction: at the magnitudes random_matrix produces, both
    // backends give the same answer with or without it.  That is what
    // softmax_survives_a_large_score is for, and it was checked by deleting the
    // subtraction from each backend in turn and confirming each deletion fails
    // the test.  No other assertion here has been mutation-checked, so the rest
    // carry only the usual claim: they pass.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
