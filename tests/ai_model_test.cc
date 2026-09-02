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

#include <jlib/ai/chat.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/model.hh>
#include <jlib/ai/tokenizer.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <cmath>
#include <random>
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

/** A named file, wherever the build happens to be run from. */
static std::string find_named(const std::string& name) {
    const std::string where[] = { "", "../", "../../", "../../../" };

    for(const std::string& w : where)
        if(exists(w + name)) return w + name;

    return "";
}

/**
 * A qwen2 file reads, and the two things it needs are load-bearing.
 *
 * `config::from` is where the architecture reaches the reader, and it decides
 * two things a llama file does not: the metadata prefix (mechanical) and the
 * **RoPE layout** (not mechanical, and not in the file at all -- ggml carries
 * it per architecture).
 *
 * The layout is the one worth an assertion of its own, because getting it
 * wrong is silent: both layouts are block-diagonal rotations with every
 * property RoPE has, so nothing downstream can notice, and Qwen answered with
 * fluent nonsense until this line was right.  See ai::rope_layout.
 */
static void a_qwen_file_reads_with_its_own_conventions() {
    std::cout << "\na qwen2 file reads with its own conventions:\n";

    const std::string path = find_named("qwen2.5-0.5b-instruct-q8_0.gguf");

    if(path.empty()) {
        std::cout << "  (no Qwen file to hand)\n";

        return;
    }

    const ai::gguf g(path);
    const ai::model<float>::config c = ai::model<float>::config::from(g);

    // Read through the qwen2.* prefix, which is the mechanical half.
    ok("  the config comes from qwen2.* keys",
       c.layers == 24 && c.d_model == 896 && c.heads == 14 &&
       c.kv_heads == 2 && c.d_ff == 4864,
       std::to_string(c.layers) + " layers, " + std::to_string(c.d_model) +
       " wide, " + std::to_string(c.heads) + " over " +
       std::to_string(c.kv_heads));

    ok("  including a rope base that is not the default",
       c.rope_theta > 900000.0f, std::to_string(c.rope_theta));

    ok("  and the layout is split, which no key in the file says",
       c.layout == ai::rope_layout::split);

    // The contrast: a llama file must still be interleaved, or this became a
    // change to every model rather than to one architecture.
    const std::string llama = find_named("tinyllama-1.1b-chat-v1.0.Q8_0.gguf");

    if(!llama.empty()) {
        const ai::gguf lg(llama);

        ok("  where a llama file is still interleaved",
           ai::model<float>::config::from(lg).layout ==
           ai::rope_layout::interleaved);
    }

    // Q, K and V biases: present here, absent on llama, and the model has to
    // pick them up from the file rather than from the architecture's name.
    ok("  the file carries Q, K and V biases",
       g.has_tensor("blk.0.attn_q.bias") &&
       g.has_tensor("blk.0.attn_k.bias") &&
       g.has_tensor("blk.0.attn_v.bias"));

    if(!llama.empty()) {
        const ai::gguf lg(llama);

        ok("  which a llama file does not", !lg.has_tensor("blk.0.attn_q.bias"));
    }
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

    // Real tokenization now, rather than the prompt spelled out piece by piece
    // and looked up by exact string.  That is what this test did before there
    // was a tokenizer, and the ids it produced by hand are the ones
    // ai_tokenizer_test still asserts encode() reproduces -- so this is the
    // same twenty tokens arrived at from the other direction.
    const ai::tokenizer tok(g);

    const std::string prompt =
        "The capital of France is Paris. The capital of Germany is Berlin. "
        "The capital of Italy is";

    const std::vector<int> ids = tok.encode(prompt);

    ok("  the prompt tokenizes to twenty tokens", ids.size() == 20,
       std::to_string(ids.size()));

    // And it says what it meant to say.  A tokenizer that produced twenty of
    // the wrong tokens would still be twenty.
    ok("  which say what the prompt said", tok.decode(ids) == prompt,
       tok.decode(ids));

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

/**
 * It writes the next sentences, not just the next token.
 *
 * Greedy, so there is nothing random to reproduce and no seed to record: the
 * output is a function of the weights alone. That is what makes an exact string
 * a fair assertion here rather than a hostage to an RNG.
 *
 * The prompt sets a pattern of three, and continuing it means both finishing
 * the third clause correctly and inventing a fourth of the same shape. Getting
 * "Rome" is one token; getting "The capital of Spain is Madrid" after it is the
 * loop, the sampler, the tokenizer and the model all agreeing over twelve
 * steps, each fed the output of the last.
 */
template<typename T>
static void it_continues_the_pattern(const char* name, ai::backend<T>& b,
                                     const ai::gguf& g)
{
    std::cout << "\nit continues the pattern, " << name << ":\n";

    const ai::tokenizer tok(g);

    typename ai::model<T>::config c = ai::model<T>::config::from(g);

    ai::model<T> m(b, c);

    m.load(g);

    const std::string prompt =
        "The capital of France is Paris. The capital of Germany is Berlin. "
        "The capital of Italy is";

    ai::sampler::config sc;
    sc.temperature = 0.0f;

    ai::sampler s(sc);

    const std::vector<int> ids = tok.encode(prompt);

    const std::vector<int> out =
        ai::generate<T>(m, b, ids, 12, s, tok.eos());

    ok("  twelve more tokens came out", out.size() == ids.size() + 12,
       std::to_string(out.size() - ids.size()));

    // Only what was generated, which is where the prompt stops.
    const std::vector<int> tail(out.begin() + long(ids.size()), out.end());

    const std::string wrote = tok.decode(tail);

    // Twelve tokens, counted from the run rather than guessed: the first
    // version of this expected the ten-token continuation and was two short.
    ok("  and they say what they should",
       wrote == "Rome. The capital of Spain is Madrid. The capital of",
       "\"" + wrote + "\"");

    // Nothing random went into that, so a second run is the same run.
    ai::sampler again(sc);

    const std::vector<int> twice =
        ai::generate<T>(m, b, ids, 12, again, tok.eos());

    ok("  greedy generation repeats exactly", twice == out);
}

/**
 * It answers a question, rather than continuing it.
 *
 * The difference is the chat layout and nothing else -- same model, same
 * weights, same greedy sampler, same question. Asked bare, TinyLlama has
 * nothing to continue and stops immediately. Asked as a conversation, it
 * replies.
 *
 * The bare case is the control, and it is a strong one: it is not that the
 * formatting improves the answer, it is that without it there is no answer at
 * all.
 */
template<typename T>
static void it_answers_a_question(const char* name, ai::backend<T>& b,
                                  const ai::gguf& g)
{
    std::cout << "\nit answers a question, " << name << ":\n";

    const ai::tokenizer tok(g);
    const ai::chat ch(g, tok.token(tok.eos()));

    typename ai::model<T>::config c = ai::model<T>::config::from(g);

    ai::model<T> m(b, c);

    m.load(g);

    const std::string question = "What is the capital of Italy?";

    ai::sampler::config sc;
    sc.temperature = 0.0f;

    // Laid out as a conversation.
    {
        ai::sampler s(sc);

        const std::vector<int> ids = tok.encode(ch.format({ { "user", question } }));
        const std::vector<int> out = ai::generate<T>(m, b, ids, 24, s, tok.eos());

        const std::string said =
            tok.decode(std::vector<int>(out.begin() + long(ids.size()), out.end()));

        ok("  it replies", !said.empty(), "\"" + said + "\"");

        ok("  and the reply says Rome",
           said.find("Rome") != std::string::npos, "\"" + said + "\"");
    }

    // The same question with no markers at all.
    {
        ai::sampler s(sc);

        const std::vector<int> ids = tok.encode(question);
        const std::vector<int> out = ai::generate<T>(m, b, ids, 24, s, tok.eos());

        const std::string said =
            tok.decode(std::vector<int>(out.begin() + long(ids.size()), out.end()));

        ok("  while asked bare it says nothing at all", said.empty(),
           "\"" + said + "\"");
    }
}

/**
 * Generation stops when the callback says to, and not before.
 *
 * On a model of zeros rather than a real one: eight wide, one layer, and every
 * weight left at its initial zero, so the logits are zero, greedy always picks
 * token 0, and what comes out depends on the loop alone. No file, so this runs
 * wherever the tests do -- which matters, because it is the mechanism an
 * interrupt reaches the generation through, and a signal is not something the
 * piped tests can send.
 */
static void generation_stops_when_asked() {
    std::cout << "\ngeneration stops when asked:\n";

    typename ai::model<float>::config c;

    c.d_model = 8;
    c.heads = 2;
    c.kv_heads = 1;
    c.d_ff = 16;
    c.layers = 1;
    c.vocab = 32;
    c.context = 64;

    ai::host_backend<float> b;
    ai::model<float> m(b, c);

    ai::sampler::config sc;
    sc.temperature = 0.0f;

    const std::vector<int> prompt{ 1, 2, 3 };

    {
        ai::sampler s(sc);

        const std::vector<int> out = ai::generate<float>(m, b, prompt, 10, s);

        ok("  with no callback it runs to the limit",
           out.size() == prompt.size() + 10,
           std::to_string(out.size() - prompt.size()));
    }

    {
        ai::sampler s(sc);
        int seen = 0;

        const std::vector<int> out = ai::generate<float>(
            m, b, prompt, 10, s, -1, [&](int) { seen++; return true; });

        ok("  and a callback that keeps saying yes changes nothing",
           out.size() == prompt.size() + 10 && seen == 10,
           std::to_string(seen));
    }

    {
        ai::sampler s(sc);
        int seen = 0;

        const std::vector<int> out = ai::generate<float>(
            m, b, prompt, 10, s, -1, [&](int) { return ++seen < 3; });

        // Three, not two: the token that prompted the stop is already made and
        // already streamed, so it belongs to the result.  A partial reply is a
        // real reply rather than something to be taken back.
        ok("  saying no ends it after that token, not before",
           out.size() == prompt.size() + 3,
           std::to_string(out.size() - prompt.size()));

        ok("  and the callback saw exactly the tokens produced", seen == 3,
           std::to_string(seen));
    }

    {
        ai::sampler s(sc);

        // Stopping at once is a whole token, not none: there is no way to
        // refuse the first, only to decline the second.
        const std::vector<int> out = ai::generate<float>(
            m, b, prompt, 10, s, -1, [&](int) { return false; });

        ok("  refusing immediately still yields one", out.size() == prompt.size() + 1,
           std::to_string(out.size() - prompt.size()));
    }
}

/** A small model with random weights, so the two paths have something to differ about. */
static void fill_randomly(ai::model<float>& m, std::mt19937& gen) {
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);

    auto rnd = [&](unsigned int rows, unsigned int cols) {
        jlib::math::matrix<float> a(rows, cols);

        for(unsigned int r = 0; r < rows; r++)
            for(unsigned int c = 0; c < cols; c++)
                a(r,c) = d(gen);

        return a;
    };

    const typename ai::model<float>::config& c = m.conf();

    m.embedding()->write(rnd(c.d_model, c.vocab));
    m.head()->write(rnd(c.d_model, c.vocab));

    jlib::math::matrix<float> ones(c.d_model, 1);

    for(unsigned int r = 0; r < c.d_model; r++) ones(r,0) = 1.0f;

    m.final_norm()->write(ones);

    const unsigned int dh = c.d_model / c.heads;

    for(unsigned int i = 0; i < c.layers; i++) {
        ai::block<float>& l = m.layer(i);

        l.attn_norm()->write(ones);
        l.ffn_norm()->write(ones);

        // One matrix each, in the file's orientation.
        l.wq()->write(rnd(c.d_model, c.heads * dh));
        l.wk()->write(rnd(c.d_model, c.kv_heads * dh));
        l.wv()->write(rnd(c.d_model, c.kv_heads * dh));
        l.wo()->write(rnd(c.heads * dh, c.d_model));

        // The file's orientation: gate and up are (d_model x d_ff), down the
        // other way, each read with multiply_tn.
        l.w_gate()->write(rnd(c.d_model, c.d_ff));
        l.w_up()->write(rnd(c.d_model, c.d_ff));
        l.w_down()->write(rnd(c.d_ff, c.d_model));
    }
}

/**
 * A cache changes what it costs and not what comes out.
 *
 * The whole correctness condition, and it is exact: greedy generation with a
 * key-value cache must produce the identical tokens to generation without one.
 * Not close, identical -- the arithmetic is the same arithmetic, reordered.
 *
 * On a small model with **random** weights rather than the zeros the other
 * tests here use, because a model of zeros produces token 0 either way and
 * would agree for reasons that have nothing to do with the cache.
 */
static void the_cache_changes_nothing() {
    std::cout << "\nthe cache changes nothing:\n";

    typename ai::model<float>::config c;

    c.d_model = 16;
    c.heads = 2;
    c.kv_heads = 1;
    c.d_ff = 32;
    c.layers = 2;
    c.vocab = 64;
    c.context = 128;

    ai::host_backend<float> b;
    ai::model<float> m(b, c);

    std::mt19937 gen(2024);

    fill_randomly(m, gen);

    ai::sampler::config sc;
    sc.temperature = 0.0f;

    const std::vector<int> prompt{ 1, 7, 13, 2 };

    ai::sampler s1(sc);

    const std::vector<int> plain = ai::generate<float>(m, b, prompt, 12, s1);

    ok("  without a cache it generates", plain.size() == prompt.size() + 12,
       std::to_string(plain.size() - prompt.size()));

    m.enable_cache();

    ok("  the cache starts empty", m.caching() && m.cached() == 0);

    ai::sampler s2(sc);

    const std::vector<int> cached = ai::generate<float>(m, b, prompt, 12, s2);

    ok("  and with one it generates the identical tokens", plain == cached);

    // Fifteen, not sixteen: four of prompt and eleven fed back.  The twelfth
    // generated token ends the loop and is never shown to the model, so its
    // keys are never computed -- counted wrong here first, which is the sort of
    // arithmetic worth writing down rather than adjusting until it passes.
    ok("  having cached every position it was shown", m.cached() == 15,
       std::to_string(m.cached()));

    // The capacity starts at what the prompt needed and doubles, so getting to
    // sixteen from four means it grew twice.  If growth lost what was in it,
    // the tokens above would not have matched.
    ai::sampler s3(sc);

    m.reset_cache();

    ok("  resetting empties it", m.cached() == 0);

    const std::vector<int> again = ai::generate<float>(m, b, prompt, 12, s3);

    ok("  and it generates the same again from a reset cache", again == plain);

    // A cache with less room than the conversation needs has to say so rather
    // than write past the end.
    typename ai::model<float>::config small = c;

    ai::model<float> tight(b, small);

    fill_randomly(tight, gen);

    tight.enable_cache(6);

    ai::sampler s4(sc);

    bool threw = false;

    try { ai::generate<float>(tight, b, prompt, 12, s4); }
    catch(std::exception&) { threw = true; }

    ok("  a cache too small for the conversation is refused", threw);
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const std::string path = find_model(argc, argv);

    // Runs with or without a model, so the generation loop is covered on a
    // machine that has neither the file nor a GPU.
    generation_stops_when_asked();
    the_cache_changes_nothing();
    a_qwen_file_reads_with_its_own_conventions();

    if(path.empty()) {
        std::cout << "\n  (no model file, so only the generation loop is "
                  << "exercised)\n"
                  << "  curl -LO https://huggingface.co/TheBloke/"
                  << "TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/"
                  << "tinyllama-1.1b-chat-v1.0.Q8_0.gguf\n";

        std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": "
                  << failures << " failure(s)\n";

        return failures ? 1 : 0;
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

        if(gpu) {
            it_knows_the_capital_of_italy<_Float16>("fp16 on the GPU", *gpu, g);
            it_continues_the_pattern<_Float16>("fp16 on the GPU", *gpu, g);
            it_answers_a_question<_Float16>("fp16 on the GPU", *gpu, g);
        }
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
    // That generation is efficient.  Every token re-runs the whole sequence,
    // because there is no key-value cache -- measured at 0.15 to 0.30s per
    // token here, dominated by streaming the weights rather than by the
    // prefix, so the quadratic term does not bite at these lengths and would
    // at a context of thousands.  See generate.hh.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
