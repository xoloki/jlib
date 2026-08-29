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

// How much the plot path allocates, and that it still gets the same answer.
//
// Counting allocations rather than timing them, deliberately.  The cost here
// was never arithmetic -- at D=14 the reduction is about 40 MFLOP a frame and
// it was taking 55ms, which is a couple of percent of what a core does -- so
// the number that means anything is how many times it goes to the heap.  It is
// also exact, where a wall-clock threshold on a shared machine is a coin toss.
//
// The bounds below are deliberately loose.  They are not a performance target;
// they are there to fail if someone reintroduces a per-vertex temporary, which
// moves these by tens rather than by ones.

#include <jlib/math/Plot.hh>
#include <jlib/math/matrix.hh>

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

static std::size_t allocs = 0;

void* operator new(std::size_t n) { ++allocs; return std::malloc(n); }
void* operator new[](std::size_t n) { ++allocs; return std::malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace jlib::math;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Plot is abstract; this reaches the protected transform() and draws nothing. */
struct probe : public Plot<double> {
    probe(uint d, std::vector<std::pair<double,double> > c)
        : Plot<double>(d, c, 1024, 768) {}

    void draw_point(std::pair<uint,uint>, uint) {}
    void draw_line(std::pair<uint,uint>, std::pair<uint,uint>, uint, uint) {}

    vertex<double> run(const vertex<double>& v) const { return transform(v); }
};

static double per_corner(uint D, Plot<double>::projection_mode mode,
                         std::vector<vertex<double> >& out)
{
    std::vector<std::pair<double,double> > clip;

    for(uint i = 0; i < D; i++)
        clip.push_back(std::make_pair(-10.0, 10.0));

    cuboid<double> shape(D);
    probe p(D, clip);

    p.set_projection_mode(mode);

    // Warm the cached projections and the modelview product, so this counts
    // the steady state rather than the first frame.
    p.run(shape[0]);

    out.clear();

    const std::size_t before = allocs;

    for(uint i = 0; i < shape.size(); i++)
        out.push_back(p.run(shape[i]));

    const std::size_t used = allocs - before;

    // out.push_back allocates too; subtract the vector's own growth by
    // measuring it separately would be fiddly, so reserve up front instead.
    return double(used) / shape.size();
}

static void the_reduction_does_not_allocate_per_step() {
    std::cout << "\nthe reduction does not allocate per step:\n";

    const uint D = 9;

    std::vector<vertex<double> > mixed, persp;

    mixed.reserve(1 << D);
    persp.reserve(1 << D);

    const double m = per_corner(D, Plot<double>::projection_mode::mixed, mixed);
    const double p = per_corner(D, Plot<double>::projection_mode::perspective, persp);

    // Measured at 28 and 42 when this was written, against 39 and 75 before
    // the pass.  The bound is what a regression would blow through, not a
    // target: reinstating one per-vertex temporary costs D-3 allocations a
    // corner, which is six here.
    ok("mixed stays under 35 allocations per corner", m < 35,
       std::to_string(m));

    ok("perspective stays under 52", p < 52, std::to_string(p));

    // Perspective reduces all the way down; mixed keeps its dimensionality.
    // Both are deliberate -- see transform() -- and getting them the wrong way
    // round is the bug that made every step after the first orthographic.
    ok("perspective reduces to two dimensions",
       !persp.empty() && persp.front().D == 2,
       persp.empty() ? "none" : std::to_string(persp.front().D));

    ok("and mixed does not", !mixed.empty() && mixed.front().D == D,
       mixed.empty() ? "none" : std::to_string(mixed.front().D));
}

static void the_answer_did_not_change() {
    std::cout << "\nthe answer did not change:\n";

    // Captured from master before the allocation pass and asserted here, so a
    // future optimisation that is merely fast has to stay correct too.  A
    // cuboid's corners are +/-1 on every axis, so these are exact in binary
    // and comparing them without a tolerance is honest rather than lucky.
    std::vector<vertex<double> > persp;

    persp.reserve(1 << 9);

    per_corner(9, Plot<double>::projection_mode::perspective, persp);

    ok("the first corner is where it was",
       persp.size() > 2 && persp[0][0] == 1.0 && persp[0][1] == 1.0,
       persp.empty() ? "none"
                     : std::to_string(persp[0][0]) + ", " + std::to_string(persp[0][1]));

    ok("and so is the second",
       persp.size() > 2 && persp[1][0] == -1.0 && persp[1][1] == 1.0,
       persp.size() < 2 ? "none"
                        : std::to_string(persp[1][0]) + ", " + std::to_string(persp[1][1]));

    ok("and the third", persp.size() > 2 && persp[2][0] == 1.0 && persp[2][1] == -1.0,
       persp.size() < 3 ? "none"
                        : std::to_string(persp[2][0]) + ", " + std::to_string(persp[2][1]));
}

int main() {
    std::cout << std::unitbuf;

    the_reduction_does_not_allocate_per_step();
    the_answer_did_not_change();

    // What a green run does not establish.
    //
    // That the plot path is fast, or that anything renders correctly.  This
    // counts heap traffic through transform() and checks three corners land
    // where they used to; it never draws, and the apps that exercise this for
    // real -- jhyper, jglfwhyper, jhardhyper, jhypermusic -- are looked at
    // rather than asserted.
    //
    // Not the remaining allocations, which are real and are the point of the
    // next branch: the per-step vertex and the by-value return are still
    // there, and removing them means a batched transform writing into caller
    // storage.  That is also the shape Metal needs, which is why the two are
    // one piece of work rather than two.
    //
    // Not jhardhyper, which overrides transform() with its own copy of this
    // loop (#28) and therefore gets none of this.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
