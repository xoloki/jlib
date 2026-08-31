/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#include <cmath>
#include <iostream>
#include <string>

#include <jlib/math/tensor.hh>

using namespace jlib::math;

typedef double T;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    std::cout << (good ? "  ok   " : "  FAIL ") << what;

    if(!detail.empty()) std::cout << ": " << detail;

    std::cout << "\n";

    if(!good) failures++;
}

static double g_next = 0;

static void fill_rec(tensor<T> t) {
    if(t.rank() == 0) {
        t = g_next;
        g_next += 1.0;

        return;
    }

    for(unsigned int i = 0; i < t.size(0); i++)
        fill_rec(t[i]);
}

/**
 * Fill with base, base+1, base+2, ... in row-major order.
 *
 * By recursion down operator[], not by a flat loop: operator[] slices the
 * first axis rather than indexing an element, so `t[i] = x` on anything above
 * rank 1 is a mismatch.  Recursing also means the fill order is whatever
 * slicing says it is, rather than an assumption about the layout that the
 * contraction below is supposed to be testing.
 */
static void fill(tensor<T>& t, double base = 1.0) {
    g_next = base;

    fill_rec(t);
}

static double at2(tensor<T>& t, unsigned int i, unsigned int j) {
    return t[i][j];
}

/**
 * Writing through a const tensor must not compile.
 *
 * Asserted in both directions: a concept that is false because of a typo rather
 * than because of constness would pass the half that matters and prove nothing.
 */
template<typename U>
concept assignable_through = requires(U& t) { t[0][0] = 1.0; };

static_assert(assignable_through<tensor<T> >,
              "a non-const tensor should be writable through operator[]");
static_assert(!assignable_through<const tensor<T> >,
              "a const tensor should not be");

static void the_accessors_agree(tensor<T>& t) {
    std::cout << "\nthe accessors agree:\n";

    const tensor<T>& ct = t;

    double furthest = 0;

    for(unsigned int i = 0; i < t.size(0); i++)
        for(unsigned int j = 0; j < t.size(1); j++) {
            // t(i,j) walks strides; t[i][j] builds two views and slices.  They
            // share no code, so agreeing is worth something.
            furthest = std::max(furthest, std::fabs(t(i, j) - at2(t, i, j)));

            // And the const overloads reach the same elements, which is the
            // whole point of adding them: this line did not compile before.
            furthest = std::max(furthest, std::fabs(ct(i, j) - double(ct[i][j])));
        }

    ok("operator(i,j), operator[i][j] and their const forms all agree",
       furthest == 0, std::to_string(furthest));

    tensor<T> scalar(0);
    scalar = 7.0;

    ok("and operator() with no indices reaches a rank 0 element",
       scalar() == 7.0, std::to_string(scalar()));

    bool threw = false;
    try { t(0u); }
    catch(tensor<T>::mismatch&) { threw = true; }

    ok("while the wrong number of indices is a mismatch", threw);
}

static void a_scalar_is_rank_zero() {
    std::cout << "\na scalar is rank zero:\n";

    tensor<T> scalar(0);
    tensor<T> vector(1, 5u);

    const T VAL = 5.0;

    scalar = VAL;

    // Both of these are the 1999 test, unchanged in substance.
    bool threw = false;

    try { vector = 5.0; }
    catch(tensor<T>::mismatch&) { threw = true; }

    ok("assigning a scalar to a vector is refused", threw);

    const T val = scalar;

    ok("and a rank 0 tensor round-trips a scalar", val == VAL,
       std::to_string(val));
}

static void contraction_is_matrix_multiply_at_rank_two() {
    std::cout << "\ncontraction is matrix multiply at rank two:\n";

    tensor<T> a(2, 3u, 4u);
    tensor<T> b(2, 4u, 5u);

    fill(a);
    fill(b, 0.5);

    tensor<T> c = a ^ b;

    ok("the result is rank 2", c.rank() == 2, std::to_string(c.rank()));
    ok("with as many rows as the first and columns as the second",
       c.rank() == 2 && c.size(0) == 3 && c.size(1) == 5);

    // An independent triple loop, written here rather than reused from the
    // header, because a header that checks itself proves nothing.
    double furthest = 0;

    for(unsigned int i = 0; i < 3; i++) {
        for(unsigned int j = 0; j < 5; j++) {
            double sum = 0;

            for(unsigned int k = 0; k < 4; k++)
                sum += at2(a, i, k) * at2(b, k, j);

            furthest = std::max(furthest, std::fabs(sum - at2(c, i, j)));
        }
    }

    ok("and every element matches a hand-written triple loop", furthest == 0,
       std::to_string(furthest));

    // The identity is the cheapest check that the flattening is the right way
    // round: a transposed layout would still have the right shape here and the
    // wrong numbers.
    tensor<T> id(2, 4u, 4u);

    for(unsigned int i = 0; i < 4; i++) id[i][i] = 1.0;

    tensor<T> same = a ^ id;

    ok("contracting with the identity returns the original", same == a);
}

static void contraction_generalises_past_rank_two() {
    std::cout << "\ncontraction generalises past rank two:\n";

    tensor<T> a(3, 2u, 3u, 4u);
    tensor<T> b(2, 4u, 5u);

    fill(a);
    fill(b, 0.25);

    tensor<T> c = a ^ b;

    // rank 3 + rank 2 - 2 = rank 3, shaped (2,3,5): a's axes without its last,
    // then b's without its first.  This is the case that threw until now.
    ok("rank 3 ^ rank 2 is rank 3", c.rank() == 3, std::to_string(c.rank()));
    ok("shaped (2,3,5)",
       c.rank() == 3 && c.size(0) == 2 && c.size(1) == 3 && c.size(2) == 5);

    // Every (i, :, :) slice of the result must be that slice of a times b, and
    // slicing is independent of the contraction code.
    double furthest = 0;

    for(unsigned int i = 0; i < 2; i++) {
        tensor<T> ai = a[i];
        tensor<T> ci = c[i];
        tensor<T> want = ai ^ b;

        for(unsigned int r = 0; r < 3; r++)
            for(unsigned int s = 0; s < 5; s++)
                furthest = std::max(furthest,
                                    std::fabs(at2(ci, r, s) - at2(want, r, s)));
    }

    ok("and each slice of it is that slice of a contracted with b",
       furthest == 0, std::to_string(furthest));

    tensor<T> u(1, 4u);
    tensor<T> v(1, 4u);

    fill(u);
    fill(v, 2.0);

    tensor<T> dot = u ^ v;

    double want = 0;
    for(unsigned int i = 0; i < 4; i++) want += (1.0 + i) * (2.0 + i);

    ok("rank 1 ^ rank 1 is rank 0", dot.rank() == 0, std::to_string(dot.rank()));
    ok("holding the dot product", double(T(dot)) == want,
       std::to_string(double(T(dot))) + " vs " + std::to_string(want));
}

static void contraction_is_associative() {
    std::cout << "\ncontraction is associative:\n";

    tensor<T> a(2, 2u, 3u);
    tensor<T> b(2, 3u, 4u);
    tensor<T> c(2, 4u, 2u);

    fill(a, 0.5);
    fill(b, 1.5);
    fill(c, 0.25);

    // (ab)c == a(bc).  A property of the operation rather than of any one
    // result, so it does not need a second implementation to check against --
    // and it exercises the shape bookkeeping in both orders.
    tensor<T> left = (a ^ b) ^ c;
    tensor<T> right = a ^ (b ^ c);

    double furthest = 0;

    for(unsigned int i = 0; i < 2; i++)
        for(unsigned int j = 0; j < 2; j++)
            furthest = std::max(furthest,
                                std::fabs(at2(left, i, j) - at2(right, i, j)));

    // Exactly equal is too much to ask of floating point in general; these
    // values are small and dyadic, so it happens to hold, and asking for it
    // catches an association error that a loose bound would let through.
    ok("(a^b)^c equals a^(b^c)", furthest == 0, std::to_string(furthest));
}

static void the_outer_product_concatenates_shapes() {
    std::cout << "\nthe outer product concatenates shapes:\n";

    tensor<T> a(2, 2u, 3u);
    tensor<T> b(2, 4u, 5u);

    fill(a);
    fill(b, 0.5);

    tensor<T> c = a * b;

    ok("rank 2 * rank 2 is rank 4", c.rank() == 4, std::to_string(c.rank()));
    ok("shaped (2,3,4,5)",
       c.rank() == 4 && c.size(0) == 2 && c.size(1) == 3 &&
       c.size(2) == 4 && c.size(3) == 5);

    double furthest = 0;

    for(unsigned int i = 0; i < 2; i++)
        for(unsigned int j = 0; j < 3; j++)
            for(unsigned int k = 0; k < 4; k++)
                for(unsigned int l = 0; l < 5; l++) {
                    const double got = c[i][j][k][l];

                    furthest = std::max(furthest,
                                        std::fabs(got - at2(a, i, j) * at2(b, k, l)));
                }

    ok("and every element is the product of its two sources", furthest == 0,
       std::to_string(furthest));

    // Rank 0 is the scaling case, not an error: an empty shape concatenated
    // with a shape is that shape.
    tensor<T> s(0);
    s = 3.0;

    tensor<T> scaled = s * a;

    ok("a rank 0 operand leaves the rank alone", scaled.rank() == 2,
       std::to_string(scaled.rank()));

    double worst = 0;

    for(unsigned int i = 0; i < 2; i++)
        for(unsigned int j = 0; j < 3; j++)
            worst = std::max(worst, std::fabs(at2(scaled, i, j) - 3.0 * at2(a, i, j)));

    ok("and scales every element", worst == 0, std::to_string(worst));
}

static void the_two_products_agree_where_they_meet() {
    std::cout << "\nthe two products agree where they meet:\n";

    // (u (x) v) . w == u (v . w).  The one identity that constrains both
    // operators at once, so a consistent misunderstanding of the layout in
    // both would have to survive it.
    tensor<T> u(1, 3u);
    tensor<T> v(1, 4u);
    tensor<T> w(1, 4u);

    fill(u, 1.0);
    fill(v, 0.5);
    fill(w, 2.0);

    tensor<T> lhs = (u * v) ^ w;

    double vw = 0;
    for(unsigned int i = 0; i < 4; i++) vw += (0.5 + i) * (2.0 + i);

    ok("(u * v) ^ w is rank 1", lhs.rank() == 1, std::to_string(lhs.rank()));

    double furthest = 0;

    for(unsigned int i = 0; i < 3; i++)
        furthest = std::max(furthest, std::fabs(double(lhs[i]) - (1.0 + i) * vw));

    ok("and equals u scaled by v.w", furthest == 0, std::to_string(furthest));
}

static void a_bad_contraction_is_refused() {
    std::cout << "\na bad contraction is refused:\n";

    tensor<T> a(2, 3u, 4u);
    tensor<T> b(2, 5u, 6u);
    tensor<T> s(0);

    bool threw = false;
    try { tensor<T> c = a ^ b; }
    catch(tensor<T>::mismatch&) { threw = true; }

    ok("axes of different lengths do not contract", threw);

    // Not a mismatch that shows up as a wrong answer: rank() - 2 is unsigned,
    // so without the guard this asks for a shape of four billion axes.
    threw = false;
    try { tensor<T> c = s ^ a; }
    catch(tensor<T>::mismatch&) { threw = true; }

    ok("and a rank 0 operand has no axis to contract", threw);

    threw = false;
    try { tensor<T> c = a ^ s; }
    catch(tensor<T>::mismatch&) { threw = true; }

    ok("in either position", threw);
}

static void equality_compares_shape_and_elements() {
    std::cout << "\nequality compares shape and elements:\n";

    tensor<T> a(2, 2u, 3u);
    tensor<T> b(2, 2u, 3u);
    tensor<T> c(2, 3u, 2u);

    fill(a);
    fill(b);
    fill(c);

    ok("equal shapes and equal elements are equal", a == b);

    b[1][2] = 99.0;

    // The version this replaces returned true here, and for a == c: it compared
    // ranks and nothing else.
    ok("one differing element is not equal", a != b);
    ok("and the same elements in a different shape are not equal", a != c);
}

int main() {
    a_scalar_is_rank_zero();
    contraction_is_matrix_multiply_at_rank_two();
    contraction_generalises_past_rank_two();
    contraction_is_associative();
    the_outer_product_concatenates_shapes();
    the_two_products_agree_where_they_meet();
    a_bad_contraction_is_refused();
    equality_compares_shape_and_elements();

    {
        tensor<T> t(2, 3u, 4u);
        fill(t);
        the_accessors_agree(t);
    }

    // What a green run does not establish.
    //
    // That these are tensors in the physics sense.  There is no metric, no dual
    // space, and no distinction between an upper and a lower index, so nothing
    // here transforms under a change of basis -- which is the property that
    // defines the word elsewhere.  These are multiway arrays with a contraction
    // and an outer product, which is what the numerical multilinear algebra
    // literature means by the term and all that is claimed.
    //
    // Nothing about performance.  Contraction is a naive triple loop over the
    // flattened views, with no blocking and no vectorisation, and it is not
    // what math::matrix or the Metal backend use.
    //
    // That the rank-0 guard in operator^ is what makes the two assertions in
    // "a bad contraction is refused" pass.  Removing it leaves them green:
    // a.rank() - 1 underflows to 4294967295, the read off the end of an empty
    // shape returns garbage, and the garbage fails the very next shape check --
    // which throws the same mismatch the test is looking for.  Under
    // -fsanitize=address the unguarded version faults instead, which is the
    // only way seen so far to tell the two apart.  The guard is load-bearing;
    // this test is not what establishes it.
    //
    // That a const tensor is deeply const.  operator[] const returns a const
    // tensor, which blocks `ct[i][j] = x` -- the static_assert above is what
    // checks that -- but the result still shares storage, so copying it into a
    // non-const tensor writes through.  The constness is shallow, as it is for
    // any handle type.  See #143.
    //
    // And only one contraction convention: a's last axis against b's first.
    // Contracting an arbitrary pair of axes, which is what einsum does and what
    // a real tensor library needs, is not here.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
