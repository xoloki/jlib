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

// Training a batch.
//
// jlib/ai had no test at all, which is how it came to be in the tree since
// 2020 with a training step whose behaviour changed with the batch size and
// nothing saying so.
//
// The weights are compared through json(), which is the only way out of the
// class.  That is a little indirect, but it is exact -- comparing what query()
// returns would put a sigmoid between the assertion and the thing asserted.

#include <jlib/ai/neural.hh>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using jlib::ai::NeuralNetwork;
using jlib::math::matrix;

namespace json = jlib::util::json;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static const uint I = 4, H = 5, O = 3;

static std::vector<uint> hidden() { return std::vector<uint>{ H }; }

/** Every weight, flattened, so two networks can be compared number by number. */
static std::vector<double> weights(NeuralNetwork<double>& nn) {
    json::object::ptr p = nn.json();

    std::vector<double> out;

    for(uint i = 0; i < H * I; i++) out.push_back(p->obj("wih")->get(i));
    for(uint i = 0; i < O * H; i++) out.push_back(p->obj("who")->get(i));

    return out;
}

static std::vector<double> delta(const std::vector<double>& a,
                                 const std::vector<double>& b) {
    std::vector<double> d;

    for(std::size_t i = 0; i < a.size() && i < b.size(); i++)
        d.push_back(b[i] - a[i]);

    return d;
}

static double worst(const std::vector<double>& a, const std::vector<double>& b) {
    double w = 0;

    for(std::size_t i = 0; i < a.size() && i < b.size(); i++)
        w = std::max(w, std::fabs(a[i] - b[i]));

    return w;
}

/** Sample k, made up but distinct. */
static matrix<double> input(uint k, uint n = 1) {
    matrix<double> x(I, n);

    for(uint c = 0; c < n; c++)
        for(uint r = 0; r < I; r++)
            x(r, c) = 0.1 * double(r + 1) + 0.07 * double(k + c);

    return x;
}

static matrix<double> target(uint k, uint n = 1) {
    matrix<double> t(O, n);

    for(uint c = 0; c < n; c++)
        for(uint r = 0; r < O; r++)
            t(r, c) = 0.2 + 0.1 * double((r + k + c) % 3);

    return t;
}

static void a_batch_is_accepted() {
    std::cout << "\na batch is accepted:\n";

    NeuralNetwork<double> nn(0.1, I, hidden(), O);

    const matrix<double> e1 = nn.train(input(0), target(0));

    ok("one sample gives one column of errors", e1.M == O && e1.N == 1,
       std::to_string(e1.M) + "x" + std::to_string(e1.N));

    const matrix<double> e4 = nn.train(input(0, 4), target(0, 4));

    ok("four samples give four", e4.M == O && e4.N == 4,
       std::to_string(e4.M) + "x" + std::to_string(e4.N));

    const matrix<double> q = nn.query(input(0, 4));

    ok("and query answers a batch too", q.M == O && q.N == 4,
       std::to_string(q.M) + "x" + std::to_string(q.N));
}

static void the_gradient_is_a_mean() {
    std::cout << "\nthe gradient is a mean:\n";

    NeuralNetwork<double> start(0.1, I, hidden(), O);

    // Identical starting weights, via the serialisation the class already had.
    NeuralNetwork<double> one(start.json());
    NeuralNetwork<double> many(start.json());

    const std::vector<double> before = weights(start);

    ok("the json round-trip preserves the weights",
       worst(before, weights(one)) == 0.0,
       std::to_string(worst(before, weights(one))));

    one.train(input(0), target(0));

    // Eight copies of the same sample.  A mean gradient over identical
    // gradients is that gradient, so this must move exactly as far as one.
    matrix<double> x(I, 8), t(O, 8);

    for(uint c = 0; c < 8; c++) {
        for(uint r = 0; r < I; r++) x(r, c) = input(0)(r, 0);
        for(uint r = 0; r < O; r++) t(r, c) = target(0)(r, 0);
    }

    many.train(x, t);

    const double w = worst(weights(one), weights(many));

    // Summing instead would move it eight times as far, so the bound does not
    // need to be tight to be decisive.
    ok("a batch of eight identical samples moves as far as one", w < 1e-12,
       "worst weight difference " + std::to_string(w));
}

static void it_is_the_mean_of_the_samples() {
    std::cout << "\nit is the mean of the samples:\n";

    NeuralNetwork<double> start(0.1, I, hidden(), O);

    NeuralNetwork<double> a(start.json());
    NeuralNetwork<double> b(start.json());
    NeuralNetwork<double> both(start.json());

    const std::vector<double> before = weights(start);

    a.train(input(0), target(0));
    b.train(input(1), target(1));
    both.train(input(0, 2), target(0, 2));

    const std::vector<double> da = delta(before, weights(a));
    const std::vector<double> db = delta(before, weights(b));
    const std::vector<double> dboth = delta(before, weights(both));

    // The output layer's step is exactly the average of the two single steps,
    // because all three gradients are taken at the same weights.
    //
    // The hidden layer's is only close, and that is not a rounding story: it
    // is 1.5e-6 where the output layer is 5.6e-17.  train() updates m_who and
    // *then* backpropagates through the updated m_who, so the hidden gradient
    // is not evaluated at the weights the step started from.  Textbook
    // backprop takes every gradient at the same weights and applies them
    // afterwards.  See the issue filed alongside this branch -- it predates
    // batching, and changing it would change how every existing network
    // trains, so it is recorded rather than fixed here.
    //
    // The split is asserted rather than glossed, so that if anyone does fix
    // it, this test says which half was already right.
    std::vector<double> mean_who, batch_who, mean_wih, batch_wih;

    for(std::size_t i = 0; i < da.size(); i++) {
        const bool is_wih = i < H * I;

        (is_wih ? mean_wih : mean_who).push_back(0.5 * (da[i] + db[i]));
        (is_wih ? batch_wih : batch_who).push_back(dboth[i]);
    }

    const double wo = worst(mean_who, batch_who);
    const double wi = worst(mean_wih, batch_wih);

    ok("the output layer's step is exactly the average", wo < 1e-14,
       "worst difference " + std::to_string(wo));

    ok("the hidden layer's is close but not exact", wi > 1e-12 && wi < 1e-3,
       "worst difference " + std::to_string(wi) +
       " -- backprop goes through the already-updated m_who");

    // And it is not the same as taking them one after the other, which is a
    // different algorithm rather than a slower version of the same one.
    NeuralNetwork<double> seq(start.json());

    seq.train(input(0), target(0));
    seq.train(input(1), target(1));

    const double s = worst(weights(both), weights(seq));

    ok("and is not the same as two sequential steps", s > 1e-12,
       "difference " + std::to_string(s));
}

static void a_mismatched_batch_is_refused() {
    std::cout << "\na mismatched batch is refused:\n";

    NeuralNetwork<double> nn(0.1, I, hidden(), O);

    bool threw = false;

    try { nn.train(input(0, 4), target(0, 2)); }
    catch(std::exception&) { threw = true; }

    // Four inputs and two targets used to reach the subtraction and throw
    // there, from math::matrix, about a shape nobody had mentioned.
    ok("inputs and targets must agree about the batch size", threw);
}

int main() {
    std::cout << std::unitbuf;

    a_batch_is_accepted();
    the_gradient_is_a_mean();
    it_is_the_mean_of_the_samples();
    a_mismatched_batch_is_refused();

    // What a green run does not establish.
    //
    // That the network learns anything.  Nothing here trains to convergence or
    // checks an error goes down; these are assertions about one step, which is
    // where the batch semantics live.  Whether MNIST still converges is a
    // question for jneural-alpha and a dataset, not for a unit test.
    //
    // Not float.  Everything here is NeuralNetwork<double>, because that is
    // what the apps use.  The class is a template and float should behave the
    // same, but "should" is not a test, and the Metal path that wants float is
    // not wired up yet.
    //
    // Not the speed.  Batching measured 2.5x faster on the CPU at a batch of
    // 32 for a 784-200-10 network, which is in the branch write-up rather than
    // asserted here, because a wall-clock bound on a shared machine is a coin
    // toss.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
