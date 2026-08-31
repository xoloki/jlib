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
 * What that costs, measured rather than assumed, is less than the paragraph
 * above suggests. TinyLlama in fp16 on an M5, generating 49 tokens from a
 * 20-token prompt, takes 11.7s -- between 0.15 and 0.30s per token, **with no
 * growth visible** as the prefix runs from 20 to 69.
 *
 * The reason is that a pass at these lengths is not spent on the prefix. It is
 * spent streaming 2.2GB of weights through the GPU, which happens once per
 * pass whatever the sequence length is, and against which the attention work
 * for a few dozen positions does not register. So the quadratic term is real
 * and is simply not the term that matters yet; it would be at a context of
 * thousands.
 *
 * Worth saying because it bounds what a cache is worth here: it removes work
 * that currently is not the bottleneck, and the honest claim for it is about
 * long contexts rather than about this.
 *
 * @param on_token called with each new token as it is produced, for a caller
 *                 that wants to stream rather than wait
 * @return the prompt followed by everything generated
 */
template<typename T>
std::vector<int> generate(model<T>& m, backend<T>& b,
                          const std::vector<int>& prompt,
                          unsigned int max_new,
                          sampler& s,
                          int eos = -1,
                          std::function<void(int)> on_token = nullptr)
{
    if(prompt.empty())
        throw backend_error("generate: an empty prompt has no last position");

    std::vector<int> ids = prompt;

    for(unsigned int step = 0; step < max_new; step++) {
        const unsigned int n = static_cast<unsigned int>(ids.size());

        if(m.conf().context && n > m.conf().context)
            break;

        // Re-reserved every step because the sequence grew.  This reallocates
        // every intermediate in every block, which sounds worse than it is --
        // it is a few hundred allocations against a forward pass over a
        // billion parameters.  The cache branch removes the growth, not this.
        m.reserve(n);

        typename backend<T>::tensor_ptr logits = b.make(m.conf().vocab, n);

        m.forward(ids, logits);
        b.wait();

        const int next = s.pick(last_logits(logits->read()));

        ids.push_back(next);

        if(on_token) on_token(next);

        if(eos >= 0 && next == eos) break;
    }

    return ids;
}

}
}

#endif // JLIB_AI_GENERATE_HH
