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

#ifndef JLIB_AI_SAMPLER_HH
#define JLIB_AI_SAMPLER_HH

#include <cstdint>
#include <exception>
#include <random>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * How a sampler chooses, hoisted out of the class that uses it.
 *
 * At namespace scope rather than nested, for the same reason sys::server_policy
 * is: a default argument is a complete-class context, so `sampler(const config&
 * c = config())` cannot see a nested aggregate's member initializers while
 * sampler is still incomplete. Defining it first removes the problem instead of
 * routing around it, and `sampler::config` still spells.
 */
struct sampler_config {
    /** 0 is greedy; the draw is skipped entirely. */
    float temperature = 0.8f;

    /** How many of the highest to keep before normalising; 0 keeps all. */
    unsigned int top_k = 40;

    /** Keep the smallest set summing to this; 1 keeps all. */
    float top_p = 0.95f;

    /**
     * How much to discourage a token that has already appeared.  1 is off.
     *
     * A logit for a token already in the window is divided by this when it is
     * positive and multiplied when it is negative -- the CTRL paper's form,
     * which is what llama.cpp and transformers both implement, and which
     * moves a logit *towards* -infinity either way rather than flipping the
     * sign of a negative one.
     *
     * **Off by default, and that is a decision rather than an omission.**  A
     * penalty is a distortion of the model's distribution: it makes a token
     * less likely for having occurred rather than for anything the model
     * believes, so a prompt that legitimately repeats -- a table, a list of
     * years, code -- is what it damages first.  Llama 3.2's generation_config
     * asks for none.  Qwen 2.5's asks for 1.1, and says so in the file rather
     * than in the GGUF, which is why nothing here can read it and a caller
     * has to say.
     */
    float repetition_penalty = 1.0f;

    /**
     * How many recent tokens the penalty considers; 0 is all of them.
     *
     * llama.cpp's default is 64 and this follows it.  The window matters:
     * over a whole conversation every common word has occurred, so a
     * penalty applied to all of history penalises "the".
     */
    unsigned int penalty_window = 64;

    std::uint64_t seed = 0;
};

/**
 * Turning a column of logits into one token.
 *
 * The steps are the usual ones and the order matters: temperature, then top-k,
 * then softmax over what is left, then top-p, then a draw.
 *
 * - **temperature** divides the logits. Below one it sharpens the distribution
 *   and above one it flattens it. Zero means greedy -- take the largest and
 *   draw nothing -- which is a different code path rather than a division by
 *   zero, and is what makes a run reproducible without reference to a seed.
 * - **top-k** keeps the k highest and discards the rest before anything is
 *   normalised, so the discarded mass is redistributed rather than merely
 *   unlikely.
 * - **top-p** (nucleus) keeps the smallest set whose probabilities already sum
 *   to p. It adapts where top-k cannot: a confident position keeps one or two
 *   tokens and an uncertain one keeps many.
 *
 * top-p is applied *after* the softmax because it is defined on probabilities,
 * and top-k *before* it because it is defined on order -- which is why they are
 * not interchangeable and why both exist.
 *
 * The RNG is the sampler's own, seeded explicitly. Two samplers with the same
 * seed and the same logits give the same tokens, which is what lets a test of
 * anything downstream be a test rather than an observation.
 */
class sampler {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::ai::sampler::exception: " + msg) {}

        const char* what() const throw() { return m_msg.c_str(); }

    private:
        std::string m_msg;
    };

    typedef sampler_config config;

    explicit sampler(const config& c = config());

    const config& conf() const { return m_conf; }

    /** Start the sequence again from the configured seed. */
    void reseed(std::uint64_t seed);

    /** One token from one column of logits. */
    int pick(const std::vector<float>& logits);

    /**
     * The same, discouraging what `recent` already contains.
     *
     * `recent` is the sequence so far -- prompt and reply both, since a model
     * repeating the prompt back is the same failure.  Only the last
     * `penalty_window` of it are looked at.  With repetition_penalty at 1
     * this is exactly pick(logits), and the argument costs nothing.
     */
    int pick(const std::vector<float>& logits, const std::vector<int>& recent);

    /** The largest, with no randomness anywhere. */
    static int argmax(const std::vector<float>& logits);

    /**
     * The probabilities pick() would draw from, for a caller that wants to see
     * them -- and for a test that wants to check the shape of the
     * distribution rather than which token came out of it.
     *
     * Returns pairs of (token, probability), ordered most likely first and
     * summing to one. Empty only if the logits are.
     */
    std::vector<std::pair<int, float> > distribution(
        const std::vector<float>& logits) const;

    /**
     * The logits a penalty would leave, for a caller that wants to see it.
     *
     * Separate from distribution() so that the penalty can be looked at on
     * its own: it is the one step here that depends on history rather than on
     * this column, and it is the one most likely to be wrong in a way the
     * output does not obviously show.
     */
    std::vector<float> penalise(const std::vector<float>& logits,
                                const std::vector<int>& recent) const;

private:
    config m_conf;
    std::mt19937_64 m_gen;
};

}
}

#endif // JLIB_AI_SAMPLER_HH
