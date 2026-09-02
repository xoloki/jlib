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
#include <jlib/ai/tokenizer.hh>

#include <cstdlib>

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
 * fits every point within a few percent. The slope is the activations growing.
 *
 * **The constant is not what this said until it was measured.** It claimed the
 * cost of streaming 2.2GB of weights, and that is wrong by about seven times:
 * 2.2GB at the ~120GB/s the device reaches is 18ms, not 127. The constant is
 * **dispatch overhead**, nearly all of it the per-head attention loop -- 22
 * layers times 32 heads times about seven operations is some 4900 dispatches
 * per token at roughly 25us each.
 *
 * Measured directly: a (2048 x 64) matrix-vector product, which is what one
 * head's projection is, runs at 12.3GB/s, while the same arithmetic for all 32
 * heads in one (2048 x 2048) call runs at 118.8GB/s -- thirty-two times the
 * work for 3.3 times the time. See #163.
 *
 * So generating n tokens from a prompt of p costs about
 * `n*0.127 + 0.00033*(n*p + n*n/2)` -- quadratic in n, but with a coefficient
 * three hundred times smaller than the constant it sits beside. Measured
 * against a real 230-token run: predicted 39s, observed 45s.
 *
 * ### What a cache is worth, in numbers
 *
 * A key-value cache removes the growing term and leaves the constant exactly
 * where it is, since a pass is dispatched the same number of times either way.
 * So it is worth about **1.35x at 230 tokens and 2.3x at a thousand**, and
 * approaches nothing as the generation gets short. Observed: 1.48x on a real
 * 230-token run.
 *
 * The larger lever is therefore batching the heads, not moving fewer bytes.
 * Keeping the weights quantised was tried on the mistaken premise and is worth
 * 1.12x end to end -- real, and a tenth of what the reasoning predicted,
 * because it shrinks a nine percent slice of the cost rather than the ninety
 * percent one. It does buy 4.5x on load and 1.5x on memory.
 *
 * @param ends the ids that end the reply; see stops.  Converts from an int,
 *             so passing `tok.eos()` is the single id the file names.
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
/**
 * The ids that end a reply.
 *
 * A parameter of type `int` is a claim that there is one of them, and there
 * is not: Qwen 2.5 names `[151645, 151643]` and Llama 3.2 names
 * `[128001, 128008, 128009]`.  A GGUF cannot say so -- `tokenizer.ggml.
 * eos_token_id` is a scalar -- so the file names one and the rest live in the
 * vendor's generation_config.json, which is not in the file.
 *
 * **jlib therefore knows one and this type does not pretend otherwise.**  It
 * exists so that a caller who knows more can say so, and so that the signature
 * stops making a claim the format cannot support.  Guessing the others from
 * the vocabulary was considered and rejected: a control token is not the same
 * thing as an ending, and stopping on one a template legitimately writes
 * would truncate a reply mid-turn.
 *
 * Converts from an int, so every existing call site reads the same and passes
 * the one id it has.  A negative id is *no* id rather than a stop that can
 * never match, which is what tokenizer::eos() returns for a file without one.
 */
class stops {
public:
    stops() {}

    /** Implicit, so `generate(..., tok.eos())` still says what it did. */
    stops(int id) { add(id); }

    stops(std::initializer_list<int> ids) {
        for(int id : ids) add(id);
    }

    explicit stops(const std::vector<int>& ids) {
        for(int id : ids) add(id);
    }

    /** Ignores a negative id, and a repeat. */
    void add(int id) {
        if(id < 0 || contains(id)) return;

        m_ids.push_back(id);
    }

    bool contains(int id) const {
        for(std::size_t i = 0; i < m_ids.size(); i++)
            if(m_ids[i] == id) return true;

        return false;
    }

    bool empty() const { return m_ids.empty(); }
    std::size_t size() const { return m_ids.size(); }

    const std::vector<int>& ids() const { return m_ids; }

private:
    // A vector rather than a set: there are one or three of these and the
    // lookup happens once per token, so a linear scan over three ints beats a
    // tree, and the order stays the order a caller gave.
    std::vector<int> m_ids;
};

/**
 * A stop named on a command line, as an id.
 *
 * Takes the token's own text or a number -- `<|endoftext|>` or `151643` --
 * because a user knows a model's markers by name and not by number, and
 * looking one up is what the vocabulary is for.
 *
 * **The name wins.**  A spelling can be both: `5` is a token in every
 * byte-level vocabulary *and* a valid id, and `--stop 5` reads as the digit
 * rather than as whatever token happens to sit at index 5.  That is the
 * reading a person means, and the other one is available by id for a token
 * whose text is a number -- which is why the fallback is the number rather
 * than the name.
 *
 * @throws tokenizer::exception if it is neither, which is better than
 *         silently adding a stop that can never fire.
 */
inline int stop_id(const tokenizer& t, const std::string& spec) {
    const int named = t.id_of(spec);

    if(named >= 0) return named;

    if(!spec.empty() &&
       spec.find_first_not_of("0123456789") == std::string::npos) {
        const int id = std::atoi(spec.c_str());

        if(id >= 0 && std::size_t(id) < t.size()) return id;

        throw tokenizer::exception("stop " + spec + " is outside a vocabulary "
                                   "of " + std::to_string(t.size()));
    }

    throw tokenizer::exception("no token in this vocabulary is called \"" +
                               spec + "\", and it is not an id");
}

template<typename T>
std::vector<int> generate(model<T>& m, backend<T>& b,
                          const std::vector<int>& prompt,
                          unsigned int max_new,
                          sampler& s,
                          const stops& ends = stops(),
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

        // The whole sequence, not just what was generated: a reply that
        // parrots the prompt back is the same failure the penalty is for.
        // With repetition_penalty at 1 -- the default -- this is the same
        // call it was.
        const int next = s.pick(last_logits(logits->read()), ids);

        ids.push_back(next);

        // The cache already holds everything up to here, so the next pass owes
        // it only the token just chosen.
        if(m.caching()) feed.assign(1, next);
        else feed = ids;

        if(on_token && !on_token(next)) break;

        if(ends.contains(next)) break;
    }

    return ids;
}

}
}

#endif // JLIB_AI_GENERATE_HH
