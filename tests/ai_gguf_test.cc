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

#include <jlib/ai/gguf.hh>

#include "kquant_values.hh"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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

static bool exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);

    return bool(f);
}

/**
 * Where the model might be.
 *
 * Not vendored -- it is over a gigabyte -- so this looks in the places it
 * plausibly sits and reports SKIP when it is nowhere, which is how the other
 * tests here treat a missing display or audio device.
 */
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

    if(const char* s = std::getenv("srcdir")) {
        const std::string p = std::string(s) + "/../tinyllama-1.1b-chat-v1.0.Q8_0.gguf";

        if(exists(p)) return p;
    }

    return "";
}

static void the_header_and_metadata(const ai::gguf& g) {
    std::cout << "\nthe header and metadata:\n";

    ok("  version 3", g.version() == 3, std::to_string(g.version()));
    ok("  201 tensors", g.tensors().size() == 201,
       std::to_string(g.tensors().size()));
    ok("  23 metadata keys", g.metadata().size() == 23,
       std::to_string(g.metadata().size()));

    // Pinned to the value, not derived from the parser.  The tensor data in
    // this file begins at 1709440: the index ends at 1709436 and the alignment
    // is 32, so four bytes of padding follow it.  Checking it against anything
    // the parser computed would be checking it against itself -- and the first
    // version of this test did exactly that further down, where a hand-decoded
    // block was read at an offset taken from g.data_offset(), so dropping the
    // padding moved both and agreed.
    ok("  the alignment is 32", g.alignment() == 32,
       std::to_string(g.alignment()));

    ok("  and the tensor data begins at 1709440", g.data_offset() == 1709440,
       std::to_string(g.data_offset()));

    ok("  the architecture is llama", g.str("general.architecture") == "llama",
       g.str("general.architecture"));

    // Every one of these is read off the file rather than off a config.json,
    // which is the point: this is the first thing in the library that checks
    // itself against a document somebody else wrote.
    struct { const char* key; std::int64_t want; } ints[] = {
        { "llama.block_count",              22 },
        { "llama.embedding_length",       2048 },
        { "llama.feed_forward_length",    5632 },
        { "llama.attention.head_count",     32 },
        { "llama.attention.head_count_kv",   4 },
        { "llama.rope.dimension_count",     64 },
        { "llama.context_length",         2048 }
    };

    for(const auto& e : ints)
        ok(std::string("  ") + e.key, g.integer(e.key) == e.want,
           std::to_string(g.integer(e.key)));

    ok("  rope frequency base is 10000",
       std::fabs(g.real("llama.rope.freq_base") - 10000.0) < 1e-6,
       std::to_string(g.real("llama.rope.freq_base")));

    ok("  and the rms epsilon is 1e-5",
       std::fabs(g.real("llama.attention.layer_norm_rms_epsilon") - 1e-5) < 1e-9,
       std::to_string(g.real("llama.attention.layer_norm_rms_epsilon")));

    // An integer read as a real is a widening; a string read as an integer is
    // not, and is refused.
    ok("  an integer widens when read as a real",
       std::fabs(g.real("llama.block_count") - 22.0) < 1e-9);

    bool threw = false;
    try { g.integer("general.architecture"); }
    catch(ai::gguf::exception&) { threw = true; }

    ok("  while a string read as an integer is refused", threw);

    threw = false;
    try { g.str("no.such.key"); }
    catch(ai::gguf::exception&) { threw = true; }

    ok("  as is a key that is not there", threw);
}

static void the_tokenizer_arrays(const ai::gguf& g) {
    std::cout << "\nthe tokenizer arrays:\n";

    const ai::gguf::value& toks = g.get("tokenizer.ggml.tokens");

    ok("  32000 tokens", toks.strings.size() == 32000,
       std::to_string(toks.strings.size()));

    ok("  beginning unk, bos, eos",
       toks.strings.size() > 2 && toks.strings[0] == "<unk>" &&
       toks.strings[1] == "<s>" && toks.strings[2] == "</s>");

    // 61249 of them, each variable length.  This is the assertion that says
    // the array walk did not seek past anything it could not seek past.
    ok("  61249 merges", g.get("tokenizer.ggml.merges").strings.size() == 61249,
       std::to_string(g.get("tokenizer.ggml.merges").strings.size()));

    ok("  a score per token",
       g.get("tokenizer.ggml.scores").numbers.size() == 32000,
       std::to_string(g.get("tokenizer.ggml.scores").numbers.size()));

    ok("  and a type per token",
       g.get("tokenizer.ggml.token_type").numbers.size() == 32000,
       std::to_string(g.get("tokenizer.ggml.token_type").numbers.size()));

    ok("  bos is 1 and eos is 2",
       g.integer("tokenizer.ggml.bos_token_id") == 1 &&
       g.integer("tokenizer.ggml.eos_token_id") == 2);
}

/**
 * The index has to agree with the metadata.
 *
 * Two independent parts of the same file, written by a tool that had no reason
 * to make them agree unless they do.  This is cross-validation rather than
 * self-consistency, which is what nothing on this path has had until now.
 */
static void the_index_agrees_with_the_metadata(const ai::gguf& g) {
    std::cout << "\nthe index agrees with the metadata:\n";

    const std::int64_t blocks = g.integer("llama.block_count");
    const std::int64_t embd = g.integer("llama.embedding_length");
    const std::int64_t ff = g.integer("llama.feed_forward_length");
    const std::int64_t heads = g.integer("llama.attention.head_count");
    const std::int64_t kv = g.integer("llama.attention.head_count_kv");

    const char* per_block[] = {
        "attn_norm.weight", "attn_q.weight", "attn_k.weight", "attn_v.weight",
        "attn_output.weight", "ffn_norm.weight", "ffn_gate.weight",
        "ffn_up.weight", "ffn_down.weight"
    };

    bool all_there = true;

    for(std::int64_t b = 0; b < blocks; b++)
        for(const char* n : per_block)
            if(!g.has_tensor("blk." + std::to_string(b) + "." + n))
                all_there = false;

    ok("  every block has all nine of its tensors", all_there);

    // 22 blocks of 9, plus the embedding, the final norm and the head.
    ok("  which accounts for every tensor in the file",
       g.tensors().size() == std::size_t(blocks * 9 + 3),
       std::to_string(g.tensors().size()) + " vs " +
       std::to_string(blocks * 9 + 3));

    const ai::gguf::tensor_info& q = g.tensor("blk.0.attn_q.weight");
    const ai::gguf::tensor_info& k = g.tensor("blk.0.attn_k.weight");

    ok("  attn_q is embedding by embedding",
       q.shape.size() == 2 && q.shape[0] == std::uint64_t(embd) &&
       q.shape[1] == std::uint64_t(embd));

    // The one that shows this model is grouped-query: k is narrower than q by
    // exactly the ratio of key-value heads to attention heads.
    ok("  attn_k is narrower by the head-count ratio",
       k.shape.size() == 2 && k.shape[0] == std::uint64_t(embd) &&
       k.shape[1] == std::uint64_t(embd * kv / heads),
       std::to_string(k.shape[1]) + " vs " + std::to_string(embd * kv / heads));

    const ai::gguf::tensor_info& gate = g.tensor("blk.0.ffn_gate.weight");

    ok("  ffn_gate is embedding by feed-forward",
       gate.shape.size() == 2 && gate.shape[0] == std::uint64_t(embd) &&
       gate.shape[1] == std::uint64_t(ff));

    // And the vocabulary appears twice, in two unrelated places.
    const ai::gguf::tensor_info& out = g.tensor("output.weight");

    ok("  the head is as wide as the token array is long",
       out.shape.size() == 2 &&
       out.shape[1] == g.get("tokenizer.ggml.tokens").strings.size(),
       std::to_string(out.shape[1]));

    ok("  the norms are unquantised and the matrices are not",
       g.tensor("blk.0.attn_norm.weight").type == ai::gguf::tensor_type::f32 &&
       q.type == ai::gguf::tensor_type::q8_0);
}

/** Dequantisation, checked against the bytes rather than against itself. */
static void the_weights_come_back(const ai::gguf& g, const std::string& path) {
    std::cout << "\nthe weights come back:\n";

    const jlib::math::matrix<float> norm = g.read("blk.0.attn_norm.weight");

    ok("  a norm weight is one column of embedding_length",
       norm.M == 2048 && norm.N == 1,
       std::to_string(norm.M) + "x" + std::to_string(norm.N));

    bool finite = true;
    double biggest = 0;

    for(unsigned int r = 0; r < norm.M; r++) {
        if(!std::isfinite(norm(r,0))) finite = false;

        biggest = std::max(biggest, std::fabs(double(norm(r,0))));
    }

    ok("  every value of it is finite", finite);

    // A range check, and specifically not a check that the mean is near one.
    // That was the first version of this assertion and it failed: this file's
    // blk.0.attn_norm.weight has mean 0.0058, running from -0.58 to 0.77, and
    // an independent read of the raw bytes agrees.  RMS norm scales are simply
    // not near one -- across this model they run from 0.006 at block 0 to 1.91
    // at output_norm -- so "near one" was a folk belief about what weights look
    // like rather than anything the format or the model promises.
    //
    // What is left is worth having anyway.  A float misread with its bytes in
    // the wrong order lands in the exponent, so it comes back astronomical or
    // denormal; requiring every value inside a sane band and at least one of
    // them not vanishingly small is the canary for that, and for a stride
    // mistake that reads into the middle of neighbouring values.
    ok("  and they sit in a range floats read the wrong way round would not",
       biggest > 0.1 && biggest < 100.0, std::to_string(biggest));

    // The same canary where the scales are large, so the band is exercised
    // from both ends rather than only near zero.
    const jlib::math::matrix<float> tail = g.read("output_norm.weight");

    double tail_mean = 0;

    for(unsigned int r = 0; r < tail.M; r++) tail_mean += tail(r,0);

    tail_mean /= double(tail.M);

    ok("  while the final norm's scales are of order one", 
       tail_mean > 0.5 && tail_mean < 5.0, std::to_string(tail_mean));

    const ai::gguf::tensor_info& info = g.tensor("blk.0.attn_q.weight");
    const jlib::math::matrix<float> q = g.read(info);

    ok("  a q8_0 weight comes back at its stated shape",
       q.M == info.shape[0] && q.N == info.shape[1],
       std::to_string(q.M) + "x" + std::to_string(q.N));

    // Independently: open the file again, seek past the header by the offset
    // asserted above, and decode blocks by hand.  q8_0 is one fp16 scale then
    // thirty-two signed bytes, so every value in a block is an integer multiple
    // of one scale -- a structure a wrong stride cannot reproduce.
    //
    // Blocks 0, 1 and 1000, not block 0 alone.  Block 0 begins at the start of
    // the tensor whatever the stride is believed to be, so checking only it
    // says nothing about the block size: the first version of this test passed
    // with the block declared 36 bytes wide.
    std::ifstream raw(path, std::ios::binary);

    const std::uint64_t DATA = 1709440;

    double furthest = 0;
    bool integral = true;

    for(std::uint64_t b : { std::uint64_t(0), std::uint64_t(1), std::uint64_t(1000) }) {
        raw.seekg(std::streamoff(DATA + info.offset + b * 34));

        char block[34];

        raw.read(block, 34);

        _Float16 d;

        std::memcpy(&d, block, sizeof(d));

        const signed char* qs = reinterpret_cast<const signed char*>(block + 2);

        for(int i = 0; i < 32; i++) {
            const std::uint64_t at = b * 32 + i;

            // Column-major, so the flat index walks down a column first.
            const float got = q(static_cast<unsigned int>(at % q.M),
                                static_cast<unsigned int>(at / q.M));

            furthest = std::max(furthest, std::fabs(double(float(d) * float(qs[i]) - got)));

            if(float(d) != 0.0f &&
               std::fabs(double(got / float(d)) - std::round(double(got / float(d)))) > 1e-3)
                integral = false;
        }
    }

    ok("  matching blocks 0, 1 and 1000 decoded by hand from the file",
       furthest == 0.0, std::to_string(furthest));

    ok("  with every value an integer multiple of its block's scale", integral);

    bool threw = false;
    try { g.read("no.such.tensor"); }
    catch(ai::gguf::exception&) { threw = true; }

    ok("  and a tensor that is not there is refused", threw);
}

/**
 * K-quantised tensors dequantise to what the reference says.
 *
 * q4_K and q6_K are two-tier: 256 weights sharing one f16 super-block scale,
 * with *sub-block* scales quantised against it.  q4_K additionally carries
 * quantised **mins**, so its reconstruction is affine (`d*q - m`) rather than
 * symmetric -- the part with no analogue in q8_0.
 *
 * Exact equality, deliberately.  Dequantisation is integer arithmetic and two
 * f16 multiplies; a correct implementation reproduces the reference bit for
 * bit, and a tolerance would hide a bit-packing error that happens to land
 * close.
 */
static std::string find_named(const std::string& name) {
    const std::string where[] = { "", "../", "../../", "../../../" };

    for(const std::string& w : where) {
        std::ifstream f(w + name, std::ios::binary);

        if(f) return w + name;
    }

    return "";
}

static void kquants_match_the_reference(const std::string& path) {
    std::cout << "\nK-quantised tensors match the reference:\n";

    const ai::gguf g(path);

    for(const kquant_case& c : KQUANT) {
        if(!g.has_tensor(c.tensor)) {
            ok(std::string("  ") + c.tensor + " is present", false);

            continue;
        }

        const jlib::math::matrix<float> m = g.read(c.tensor);

        // Column-major, dims[0] contiguous -- so this walks the order the
        // file stores, which is the order the reference ravels.
        bool same = true;
        float worst = 0;

        for(int i = 0; i < c.count; i++) {
            const float got = m(static_cast<unsigned>(i), 0);
            const float d = std::fabs(got - c.first[i]);

            if(d > worst) worst = d;
            if(got != c.first[i]) same = false;
        }

        ok(std::string("  ") + c.tensor + " (type " + c.type + ")", same,
           same ? "" : "max difference " + std::to_string(worst));
    }
}

int main(int argc, char** argv) {
    {
        // Its own file: the K-quants are a mixture and the model the rest of
        // this test uses is q8_0 throughout, so there is nothing to read.
        const std::string kq = find_named("gemma-2-2b-it-Q4_K_M.gguf");

        if(kq.empty())
            std::cout << "\n(no K-quantised file; q4_K and q6_K unchecked)\n";
        else
            kquants_match_the_reference(kq);
    }

    std::cout << std::unitbuf;

    const std::string path = find_model(argc, argv);

    if(path.empty()) {
        std::cout << "ai_gguf_test: no model file; pass one as an argument or "
                  << "set JLIB_GGUF.\n"
                  << "  curl -LO https://huggingface.co/TheBloke/"
                  << "TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/"
                  << "tinyllama-1.1b-chat-v1.0.Q8_0.gguf\n";

        return 77;
    }

    std::cout << "ai_gguf_test: " << path << "\n";

    try {
        const ai::gguf g(path);

        the_header_and_metadata(g);
        the_tokenizer_arrays(g);
        the_index_agrees_with_the_metadata(g);
        the_weights_come_back(g, path);
    }
    catch(std::exception& e) {
        std::cerr << "ai_gguf_test: " << e.what() << "\n";

        return 1;
    }

    // What a green run does not establish.
    //
    // That the weights are *right*, only that they are the bytes the file
    // holds, decoded the way the format says.  Nothing here runs the model, so
    // a systematic error that preserved the block structure -- a transposed
    // matrix, say -- would pass everything above.  That is settled by
    // generating text and comparing it with llama.cpp, which needs the rest of
    // the stack.
    //
    // Only one file, and only the types in it: f32 and q8_0.  f16 is
    // implemented and untested, because this model has none.  Every k-quant is
    // refused by name, which is a different thing from being handled.
    //
    // And nothing about big-endian, which the format permits and this reads
    // wrongly by construction -- though the range check on the norm weights
    // would notice, since a byte-swapped float lands in its exponent.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
