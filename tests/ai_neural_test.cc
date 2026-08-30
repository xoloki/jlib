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

    // Every layer's step is exactly the average of the two single steps,
    // because all three gradients are taken at the same weights.
    //
    // The hidden half of this used to be only *close* -- 1.5e-6 where the
    // output layer was 5.6e-17 -- because train() updated m_who and then
    // backpropagated through the updated m_who (#130).  The two halves are
    // still asserted separately rather than together: the output layer was
    // right all along, and keeping them apart is what made the difference
    // between them visible in the first place.
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

    // 1e-14 rather than 0: the two sides reach the same value by different
    // routes -- one averages two products, the other averages inside the
    // gradient -- so last-bit rounding is allowed and 1.5e-6 is not.
    ok("and so is the hidden layer's, which it was not before #130", wi < 1e-14,
       "worst difference " + std::to_string(wi));

    // And it is not the same as taking them one after the other, which is a
    // different algorithm rather than a slower version of the same one.
    NeuralNetwork<double> seq(start.json());

    seq.train(input(0), target(0));
    seq.train(input(1), target(1));

    const double s = worst(weights(both), weights(seq));

    ok("and is not the same as two sequential steps", s > 1e-12,
       "difference " + std::to_string(s));
}

static void weights_are_scaled_by_the_fan_in() {
    std::cout << "\nweights are scaled by the fan in:\n";

    // 1/sqrt(fan-in) is the heuristic, and the fan-in of a weight matrix is
    // the layer feeding it, not the layer it feeds.  Deliberately asymmetric
    // sizes: with equal widths the two readings coincide, which is exactly why
    // the wrong one survived (#131).
    const uint ni = 100, no = 4;
    const std::vector<uint> deep{ 25, 9 };

    NeuralNetwork<double> nn(0.1, ni, deep, no);

    json::object::ptr p = nn.json();

    double wih = 0, who = 0, d0 = 0;

    for(uint i = 0; i < deep.front() * ni; i++)
        wih = std::max(wih, std::fabs(double(p->obj("wih")->get(i))));

    for(uint i = 0; i < no * deep.back(); i++)
        who = std::max(who, std::fabs(double(p->obj("who")->get(i))));

    for(uint i = 0; i < deep[1] * deep[0]; i++)
        d0 = std::max(d0, std::fabs(double(p->obj("deep")->obj(0)->get(i))));

    // Each bound, and the one the old code would have used.  The gap between
    // them is what makes these assertions worth anything: 1/sqrt(100) = 0.1
    // against 1/sqrt(25) = 0.2, and 1/sqrt(9) = 0.333 against 1/sqrt(4) = 0.5.
    ok("wih is bounded by 1/sqrt(inputs)", wih <= std::pow(double(ni), -0.5),
       std::to_string(wih) + " <= " + std::to_string(std::pow(double(ni), -0.5)));

    ok("  and not by 1/sqrt(first hidden), which is looser",
       wih <= std::pow(double(deep.front()), -0.5) &&
       std::pow(double(ni), -0.5) < std::pow(double(deep.front()), -0.5));

    ok("who is bounded by 1/sqrt(last hidden)",
       who <= std::pow(double(deep.back()), -0.5),
       std::to_string(who) + " <= " + std::to_string(std::pow(double(deep.back()), -0.5)));

    ok("  and not by 1/sqrt(outputs), which is looser",
       who <= std::pow(double(no), -0.5) &&
       std::pow(double(deep.back()), -0.5) < std::pow(double(no), -0.5));

    ok("a deep layer is bounded by 1/sqrt(the layer below)",
       d0 <= std::pow(double(deep[0]), -0.5),
       std::to_string(d0) + " <= " + std::to_string(std::pow(double(deep[0]), -0.5)));

    // A bound nothing reaches is not a bound anyone can check, so the
    // distribution has to actually fill it.  Uniform over the range, so the
    // largest of a few hundred draws sits close to the edge.
    ok("and the range is used rather than merely respected",
       wih > 0.8 * std::pow(double(ni), -0.5),
       std::to_string(wih));
}

static void each_activation_and_its_slope() {
    std::cout << "\neach activation and its slope:\n";

    using jlib::ai::activation;
    using jlib::ai::activate;
    using jlib::ai::activate_slope;
    using jlib::ai::LEAK;

    matrix<double> x(1, 4);
    x(0,0) = -2.0; x(0,1) = -0.5; x(0,2) = 0.5; x(0,3) = 2.0;

    const matrix<double> sig = activate(activation::sigmoid, x);
    const matrix<double> rel = activate(activation::relu, x);
    const matrix<double> lky = activate(activation::leaky_relu, x);
    const matrix<double> tnh = activate(activation::tanh, x);

    ok("sigmoid is 1/(1+exp(-x))",
       std::fabs(sig(0,2) - 1.0/(1.0+std::exp(-0.5))) < 1e-15,
       std::to_string(sig(0,2)));

    ok("relu zeroes what is negative and passes what is not",
       rel(0,0) == 0.0 && rel(0,1) == 0.0 && rel(0,2) == 0.5 && rel(0,3) == 2.0);

    ok("leaky relu leans rather than flattens",
       std::fabs(lky(0,0) - LEAK * -2.0) < 1e-15 && lky(0,3) == 2.0,
       std::to_string(lky(0,0)));

    ok("tanh is tanh", std::fabs(tnh(0,3) - std::tanh(2.0)) < 1e-15);

    // The slope is taken from the *output*, which is what lets these be
    // interchangeable without train() caching the pre-activations as well.
    const matrix<double> dsig = activate_slope(activation::sigmoid, sig);
    const matrix<double> drel = activate_slope(activation::relu, rel);
    const matrix<double> dlky = activate_slope(activation::leaky_relu, lky);
    const matrix<double> dtnh = activate_slope(activation::tanh, tnh);

    ok("sigmoid's slope is s(1-s)",
       std::fabs(dsig(0,2) - sig(0,2)*(1.0-sig(0,2))) < 1e-15);

    ok("relu's slope is one above zero and nothing at or below",
       drel(0,0) == 0.0 && drel(0,1) == 0.0 && drel(0,2) == 1.0 && drel(0,3) == 1.0);

    // The reason leaky exists: a unit that has gone negative still has a
    // gradient, so it can come back.  relu's cannot.
    ok("leaky relu keeps a slope where relu has none",
       dlky(0,0) == LEAK && drel(0,0) == 0.0,
       std::to_string(dlky(0,0)));

    ok("tanh's slope is 1 - s^2",
       std::fabs(dtnh(0,3) - (1.0 - tnh(0,3)*tnh(0,3))) < 1e-15);
}

static void the_activation_survives_a_round_trip() {
    std::cout << "\nthe activation survives a round trip:\n";

    using jlib::ai::activation;

    NeuralNetwork<double> a(0.1, I, hidden(), O);

    ok("both default to sigmoid",
       a.get_hidden_activation() == activation::sigmoid &&
       a.get_output_activation() == activation::sigmoid);

    a.set_hidden_activation(activation::relu);

    // Hidden and output are set separately because they want different
    // answers -- relu hidden with a sigmoid output is the ordinary
    // arrangement, and relu on the output of a classifier scored against
    // 0.01/0.99 kills any unit that goes negative.
    NeuralNetwork<double> b(a.json());

    ok("the hidden choice comes back",
       b.get_hidden_activation() == activation::relu);

    ok("and the output was not dragged along with it",
       b.get_output_activation() == activation::sigmoid);

    // Written by name, so reordering the enum cannot silently change what a
    // saved file means.
    json::object::ptr p = a.json();

    ok("it is stored by name", std::string(p->get("hidden_activation")) == "relu",
       std::string(p->get("hidden_activation")));

    // A network saved before these fields existed must still load.  Simulated
    // by serialising one and cutting the fields back out of the text, which is
    // what such a file is.
    std::string text = a.json()->str(false);

    for(const char* key : { "\"hidden_activation\"", "\"output_activation\"" }) {
        const std::size_t at = text.find(key);

        if(at == std::string::npos) continue;

        std::size_t end = text.find(',', at);

        if(end == std::string::npos) {
            // Last member: take the preceding comma instead.
            end = text.rfind(',', at);
            text.erase(end, text.find('}', at) - end);
        }
        else {
            text.erase(at, end - at + 1);
        }
    }

    ok("the fields really are gone from the text",
       text.find("hidden_activation") == std::string::npos,
       text.substr(0, 60) + "...");

    NeuralNetwork<double> old(json::object::create(text));

    ok("and a file that predates them loads as sigmoid",
       old.get_hidden_activation() == activation::sigmoid &&
       old.get_output_activation() == activation::sigmoid,
       jlib::ai::name_of(old.get_hidden_activation()));

    bool threw = false;
    try { jlib::ai::activation_from_name("wibble"); }
    catch(std::exception&) { threw = true; }

    ok("an unknown name is refused rather than defaulted", threw);
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
    weights_are_scaled_by_the_fan_in();
    each_activation_and_its_slope();
    the_activation_survives_a_round_trip();
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
