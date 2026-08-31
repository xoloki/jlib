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

#include <jlib/ai/sampler.hh>

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace ai = jlib::ai;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void greedy_is_not_a_draw() {
    std::cout << "\ngreedy is not a draw:\n";

    const std::vector<float> logits{ 1.0f, 3.0f, 2.0f, 0.5f };

    ok("  argmax finds the largest", ai::sampler::argmax(logits) == 1);

    ai::sampler::config c;
    c.temperature = 0.0f;

    // Two samplers with *different* seeds.  At temperature zero neither draws,
    // so the seed cannot matter -- which is the property that makes a greedy
    // run reproducible without anybody recording a seed.
    c.seed = 1;
    ai::sampler a(c);

    c.seed = 999999;
    ai::sampler b(c);

    bool same = true;

    for(int i = 0; i < 20; i++)
        if(a.pick(logits) != 1 || b.pick(logits) != 1) same = false;

    ok("  and temperature zero always takes it, whatever the seed", same);
}

static void the_same_seed_gives_the_same_tokens() {
    std::cout << "\nthe same seed gives the same tokens:\n";

    std::vector<float> logits;

    for(int i = 0; i < 50; i++) logits.push_back(float(i % 7) * 0.5f);

    ai::sampler::config c;
    c.temperature = 1.0f;
    c.top_k = 0;
    c.top_p = 1.0f;
    c.seed = 42;

    ai::sampler a(c), b(c);

    std::vector<int> first, second;

    for(int i = 0; i < 30; i++) { first.push_back(a.pick(logits));
                                  second.push_back(b.pick(logits)); }

    ok("  two samplers with one seed agree", first == second);

    c.seed = 43;
    ai::sampler d(c);

    std::vector<int> third;

    for(int i = 0; i < 30; i++) third.push_back(d.pick(logits));

    // Not a law -- two seeds *could* agree by chance -- but over thirty draws
    // from a spread distribution it would be remarkable.
    ok("  and a different seed does not", first != third);

    a.reseed(42);

    std::vector<int> again;

    for(int i = 0; i < 30; i++) again.push_back(a.pick(logits));

    ok("  reseeding starts the sequence over", first == again);
}

static void top_k_and_top_p_cut_where_they_say() {
    std::cout << "\ntop k and top p cut where they say:\n";

    const std::vector<float> logits{ 0.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f };

    ai::sampler::config c;
    c.temperature = 1.0f;
    c.top_p = 1.0f;
    c.seed = 7;

    c.top_k = 1;
    ai::sampler one(c);

    bool always = true;

    for(int i = 0; i < 50; i++) if(one.pick(logits) != 1) always = false;

    ok("  top_k 1 is greedy by another route", always);

    c.top_k = 3;
    ai::sampler three(c);

    ok("  top_k 3 keeps three", three.distribution(logits).size() == 3,
       std::to_string(three.distribution(logits).size()));

    bool in_range = true;

    for(int i = 0; i < 200; i++) {
        const int t = three.pick(logits);

        if(t != 1 && t != 2 && t != 3) in_range = false;
    }

    ok("  and never draws outside them", in_range);

    // A nucleus of nothing is not a choice, so the smallest p still keeps one.
    c.top_k = 0;
    c.top_p = 0.0001f;
    ai::sampler tiny(c);

    ok("  the smallest top_p still keeps one", tiny.distribution(logits).size() == 1,
       std::to_string(tiny.distribution(logits).size()));

    bool only_best = true;

    for(int i = 0; i < 50; i++) if(tiny.pick(logits) != 1) only_best = false;

    ok("  which is the likeliest", only_best);
}

/**
 * The draw follows the distribution, not merely the order.
 *
 * Two tokens whose logits differ by ln(3) should come up three times to one.
 * Nothing else here checks that the probabilities are *used* -- every other
 * assertion would pass on a sampler that always returned its first choice.
 */
static void the_draw_follows_the_probabilities() {
    std::cout << "\nthe draw follows the probabilities:\n";

    std::vector<float> logits{ float(std::log(3.0)), 0.0f };

    ai::sampler::config c;
    c.temperature = 1.0f;
    c.top_k = 0;
    c.top_p = 1.0f;
    c.seed = 11;

    ai::sampler s(c);

    const std::vector<std::pair<int, float> > d = s.distribution(logits);

    ok("  the distribution is 0.75 and 0.25",
       d.size() == 2 && std::fabs(d[0].second - 0.75f) < 1e-4f &&
       std::fabs(d[1].second - 0.25f) < 1e-4f,
       d.size() == 2 ? std::to_string(d[0].second) + " and " +
                       std::to_string(d[1].second) : "wrong size");

    int zero = 0;

    const int draws = 20000;

    for(int i = 0; i < draws; i++) if(s.pick(logits) == 0) zero++;

    const double got = double(zero) / double(draws);

    // Three sigma for 20000 draws at p=0.75 is about 0.009, so 0.02 is loose
    // enough never to flake and tight enough that 50/50 or 100/0 fails.
    ok("  and twenty thousand draws land near it", std::fabs(got - 0.75) < 0.02,
       std::to_string(got));
}

/**
 * Temperature sharpens and flattens, by exactly how much.
 *
 * Dividing the logits by t scales their differences by 1/t, so two tokens
 * ln(3) apart are ln(3)/t apart afterwards and the top probability follows
 * exactly. Every other assertion here runs at temperature 1, where the
 * division does nothing -- measured: removing it entirely failed nothing until
 * this existed.
 */
static void temperature_moves_the_mass() {
    std::cout << "\ntemperature moves the mass:\n";

    const std::vector<float> logits{ float(std::log(3.0)), 0.0f };

    ai::sampler::config c;
    c.top_k = 0;
    c.top_p = 1.0f;
    c.seed = 3;

    struct { float t; double want; const char* what; } cases[] = {
        // exp(ln3 / t) / (exp(ln3 / t) + 1)
        { 1.0f, 3.0 / 4.0,                     "at one, three to one" },
        { 0.5f, 9.0 / 10.0,                    "at a half, nine to one" },
        { 2.0f, std::sqrt(3.0) / (std::sqrt(3.0) + 1.0), "at two, flatter" }
    };

    for(const auto& e : cases) {
        c.temperature = e.t;

        ai::sampler s(c);

        const std::vector<std::pair<int, float> > d = s.distribution(logits);

        ok(std::string("  ") + e.what,
           d.size() == 2 && std::fabs(double(d[0].second) - e.want) < 1e-4,
           d.empty() ? "empty" : std::to_string(d[0].second) + " wanted " +
                                 std::to_string(e.want));
    }

    // And it is not only the reported distribution that changes: a colder
    // sampler really does draw the favourite more often.
    c.temperature = 0.25f;
    ai::sampler cold(c);

    c.temperature = 4.0f;
    ai::sampler hot(c);

    int cold_top = 0;
    int hot_top = 0;

    for(int i = 0; i < 4000; i++) {
        if(cold.pick(logits) == 0) cold_top++;
        if(hot.pick(logits) == 0) hot_top++;
    }

    ok("  and a colder sampler draws the favourite more often",
       cold_top > hot_top + 400,
       std::to_string(cold_top) + " against " + std::to_string(hot_top));
}

static void the_edges_are_refused() {
    std::cout << "\nthe edges are refused:\n";

    bool threw = false;
    try { ai::sampler::argmax(std::vector<float>()); }
    catch(ai::sampler::exception&) { threw = true; }

    ok("  argmax of nothing", threw);

    ai::sampler s;

    threw = false;
    try { s.pick(std::vector<float>()); }
    catch(ai::sampler::exception&) { threw = true; }

    ok("  a choice between no tokens", threw);

    ai::sampler::config c;

    c.temperature = -1.0f;
    threw = false;
    try { ai::sampler bad(c); }
    catch(ai::sampler::exception&) { threw = true; }

    ok("  a negative temperature", threw);

    c.temperature = 1.0f;
    c.top_p = 0.0f;
    threw = false;
    try { ai::sampler bad(c); }
    catch(ai::sampler::exception&) { threw = true; }

    ok("  and a top_p of zero", threw);
}

int main() {
    std::cout << std::unitbuf;

    greedy_is_not_a_draw();
    the_same_seed_gives_the_same_tokens();
    top_k_and_top_p_cut_where_they_say();
    the_draw_follows_the_probabilities();
    temperature_moves_the_mass();
    the_edges_are_refused();

    // What a green run does not establish.
    //
    // That these are the *same* cuts llama.cpp makes.  top-k before the softmax
    // and top-p after it is the usual order and the one documented in the
    // header, but a different implementation could apply them the other way
    // round and be equally defensible -- and would produce different text from
    // the same seed.  Nothing here compares against another sampler.
    //
    // Nothing about repetition penalties, frequency penalties, mirostat or any
    // of the other knobs a full sampler carries.  What is here is temperature,
    // top-k and top-p.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
