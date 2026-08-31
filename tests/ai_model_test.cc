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

#include <jlib/ai/model.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ai = jlib::ai;

/** The SentencePiece space marker, U+2581, which every word piece begins with. */
#define SP "\xe2\x96\x81"

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static bool exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);

    return bool(f);
}

static std::string find_model(int argc, char** argv) {
    if(argc > 1 && exists(argv[1])) return argv[1];

    if(const char* env = std::getenv("JLIB_GGUF"))
        if(exists(env)) return env;

    const char* names[] = {
        "tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf"
    };

    for(const char* n : names)
        if(exists(n)) return n;

    return "";
}

/** The config a file states about itself. */
static void the_config_comes_from_the_file(const ai::gguf& g) {
    std::cout << "\nthe config comes from the file:\n";

    typedef ai::model<_Float16>::config config;

    const config c = config::from(g);

    ok("  2048 wide, 22 layers", c.d_model == 2048 && c.layers == 22);
    ok("  32 heads over 4 key-value heads",
       c.heads == 32 && c.kv_heads == 4);
    ok("  5632 of feed-forward", c.d_ff == 5632, std::to_string(c.d_ff));

    // Not a llama.* key: the vocabulary is however long the token array is.
    ok("  32000 of vocabulary", c.vocab == 32000, std::to_string(c.vocab));

    ok("  rope base 10000 and rms epsilon 1e-5",
       std::fabs(c.rope_theta - 10000.0f) < 1e-3f &&
       std::fabs(c.rms_eps - 1e-5f) < 1e-9f);
}

/**
 * The whole stack, against a real model, with an answer that is either right
 * or is not.
 *
 * "The capital of France is Paris.  The capital of Germany is Berlin.  The
 * capital of Italy is ___" -- long enough that position matters, and
 * structured so the answer depends on which of three repeated clauses is being
 * finished rather than on the last few words alone.
 *
 * Every orientation decision in load() has to be right for this to work, and
 * so does every convention that no property test could settle. Measured, with
 * this prompt:
 *
 *   as shipped              Rome 19.1, Roma 13.0, Florence 12.3
 *   rope layout split       the, a, home          -- no city at all
 *   swiglu gate and up swapped   space, comma, in
 *   grouped-query striped   lip, sterreich, etto  -- word fragments
 *
 * So this one assertion stands behind the SwiGLU gate assignment, the RoPE
 * pairing and the grouped-query mapping at once. Each of those has a comment
 * elsewhere saying a reference is the only thing that can settle it; this is
 * that reference.
 */
template<typename T>
static void it_knows_the_capital_of_italy(const char* name, ai::backend<T>& b,
                                          const ai::gguf& g)
{
    std::cout << "\nit knows the capital of italy, " << name << ":\n";

    const std::vector<std::string>& toks =
        g.get("tokenizer.ggml.tokens").strings;

    const char* pieces[] = {
        "<s>",
        SP "The", SP "capital", SP "of", SP "France", SP "is", SP "Paris", ".",
        SP "The", SP "capital", SP "of", SP "Germany", SP "is", SP "Berlin", ".",
        SP "The", SP "capital", SP "of", SP "Italy", SP "is"
    };

    std::vector<int> ids;

    for(const char* piece : pieces) {
        int found = -1;

        for(std::size_t i = 0; i < toks.size(); i++)
            if(toks[i] == piece) { found = int(i); break; }

        if(found < 0) {
            ok(std::string("  the vocabulary has ") + piece, false);

            return;
        }

        ids.push_back(found);
    }

    ok("  every piece of the prompt is in the vocabulary", true,
       std::to_string(ids.size()) + " tokens");

    typename ai::model<T>::config c = ai::model<T>::config::from(g);

    ai::model<T> m(b, c);

    m.load(g);
    m.reserve(static_cast<unsigned int>(ids.size()));

    typename ai::backend<T>::tensor_ptr logits =
        b.make(c.vocab, static_cast<unsigned int>(ids.size()));

    m.forward(ids, logits);
    b.wait();

    const jlib::math::matrix<T> l = logits->read();

    ok("  logits are vocabulary by position",
       l.M == c.vocab && l.N == ids.size(),
       std::to_string(l.M) + "x" + std::to_string(l.N));

    const unsigned int last = l.N - 1;

    int best = -1;
    int second = -1;
    double best_v = -1e30;
    double second_v = -1e30;
    bool finite = true;

    for(unsigned int v = 0; v < l.M; v++) {
        const double x = double(float(l(v, last)));

        if(!std::isfinite(x)) finite = false;

        if(x > best_v) {
            second = best; second_v = best_v;
            best = int(v); best_v = x;
        }
        else if(x > second_v) {
            second = int(v); second_v = x;
        }
    }

    ok("  every logit is finite", finite);

    ok("  and the most likely next token is Rome",
       best >= 0 && toks[std::size_t(best)] == SP "Rome",
       best >= 0 ? toks[std::size_t(best)] + " at " + std::to_string(best_v)
                 : "nothing");

    // Not a close-run thing.  Every wrong convention measured above dropped
    // this margin to nothing or produced no city at all, so requiring daylight
    // is what makes the assertion mean something rather than a coin landing.
    ok("  by a wide margin", best_v - second_v > 3.0,
       std::to_string(best_v - second_v) + " over " +
       (second >= 0 ? toks[std::size_t(second)] : "nothing"));
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const std::string path = find_model(argc, argv);

    if(path.empty()) {
        std::cout << "ai_model_test: no model file; pass one as an argument or "
                  << "set JLIB_GGUF.\n"
                  << "  curl -LO https://huggingface.co/TheBloke/"
                  << "TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/"
                  << "tinyllama-1.1b-chat-v1.0.Q8_0.gguf\n";

        return 77;
    }

    std::cout << "ai_model_test: " << path << "\n";

    try {
        const ai::gguf g(path);

        the_config_comes_from_the_file(g);

        // fp16, which halves a 1.1B-parameter model to about 2.2GB and is what
        // inference runs in anyway.  Not the host backend: its GEMM accumulates
        // in the element type, and a 2048-long dot product summed in fp16
        // would lose the answer.  MPS accumulates in fp32.
#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<_Float16> > gpu;

        try { gpu.reset(new jlib::metal::backend<_Float16>); }
        catch(std::exception& e) {
            ok("  the Metal backend comes up", false, e.what());
        }

        if(gpu) it_knows_the_capital_of_italy<_Float16>("fp16 on the GPU", *gpu, g);
#else
        std::cout << "\n  (no Metal: the forward pass needs a backend whose "
                  << "GEMM accumulates wider than fp16)\n";
#endif
    }
    catch(std::exception& e) {
        std::cerr << "ai_model_test: " << e.what() << "\n";

        return 1;
    }

    // What a green run does not establish.
    //
    // That the numbers match llama.cpp, only that the answer is right.  A small
    // error -- an epsilon in the wrong place, a rounding difference -- would
    // leave Rome on top and go unnoticed here.  Comparing logits with a
    // reference implementation is the stronger check and is not done.
    //
    // Nothing beyond one prompt of twenty tokens on one model.  Long contexts,
    // where the rope angle grows and fp16 accumulation has more to lose, are
    // untested.
    //
    // And nothing about generation: this is one forward pass over a prompt.
    // There is no sampling, no cache, and no tokenizer -- the prompt above is
    // spelled out piece by piece and looked up by exact string, which is not
    // tokenization and does not pretend to be.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
