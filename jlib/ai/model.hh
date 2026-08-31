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

    if(arch != "llama")
        throw backend_error("model: this reads llama-architecture files, and "
                            "that one says '" + arch + "'");

    c.d_model = static_cast<unsigned int>(g.integer("llama.embedding_length"));
    c.heads   = static_cast<unsigned int>(g.integer("llama.attention.head_count"));
    c.d_ff    = static_cast<unsigned int>(g.integer("llama.feed_forward_length"));
    c.layers  = static_cast<unsigned int>(g.integer("llama.block_count"));
    c.context = static_cast<unsigned int>(g.integer("llama.context_length"));

    c.kv_heads = g.has("llama.attention.head_count_kv")
        ? static_cast<unsigned int>(g.integer("llama.attention.head_count_kv"))
        : c.heads;

    if(g.has("llama.rope.freq_base"))
        c.rope_theta = static_cast<float>(g.real("llama.rope.freq_base"));

    if(g.has("llama.attention.layer_norm_rms_epsilon"))
        c.rms_eps = static_cast<float>(
            g.real("llama.attention.layer_norm_rms_epsilon"));

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
            new block<T>(b, c.d_model, c.heads, c.kv_heads, c.d_ff));

        l->set_eps(c.rms_eps);
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
    const unsigned int dh = d / m_conf.heads;

    // No transposition for either of these.  The embedding is a table of
    // columns and gather reads columns; the head is used through multiply_tn,
    // which wants exactly the file's orientation.
    expect(g, "token_embd.weight", d, m_conf.vocab);
    m_embed->write(narrowed(g.read("token_embd.weight")));

    expect(g, "output.weight", d, m_conf.vocab);

    // Kept quantised where the file quantised it.  These are the tensors whose
    // blocks run along the dimension they are used on, so nothing has to be
    // rearranged and the file's bytes go to the device unchanged.
    if(g.tensor("output.weight").type == gguf::tensor_type::q8_0) {
        const std::vector<char> raw = g.read_raw("output.weight");

        set_head(m_b.make_q8_0(d, m_conf.vocab, raw.data(), raw.size()));
    }
    else
        m_head->write(narrowed(g.read("output.weight")));

    expect(g, "output_norm.weight", d, 1);
    m_final_norm->write(narrowed(g.read("output_norm.weight")));

    for(unsigned int i = 0; i < m_conf.layers; i++) {
        const std::string p = "blk." + std::to_string(i) + ".";

        block<T>& l = *m_layers[i];

        expect(g, p + "attn_norm.weight", d, 1);
        l.attn_norm()->write(narrowed(g.read(p + "attn_norm.weight")));

        expect(g, p + "ffn_norm.weight", d, 1);
        l.ffn_norm()->write(narrowed(g.read(p + "ffn_norm.weight")));

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

    // Ping-pong rather than in place: block::forward writes its output while
    // still reading its input.
    for(std::size_t i = 0; i < m_layers.size(); i++) {
        m_layers[i]->forward(m_x, m_y, true, base_pos);

        m_x.swap(m_y);
    }

    m_b.rms_norm(m_x, m_final_norm, m_y, m_conf.rms_eps);
    if(m_head_q) m_b.multiply_tn(m_head_q, m_y, logits);
    else m_b.multiply_tn(m_head, m_y, logits);
}

}
}

#endif // JLIB_AI_MODEL_HH
