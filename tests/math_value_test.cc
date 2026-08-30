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

// matrix and vertex are values.
//
// They were not.  rep is a buffer and a buffer holds a shared_ptr, so copying
// a matrix copied the handle: two matrices, one array.  vertex then declared
// an element-wise operator= on top, so copying aliased and assigning did not,
// and the two read identically at the call site.
//
// Every assertion here fails against the old code.  That is the point of the
// file -- the semantics are not observable except by writing through one
// handle and reading the other, which is exactly what nobody does on purpose.

#include <jlib/math/matrix.hh>
#include <jlib/math/tensor.hh>

#include <iostream>
#include <string>
#include <vector>

using jlib::math::matrix;
using jlib::math::vertex;
using jlib::math::buffer;
using jlib::math::tensor;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void a_matrix_is_a_value() {
    std::cout << "\na matrix is a value:\n";

    {
        matrix<double> a(2,2);
        a(0,0) = 1;

        matrix<double> b = a;
        b(0,0) = 99;

        ok("copy-construction copies", a(0,0) == 1,
           "a(0,0) = " + std::to_string(a(0,0)));
    }

    {
        matrix<double> a(2,2);
        a(0,0) = 1;

        matrix<double> b(2,2);
        b = a;
        b(0,0) = 99;

        ok("assignment copies", a(0,0) == 1,
           "a(0,0) = " + std::to_string(a(0,0)));
    }

    {
        // The one that used to be true by accident: both were shallow, so
        // matrix at least agreed with itself.  It has to keep agreeing.
        matrix<double> a(2,2);
        a(0,0) = 7;

        matrix<double> viacopy = a;
        matrix<double> viaassign(2,2);
        viaassign = a;

        ok("and both do the same thing",
           viacopy(0,0) == 7 && viaassign(0,0) == 7);
    }

    {
        matrix<double> a(3,3);
        a(1,1) = 5;

        a = a;   // NOLINT: self-assignment is the assertion

        ok("self-assignment leaves it intact", a(1,1) == 5,
           std::to_string(a(1,1)));
    }

    {
        // Moves were suppressed by declaring a copy, so they are declared back.
        matrix<double> a(2,2);
        a(0,1) = 3;

        matrix<double> b = std::move(a);

        ok("moving carries the values", b(0,1) == 3, std::to_string(b(0,1)));
    }
}

static void a_vertex_is_a_value() {
    std::cout << "\na vertex is a value:\n";

    {
        vertex<double> a(3);
        a[0] = 1;

        vertex<double> b = a;
        b[0] = 99;

        ok("copy-construction copies", a[0] == 1, std::to_string(a[0]));
    }

    {
        vertex<double> a(3);
        a[0] = 1;

        vertex<double> b(3);
        b = a;
        b[0] = 99;

        ok("assignment copies", a[0] == 1, std::to_string(a[0]));
    }

    {
        // Not changed, and pinned so that it is not changed by accident:
        // operator= takes values into the shape this already has.  change()
        // depends on the truncation.
        vertex<double> big(4);
        for(int i = 0; i < 4; i++) big[i] = i + 1;

        vertex<double> small(2);
        small = big;

        ok("assignment does not resize", small.D == 2, std::to_string(small.D));
        ok("and takes what fits", small[0] == 1 && small[1] == 2);
    }

    {
        vertex<double> a(3);
        a[0] = 1; a[1] = 2; a[2] = 3;

        a.change(5);

        ok("change() keeps the values it can", a.D == 5 && a[0] == 1 && a[2] == 3,
           "D = " + std::to_string(a.D));
    }
}

static void data_points_at_d_plus_one() {
    std::cout << "\ndata() points at D+1 components:\n";

    // The invariant jhardhyper's dimension floor rests on.  vertex(n) builds
    // an (n+1)x1 column -- n coordinates and a homogeneous 1 -- so data()
    // addresses D+1 doubles and no more.  A fixed-arity read of four, which
    // is what glVertex4dv does, therefore needs D >= 3.
    for(uint d = 1; d <= 5; d++) {
        vertex<double> v(d);

        for(uint i = 0; i < d; i++)
            v[i] = i + 1;

        bool placed = true;

        for(uint i = 0; i < d; i++)
            if(v.data()[i] != double(i + 1)) placed = false;

        ok("D=" + std::to_string(d) + ": every coordinate is where data() says",
           placed);

        // Index D is the homogeneous coordinate, and reaching it is what says
        // the buffer really holds D+1 rather than D.
        ok("  and index D is the homogeneous 1", v.data()[d] == 1.0,
           std::to_string(v.data()[d]));
    }
}

static void the_jhypermusic_case() {
    std::cout << "\nthe case from the field:\n";

    // jhypermusic kept the current and previous pose of every corner so it
    // could work out a radial velocity.  Both vectors were filled from the
    // same source, so under the old semantics all three aliased -- the
    // snapshot was a no-op and every corner reported exactly zero velocity
    // while the shape visibly turned on screen.
    std::vector<vertex<double>> at, was;

    vertex<double> shape(3);
    shape[0] = 1;

    at.push_back(shape);
    was.push_back(shape);

    was[0] = at[0];        // snapshot the old pose
    at[0][0] = 42;         // then move

    ok("a vector of vertices holds independent elements", was[0][0] == 1,
       "was[0][0] = " + std::to_string(was[0][0]));

    ok("and the source is untouched too", shape[0] == 1,
       "shape[0] = " + std::to_string(shape[0]));

    const double velocity = at[0][0] - was[0][0];

    ok("so a difference of two poses is not identically zero", velocity == 41,
       std::to_string(velocity));
}

static void transpose_is_a_value_too() {
    std::cout << "\ntranspose is a value too:\n";

    matrix<int> a(2,3);
    int n = 0;
    for(unsigned r = 0; r < 2; r++)
        for(unsigned c = 0; c < 3; c++)
            a(r,c) = ++n;

    matrix<int> t = a.transpose();

    ok("the shape is transposed", t.M == 3 && t.N == 2,
       std::to_string(t.M) + "x" + std::to_string(t.N));

    bool same = true;
    for(unsigned r = 0; r < 2; r++)
        for(unsigned c = 0; c < 3; c++)
            if(t(c,r) != a(r,c)) same = false;

    ok("and so are the values", same);

    t(0,0) = 999;

    // It used to hand back a view with a flag flipped, so this wrote through.
    ok("writing to it does not write through to the source", a(0,0) == 1,
       "a(0,0) = " + std::to_string(a(0,0)));

    ok("transposing twice is the original", a.transpose().transpose() == a);
}

static void the_submatrix_constructor_works() {
    std::cout << "\nthe submatrix constructor works:\n";

    // #53.  Empty body and no initialiser list, so M and N were uninitialised
    // and rep was null -- undefined behaviour, not a no-op.  Nothing called it.
    matrix<int> a(4,4);
    int n = 0;
    for(unsigned r = 0; r < 4; r++)
        for(unsigned c = 0; c < 4; c++)
            a(r,c) = ++n;

    matrix<int> s(2,2,a,1,1);

    ok("it has the size asked for", s.M == 2 && s.N == 2,
       std::to_string(s.M) + "x" + std::to_string(s.N));

    ok("anchored where asked", s(0,0) == a(1,1) && s(1,1) == a(2,2),
       std::to_string(s(0,0)) + "," + std::to_string(s(1,1)));

    s(0,0) = 777;

    ok("and it is a copy, not a window", a(1,1) != 777,
       "a(1,1) = " + std::to_string(a(1,1)));

    bool threw = false;
    try { matrix<int> bad(3,3,a,2,2); }
    catch(std::exception&) { threw = true; }

    ok("a submatrix that runs off the end is refused", threw);
}

static void buffer_keeps_its_reference_semantics() {
    std::cout << "\nbuffer keeps its reference semantics:\n";

    // Deliberately unchanged.  tensor slices with it -- operator[] builds a
    // sub-view with buffer(b, offset, size) -- and that is what buffer is for.
    // What changed is that matrix no longer leaks those semantics by accident.
    buffer<double> a(4);
    a[0] = 1;

    buffer<double> b = a;
    b[0] = 99;

    ok("a buffer copy still shares", a[0] == 99, std::to_string(a[0]));

    buffer<unsigned int> meta(2);
    meta[0] = 2; meta[1] = 2;

    tensor<double> t(meta);
    t[0][0] = 5;

    ok("and a tensor slice still writes through to its parent",
       t[0][0] == 5, std::to_string(t[0][0]));
}

int main() {
    std::cout << std::unitbuf;

    a_matrix_is_a_value();
    a_vertex_is_a_value();
    data_points_at_d_plus_one();
    the_jhypermusic_case();
    transpose_is_a_value_too();
    the_submatrix_constructor_works();
    buffer_keeps_its_reference_semantics();

    // What a green run does not establish.
    //
    // That nothing depended on the old aliasing.  The whole suite passes and
    // the tree builds, but the apps that use this most -- jhyper, jglfwhyper,
    // jhardhyper, jhypermusic -- are rendered rather than asserted, and a
    // wrong transform there looks like a picture rather than a failure.
    // jhypermusic reporting a non-zero Doppler again is the check that wants
    // a pair of ears.
    //
    // Not the cost.  Copies are real now where they used to be free, and the
    // plot path copies per frame (#47).  A backprop-shaped benchmark measured
    // 0.24ms/step before and after, because a materialised transpose is O(MN)
    // feeding a multiply that is O(MNK) -- but that is one shape, not the
    // plot path.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
