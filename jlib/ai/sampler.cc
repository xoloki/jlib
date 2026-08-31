/* -*- mode: C++ c-basic-offset: 4  -*-
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

#include <algorithm>
#include <cmath>

namespace jlib {
namespace ai {

sampler::sampler(const config& c)
    : m_conf(c),
      m_gen(c.seed)
{
    if(c.temperature < 0)
        throw exception("a negative temperature");

    if(c.top_p <= 0 || c.top_p > 1)
        throw exception("top_p must be above zero and at most one");
}

void sampler::reseed(std::uint64_t seed) {
    m_conf.seed = seed;
    m_gen.seed(seed);
}

int sampler::argmax(const std::vector<float>& logits) {
    if(logits.empty())
        throw exception("argmax of nothing");

    int best = 0;

    for(std::size_t i = 1; i < logits.size(); i++)
        if(logits[i] > logits[std::size_t(best)]) best = int(i);

    return best;
}

std::vector<std::pair<int, float> >
sampler::distribution(const std::vector<float>& logits) const {
    std::vector<std::pair<int, float> > kept;

    if(logits.empty()) return kept;

    kept.reserve(logits.size());

    for(std::size_t i = 0; i < logits.size(); i++)
        kept.push_back(std::make_pair(int(i), logits[i]));

    // Descending, breaking ties by token so the result does not depend on the
    // sort being stable or on the order the vocabulary happens to be in.
    std::sort(kept.begin(), kept.end(),
              [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                  return a.second > b.second ||
                         (a.second == b.second && a.first < b.first);
              });

    // top-k first, on the order alone.  Cutting here rather than after the
    // softmax is what makes the discarded mass go back to the survivors.
    if(m_conf.top_k > 0 && kept.size() > m_conf.top_k)
        kept.resize(m_conf.top_k);

    const float t = m_conf.temperature;

    // Softmax, with the maximum subtracted -- which is free here, the list
    // being sorted, and is the same guard against exp() overflowing that
    // backend::softmax carries.
    const float top = kept[0].second;

    double sum = 0;

    for(std::size_t i = 0; i < kept.size(); i++) {
        const double e = std::exp(double(kept[i].second - top) /
                                  double(t > 0 ? t : 1.0f));

        kept[i].second = float(e);
        sum += e;
    }

    for(std::size_t i = 0; i < kept.size(); i++)
        kept[i].second = float(double(kept[i].second) / sum);

    // And top-p, which needs the probabilities that only now exist.  At least
    // one survives however small p is: a nucleus of nothing is not a choice.
    if(m_conf.top_p < 1.0f) {
        double running = 0;
        std::size_t keep = 0;

        for(; keep < kept.size(); keep++) {
            running += double(kept[keep].second);

            if(running >= double(m_conf.top_p)) { keep++; break; }
        }

        // No floor needed, and there was one here until a mutation showed it
        // could never fire: kept is non-empty by now, so the loop runs at least
        // once and leaves keep at 1 or more either way it exits.  A guard that
        // cannot trigger reads as though the case it names is possible.
        kept.resize(keep);

        double again = 0;

        for(std::size_t i = 0; i < kept.size(); i++) again += double(kept[i].second);

        for(std::size_t i = 0; i < kept.size(); i++)
            kept[i].second = float(double(kept[i].second) / again);
    }

    return kept;
}

int sampler::pick(const std::vector<float>& logits) {
    if(logits.empty())
        throw exception("a choice between no tokens");

    // Greedy is not temperature zero taken to a limit, it is the absence of a
    // draw -- so the generator does not consume randomness and two runs at
    // temperature zero agree whatever the seeds were.
    if(m_conf.temperature <= 0) return argmax(logits);

    const std::vector<std::pair<int, float> > d = distribution(logits);

    std::uniform_real_distribution<double> u(0.0, 1.0);

    const double r = u(m_gen);

    double running = 0;

    for(std::size_t i = 0; i < d.size(); i++) {
        running += double(d[i].second);

        if(r <= running) return d[i].first;
    }

    // Only reachable if the probabilities sum to slightly under one, which
    // rounding can manage.  The last is the honest answer, not an error.
    return d.back().first;
}

}
}
