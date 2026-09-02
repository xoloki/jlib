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

#ifndef JLIB_AI_MODEL_HH
#define JLIB_AI_MODEL_HH

#include <jlib/ai/gguf.hh>

#include <cmath>
#include <jlib/ai/transformer.hh>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * A Llama-architecture language model: embed, a stack of blocks, norm, project.
 *
 *   x = embedding[:, ids]
 *   x = block_i(x)                 for each of layers
 *   x = rms_norm(x, final_norm)
 *   logits = head^T x              (vocab x positions)
 *
 * Nothing here is new arithmetic -- every step is a primitive or a block. What
 * is new is that the weights come from a file somebody else wrote, which is
 * what makes this the first thing in the library that can be *wrong* in a way
 * the tests could not have told it was.
 */
template<typename T>
class model {
public:
    typedef typename backend<T>::tensor_ptr tensor_ptr;
    typedef typename backend<T>::quantised_ptr quantised_ptr;

    /** What a file says about its own shape. */
    struct config {
        unsigned int d_model = 0;
        unsigned int heads = 0;
        unsigned int kv_heads = 0;
        unsigned int d_ff = 0;
        unsigned int layers = 0;
        unsigned int vocab = 0;
        unsigned int context = 0;

        /**
         * The width of one head, or 0 to derive it as d_model / heads.
         *
         * An independent quantity that llama and qwen2 files happen to make
         * derivable.  Gemma 2 states it and disagrees: 2304/8 is 288 and
         * attention.key_length is 256.
         */
        unsigned int d_head = 0;

        /**
         * What to multiply the embedding by after the lookup, or 0 for none.
         *
         * Gemma 2 scales by sqrt(d_model) -- 48 for its 2304 -- and no key in
         * the file says so; it is a property of the architecture, like the
         * RoPE layout.  Llama and qwen2 do not.
         */
        float embed_scale = 0;


        /** SwiGLU or GeGLU; see block::set_gate_activation. */
        activation gate = activation::silu;

        /** Caps on the attention and output logits; 0 is none. */
        float attn_cap = 0;
        float final_cap = 0;

        float rope_theta = 10000.0f;
        float rms_eps = 1e-5f;

        rope_layout layout = rope_layout::interleaved;

        /**
         * Read the llama.* keys.
         *
         * head_count_kv is optional in the format and absent on models from
         * before grouped-query attention existed; absent means every query
         * head has its own, which is what head_count says.
         */
        static config from(const gguf& g);
    };

    model(backend<T>& b, const config& c);

    const config& conf() const { return m_conf; }

    block<T>& layer(unsigned int i) { return *m_layers[i]; }

    /** (d_model x vocab): one column per token, which is how gather wants it. */
    tensor_ptr& embedding() { return m_embed; }

    /** (d_model x 1). */
    tensor_ptr& final_norm() { return m_final_norm; }

    /** (d_model x vocab), read through multiply_tn -- see the note on load(). */
    tensor_ptr& head() { return m_head; }

    /**
     * The head kept in the encoding the file used.
     *
     * Free to quantise: it is already in the file's orientation and already
     * read with multiply_tn, so nothing about its shape or its use changes.
     */
    void set_head(const quantised_ptr& q) { m_head_q = q; m_head.reset(); }

    /**
     * Place every weight in the file.
     *
     * ### Orientation
     *
     * There is nothing to do. A GGUF weight arrives as `[n_in, n_out]` and the
     * product a layer wants is `W^T x`, so every weight is used in the
     * orientation it came in and this function is a copy.
     *
     * It was not always. The block used to hold each attention weight sliced
     * per head and transposed, and this function did that work -- which cost a
     * transpose of a gigabyte at load and, worse, meant a weight could not stay
     * in the quantisation it arrived in, since q8_0 blocks run along the
     * contiguous dimension and do not survive being turned. Both went when the
     * per-head loop did.
     *
     * @throws gguf::exception or backend_error if a tensor is missing or the
     *         wrong shape for what the config said
     */
    void load(const gguf& g);

    /**
     * Keep every block's keys and values, so a later call supplies only what
     * is new.  See block::enable_cache.
     *
     * Defaults to the context the file declared, which is the most a model can
     * be asked to hold anyway.
     */
    void enable_cache(unsigned int context = 0);

    /** Forget it, for a conversation that starts again. */
    void reset_cache();

    bool caching() const { return !m_layers.empty() && m_layers[0]->caching(); }

    /** How many positions are cached. */
    unsigned int cached() const {
        return m_layers.empty() ? 0 : m_layers[0]->cached();
    }

    /** Size every intermediate for a sequence of this length. */
    void reserve(unsigned int seq);

    /**
     * ids -> logits, which come back (vocab x positions).
     *
     * One column of logits per input position, not just for the last one. A
     * generator wants only the last; a perplexity measurement wants them all,
     * and dropping the rest here would be deciding that for both.
     */
    void forward(const std::vector<int>& ids, tensor_ptr& logits,
                 unsigned int base_pos = 0);

private:
    /** Straight across, narrowing to T. */
    static math::matrix<T> narrowed(const math::matrix<float>& w);


    void expect(const gguf& g, const std::string& name,
                unsigned int d0, unsigned int d1) const;

    backend<T>& m_b;
    config m_conf;

    std::vector<std::shared_ptr<block<T> > > m_layers;

    tensor_ptr m_embed, m_final_norm, m_head;
    quantised_ptr m_head_q;
    tensor_ptr m_x, m_y;

    unsigned int m_seq = 0;
};

template<typename T>
typename model<T>::config model<T>::config::from(const gguf& g) {
    config c;

    const std::string arch = g.str("general.architecture");

    // Which architectures this reads, and the list is the *whole* claim: the
    // metadata keys are prefixed with the architecture's own name, so reading
    // a file's keys is a matter of spelling the prefix -- and being able to
    // spell it is not the same as supporting the model.  Anything not named
    // here is refused even though its keys would read perfectly well, because
    // the shapes and the sublayers are what decide, not the spelling.
    //
    // llama and qwen2 differ in exactly one thing that reaches this far: Qwen
    // carries biases on Q, K and V, which load() picks up when they are
    // present.  Everything else about the block is the same tensor set.
    if(arch != "llama" && arch != "qwen2" && arch != "gemma2")
        throw backend_error("model: this reads llama-, qwen2- and "
                            "gemma2-architecture files, and that one says '" +
                            arch + "'");

    const std::string a = arch + ".";

    c.d_model = static_cast<unsigned int>(g.integer(a + "embedding_length"));
    c.heads   = static_cast<unsigned int>(g.integer(a + "attention.head_count"));
    c.d_ff    = static_cast<unsigned int>(g.integer(a + "feed_forward_length"));
    c.layers  = static_cast<unsigned int>(g.integer(a + "block_count"));
    c.context = static_cast<unsigned int>(g.integer(a + "context_length"));

    c.kv_heads = g.has(a + "attention.head_count_kv")
        ? static_cast<unsigned int>(g.integer(a + "attention.head_count_kv"))
        : c.heads;

    if(g.has(a + "rope.freq_base"))
        c.rope_theta = static_cast<float>(g.real(a + "rope.freq_base"));

    if(g.has(a + "attention.layer_norm_rms_epsilon"))
        c.rms_eps = static_cast<float>(
            g.real(a + "attention.layer_norm_rms_epsilon"));

    // **Which RoPE layout, and it is not in the file.**  ggml carries it as a
    // per-architecture constant: llama is the normal type and qwen2 is the
    // NeoX one, and a GGUF says neither.  The two differ only in which
    // coordinates share a rotation block, so every property RoPE has holds
    // for both and nothing here can detect the wrong choice -- it comes out
    // as fluent nonsense, which is exactly what Qwen produced before this
    // line.  See ai::rope_layout, which says the same thing at more length.
    // qwen2 and gemma2 are both the NeoX layout; llama is the normal one.
    // Not in any file -- ggml carries it per architecture.  See
    // ai::rope_layout, and note that getting it wrong is silent.
    if(arch == "qwen2" || arch == "gemma2") c.layout = rope_layout::split;

    // Gemma 2's two unwritten conventions.  Neither is in the file and both
    // are load-bearing: without the scale the residual stream starts 48x too
    // small, and without the offset every norm weight is near zero and scales
    // the activations to nothing.
    // Gemma 2's conventions, none of which are in the file.
    //
    // **Not** the `1 + w` norm weights, and that is worth writing down
    // because Gemma's published implementation does exactly that and copying
    // it from there is wrong here: the GGUF conversion has already applied
    // it. Measured on gemma-2-2b-it-Q8_0, attn_norm runs [0.000, 5.969] with
    // mean 1.19 -- non-negative, centred near one -- where TinyLlama's raw
    // weights run [-0.582, 0.770] with mean 0.006. Adding one again put
    // every norm out by a factor and the model answered "1" to everything.
    //
    // The lesson generalises: the HF checkpoint and the GGUF are different
    // artefacts, and a convention read out of modeling_gemma2.py describes
    // the former.
    if(arch == "gemma2") {
        c.embed_scale = std::sqrt(float(c.d_model));
        c.gate = activation::gelu;
    }

    // The caps, which *are* in the file.
    if(g.has(a + "attn_logit_softcapping"))
        c.attn_cap = static_cast<float>(g.real(a + "attn_logit_softcapping"));

    if(g.has(a + "final_logit_softcapping"))
        c.final_cap = static_cast<float>(g.real(a + "final_logit_softcapping"));

    // The head width, when the file states it.  key_length and value_length
    // are separate keys and this reads one, because nothing here can hold a
    // model whose keys and values are different widths -- so they are checked
    // to agree rather than quietly taking the first.
    if(g.has(a + "attention.key_length")) {
        c.d_head = static_cast<unsigned int>(g.integer(a + "attention.key_length"));

        if(g.has(a + "attention.value_length") &&
           g.integer(a + "attention.value_length") != c.d_head)
            throw backend_error("model: this file's keys and values are "
                                "different widths, which nothing here holds");
    }

    // The vocabulary is not a llama.* key.  It is the length of the token
    // array, and the head's width has to agree with it -- which load() checks.
    c.vocab = static_cast<unsigned int>(
        g.get("tokenizer.ggml.tokens").strings.size());

    return c;
}

template<typename T>
model<T>::model(backend<T>& b, const config& c)
    : m_b(b),
      m_conf(c)
{
    if(!c.d_model || !c.heads || !c.kv_heads || !c.d_ff || !c.layers || !c.vocab)
        throw backend_error("model: a config with a zero in it");

    for(unsigned int i = 0; i < c.layers; i++) {
        std::shared_ptr<block<T> > l(
            new block<T>(b, c.d_model, c.heads, c.kv_heads, c.d_ff,
                         c.d_head));

        l->set_eps(c.rms_eps);
        l->set_gate_activation(c.gate);
        l->set_attention_cap(c.attn_cap);
        l->set_rope(true, c.rope_theta, c.layout);

        m_layers.push_back(l);
    }

    m_embed = b.make(c.d_model, c.vocab);
    m_final_norm = b.make(c.d_model, 1);
    m_head = b.make(c.d_model, c.vocab);
}

template<typename T>
math::matrix<T> model<T>::narrowed(const math::matrix<float>& w) {
    math::matrix<T> out(w.M, w.N);

    for(unsigned int r = 0; r < w.M; r++)
        for(unsigned int c = 0; c < w.N; c++)
            out(r, c) = T(w(r, c));

    return out;
}

template<typename T>
void model<T>::expect(const gguf& g, const std::string& name,
                      unsigned int d0, unsigned int d1) const
{
    const gguf::tensor_info& t = g.tensor(name);

    const unsigned int got0 = t.shape.size() > 0
        ? static_cast<unsigned int>(t.shape[0]) : 0;
    const unsigned int got1 = t.shape.size() > 1
        ? static_cast<unsigned int>(t.shape[1]) : 1;

    if(got0 != d0 || got1 != d1) {
        std::ostringstream e;

        e << "model: " << name << " is " << got0 << "x" << got1
          << " where the metadata implies " << d0 << "x" << d1;

        throw backend_error(e.str());
    }
}

template<typename T>
void model<T>::load(const gguf& g) {
    const unsigned int d = m_conf.d_model;

    // The same width the blocks were built with, and for the same reason:
    // deriving it here would make every expect() below check a shape the
    // file does not have.
    const unsigned int dh = m_conf.d_head ? m_conf.d_head : d / m_conf.heads;

    // No transposition for either of these.  The embedding is a table of
    // columns and gather reads columns; the head is used through multiply_tn,
    // which wants exactly the file's orientation.
    expect(g, "token_embd.weight", d, m_conf.vocab);
    m_embed->write(narrowed(g.read("token_embd.weight")));

    // Tied embeddings.  Llama 3.2 and others ship no output.weight at all:
    // the projection back to the vocabulary *is* the embedding table, reused.
    // Both are [d_model, vocab] as the file has them, and the head is used
    // through multiply_tn which wants that orientation, so the same bytes
    // serve both ends with nothing rearranged.
    const std::string head = g.has_tensor("output.weight") ? "output.weight"
                                                           : "token_embd.weight";

    expect(g, head, d, m_conf.vocab);

    // Kept quantised where the file quantised it.  These are the tensors whose
    // blocks run along the dimension they are used on, so nothing has to be
    // rearranged and the file's bytes go to the device unchanged.
    if(g.tensor(head).type == gguf::tensor_type::q8_0) {
        const std::vector<char> raw = g.read_raw(head);

        set_head(m_b.make_q8_0(d, m_conf.vocab, raw.data(), raw.size()));
    }
    else
        m_head->write(narrowed(g.read(head)));

    expect(g, "output_norm.weight", d, 1);
    m_final_norm->write(narrowed(g.read("output_norm.weight")));

    for(unsigned int i = 0; i < m_conf.layers; i++) {
        const std::string p = "blk." + std::to_string(i) + ".";

        block<T>& l = *m_layers[i];

        expect(g, p + "attn_norm.weight", d, 1);
        l.attn_norm()->write(narrowed(g.read(p + "attn_norm.weight")));

        expect(g, p + "ffn_norm.weight", d, 1);
        l.ffn_norm()->write(narrowed(g.read(p + "ffn_norm.weight")));

        // Gemma's post-sublayer norms, from the file rather than the
        // architecture's name -- absent everywhere else, and the block costs
        // nothing for them when they are.
        struct { const char* name; tensor_ptr& (block<T>::*get)(); } post[] = {
            { "post_attention_norm.weight", &block<T>::post_attn_norm },
            { "post_ffw_norm.weight",       &block<T>::post_ffn_norm }
        };

        for(const auto& e : post) {
            if(!g.has_tensor(p + e.name)) continue;

            expect(g, p + e.name, d, 1);

            (l.*e.get)()->write(narrowed(g.read(p + e.name)));
        }

        // Every one of these is a whole matrix in the file's orientation now
        // -- no slice, no transpose, so all four can stay in the quantisation
        // they arrived in. The row slicing that kept attn_output out of that
        // went with the per-head loop that needed it.
        struct { const char* name; unsigned int rows; unsigned int cols;
                 void (block<T>::*set)(const quantised_ptr&);
                 tensor_ptr& (block<T>::*get)(); } attn[] = {
            { "attn_q.weight",      d, m_conf.heads * dh,    &block<T>::set_wq, &block<T>::wq },
            { "attn_k.weight",      d, m_conf.kv_heads * dh, &block<T>::set_wk, &block<T>::wk },
            { "attn_v.weight",      d, m_conf.kv_heads * dh, &block<T>::set_wv, &block<T>::wv },
            { "attn_output.weight", m_conf.heads * dh, d,    &block<T>::set_wo, &block<T>::wo }
        };

        for(const auto& e : attn) {
            expect(g, p + e.name, e.rows, e.cols);

            if(g.tensor(p + e.name).type == gguf::tensor_type::q8_0) {
                const std::vector<char> raw = g.read_raw(p + e.name);

                (l.*e.set)(m_b.make_q8_0(e.rows, e.cols, raw.data(), raw.size()));
            }
            else
                (l.*e.get)()->write(narrowed(g.read(p + e.name)));
        }

        // Present on qwen2, absent on llama, and optional here rather than
        // architecture-gated: the file is what says, and a llama file that
        // grew biases would work without this having to learn its name.
        //
        // Never quantised -- these are one vector each and every file stores
        // them in float -- so there is no q8_0 branch to mirror.
        struct { const char* name; unsigned int rows;
                 tensor_ptr& (block<T>::*get)(); } bias[] = {
            { "attn_q.bias", m_conf.heads * dh,    &block<T>::bq },
            { "attn_k.bias", m_conf.kv_heads * dh, &block<T>::bk },
            { "attn_v.bias", m_conf.kv_heads * dh, &block<T>::bv }
        };

        for(const auto& e : bias) {
            if(!g.has_tensor(p + e.name)) continue;

            expect(g, p + e.name, e.rows, 1);

            (l.*e.get)()->write(narrowed(g.read(p + e.name)));
        }

        expect(g, p + "ffn_gate.weight", d, m_conf.d_ff);
        expect(g, p + "ffn_up.weight", d, m_conf.d_ff);
        expect(g, p + "ffn_down.weight", m_conf.d_ff, d);

        // Straight across in the file's orientation -- no transpose, so the
        // quantisation blocks still run along the dimension they were built
        // for, and the bytes can go to the device as they are.
        struct { const char* name; unsigned int rows; unsigned int cols;
                 void (block<T>::*set)(const quantised_ptr&);
                 tensor_ptr& (block<T>::*get)(); } ffn[] = {
            { "ffn_gate.weight", d, m_conf.d_ff, &block<T>::set_gate, &block<T>::w_gate },
            { "ffn_up.weight",   d, m_conf.d_ff, &block<T>::set_up,   &block<T>::w_up },
            { "ffn_down.weight", m_conf.d_ff, d, &block<T>::set_down, &block<T>::w_down }
        };

        for(const auto& e : ffn) {
            if(g.tensor(p + e.name).type == gguf::tensor_type::q8_0) {
                const std::vector<char> raw = g.read_raw(p + e.name);

                (l.*e.set)(m_b.make_q8_0(e.rows, e.cols, raw.data(), raw.size()));
            }
            else
                (l.*e.get)()->write(narrowed(g.read(p + e.name)));
        }
    }
}

template<typename T>
void model<T>::enable_cache(unsigned int context) {
    const unsigned int n = context ? context : m_conf.context;

    for(std::size_t i = 0; i < m_layers.size(); i++)
        m_layers[i]->enable_cache(n);
}

template<typename T>
void model<T>::reset_cache() {
    for(std::size_t i = 0; i < m_layers.size(); i++)
        m_layers[i]->reset_cache();
}

template<typename T>
void model<T>::reserve(unsigned int seq) {
    if(seq == 0)
        throw backend_error("model: a sequence of no positions");

    if(seq == m_seq) return;   // see block::reserve

    m_seq = seq;

    m_x = m_b.make(m_conf.d_model, seq);
    m_y = m_b.make(m_conf.d_model, seq);

    for(std::size_t i = 0; i < m_layers.size(); i++)
        m_layers[i]->reserve(seq);
}

template<typename T>
void model<T>::forward(const std::vector<int>& ids, tensor_ptr& logits,
                       unsigned int base_pos)
{
    if(m_seq == 0)
        throw backend_error("model: forward before reserve");

    if(ids.size() != m_seq)
        throw backend_error("model: as many ids as the reserved length");

    if(logits->rows() != m_conf.vocab || logits->cols() != m_seq)
        throw backend_error("model: logits must be vocab by the reserved length");

    m_b.gather(m_embed, ids, m_x);

    // Scaled after the lookup and not in the table, because the table is also
    // the output head when the file ties them -- as Gemma's does -- and
    // scaling it would scale the logits too.
    //
    // x += (s - 1) x  is  x *= s, and needs no operation the backend did not
    // already have.  Safe aliased: add_scaled reads and writes the same index.
    if(m_conf.embed_scale)
        m_b.add_scaled(T(m_conf.embed_scale - 1.0f), m_x, m_x);

    // Ping-pong rather than in place: block::forward writes its output while
    // still reading its input.
    for(std::size_t i = 0; i < m_layers.size(); i++) {
        m_layers[i]->forward(m_x, m_y, true, base_pos);

        m_x.swap(m_y);
    }

    m_b.rms_norm(m_x, m_final_norm, m_y, m_conf.rms_eps);
    if(m_head_q) m_b.multiply_tn(m_head_q, m_y, logits);
    else m_b.multiply_tn(m_head, m_y, logits);

    // Monotone, so it cannot change an argmax -- but it changes every
    // temperature above zero, and it keeps the logits inside what a half
    // holds.
    m_b.softcap(logits, m_conf.final_cap);
}

}
}

#endif // JLIB_AI_MODEL_HH
