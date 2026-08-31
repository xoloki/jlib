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

#ifndef JLIB_AI_GENERATE_HH
#define JLIB_AI_GENERATE_HH

#include <jlib/ai/model.hh>
#include <jlib/ai/sampler.hh>

#include <functional>
#include <vector>

namespace jlib {
namespace ai {

/**
 * The logits for the last position of a sequence, as floats.
 *
 * Pulled out because sampling is defined on one column and the model produces
 * a matrix, and because reading a (vocab x seq) matrix back to get one column
 * of it is the sort of thing worth having a name for.
 */
template<typename T>
std::vector<float> last_logits(const math::matrix<T>& logits) {
    if(logits.N == 0)
        throw backend_error("last_logits: no positions");

    std::vector<float> out(logits.M);

    for(unsigned int v = 0; v < logits.M; v++)
        out[v] = float(logits(v, logits.N - 1));

    return out;
}

/**
 * Generate tokens, one at a time, until eos or a limit.
 *
 * **The whole sequence is re-run for every token.** There is no cache here, so
 * generating n tokens from a prompt of m costs n forward passes over sequences
 * of m+1, m+2, ... m+n -- quadratic work for what should be linear. That is
 * the honest first version and it is deliberately the *reference*: a key-value
 * cache is a separate piece of work whose correctness condition is that it
 * produces exactly this, token for token, and it cannot be checked against
 * something that does not exist yet.
 *
 * What that costs was measured rather than assumed, and then measured again
 * because the first reading was too short to see anything. TinyLlama in fp16
 * on an M5, timing one forward pass at a fixed context:
 *
 *      ctx     16     64    128    256    512   1024
 *      pass  0.132  0.145  0.177  0.208  0.301  0.463  seconds
 *
 * which is **linear in the context**, not quadratic: `t = 0.127 + 0.00033n`
 * fits every point within a few percent. The constant is the cost of streaming
 * 2.2GB of weights through the GPU, which happens once per pass whatever the
 * length; the small slope is the activations growing with it.
 *
 * So generating n tokens from a prompt of p costs about
 * `n*0.127 + 0.00033*(n*p + n*n/2)` -- quadratic in n, but with a coefficient
 * three hundred times smaller than the constant it sits beside. Measured
 * against a real 230-token run: predicted 39s, observed 45s.
 *
 * ### What a cache is worth, in numbers
 *
 * A key-value cache removes the growing term and leaves the constant exactly
 * where it is, because the weights still stream once per token either way. So
 * it is worth about **1.35x at 230 tokens and 2.3x at a thousand**, and
 * approaches nothing as the generation gets short.
 *
 * Worth writing down because the larger lever is elsewhere: that 0.127s floor
 * is weight bandwidth, so holding the weights quantised and dequantising
 * inside the kernel would roughly halve it at every length, and compounds with
 * the cache rather than competing with it.
 *
 * @param on_token called with each new token as it is produced, for a caller
 *                 that wants to stream rather than wait.  **Return false to
 *                 stop**, which ends the generation after that token rather
 *                 than before it -- so what a caller has already printed is
 *                 what the result contains, and a partial reply is a real
 *                 reply rather than something to be undone.
 *
 *                 This is how an interrupt reaches the loop: a signal handler
 *                 sets a flag, the callback reads it, and generation ends
 *                 between tokens at a point where nothing is half-written. A
 *                 caller passing no callback cannot stop early, which is the
 *                 honest consequence of there being nothing to ask.
 * @return the prompt followed by everything generated
 */
template<typename T>
std::vector<int> generate(model<T>& m, backend<T>& b,
                          const std::vector<int>& prompt,
                          unsigned int max_new,
                          sampler& s,
                          int eos = -1,
                          std::function<bool(int)> on_token = nullptr)
{
    if(prompt.empty())
        throw backend_error("generate: an empty prompt has no last position");

    std::vector<int> ids = prompt;

    // With a cache the first pass takes the whole prompt and every later one a
    // single token; without, each pass takes everything so far.  The rest of
    // the loop is the same either way, which is the point -- a cache is an
    // optimisation and not a second way of generating.
    if(m.caching()) m.reset_cache();

    std::vector<int> feed = ids;

    for(unsigned int step = 0; step < max_new; step++) {
        const unsigned int n = static_cast<unsigned int>(feed.size());

        if(m.conf().context && ids.size() > m.conf().context)
            break;

        // Re-reserved every step because what is fed changes size -- from the
        // whole prompt to one token with a cache, and by one more each time
        // without.  A few hundred small allocations against a forward pass
        // over a billion parameters.
        m.reserve(n);

        typename backend<T>::tensor_ptr logits = b.make(m.conf().vocab, n);

        m.forward(feed, logits);
        b.wait();

        const int next = s.pick(last_logits(logits->read()));

        ids.push_back(next);

        // The cache already holds everything up to here, so the next pass owes
        // it only the token just chosen.
        if(m.caching()) feed.assign(1, next);
        else feed = ids;

        if(on_token && !on_token(next)) break;

        if(eos >= 0 && next == eos) break;
    }

    return ids;
}

}
}

#endif // JLIB_AI_GENERATE_HH
