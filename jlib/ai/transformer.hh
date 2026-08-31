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

#ifndef JLIB_AI_TRANSFORMER_HH
#define JLIB_AI_TRANSFORMER_HH

#include <jlib/ai/attention.hh>
#include <jlib/ai/backend.hh>

#include <vector>

namespace jlib {
namespace ai {

/**
 * One pre-norm transformer block: attention, then a gated feed-forward, each
 * around a residual.
 *
 *   h = x + attention(rms_norm(x))
 *   y = h + swiglu(rms_norm(h))
 *
 * Pre-norm -- normalise the branch input, not the sum -- because it is what
 * lets a deep stack train at all: the residual path from input to output stays
 * an unmodified sum, so a gradient reaches the bottom without passing through
 * any normalisation. Post-norm was the original and needs a warmup schedule to
 * survive depth.
 *
 * ### Heads are weights, not slices
 *
 * The usual formulation projects to a full (d_model x T) Q, then slices it into
 * heads. Here each head owns its own (d_head x d_model) projection, so
 * `wq(h) * x` produces that head's Q directly and nothing is ever sliced. The
 * two are the same arithmetic -- a slice of (W x) is (a slice of W) x -- and
 * this way needs no sub-tensor views, which no backend here has.
 *
 * The output projection is split the same way: `wo(h)` is (d_model x d_head),
 * and the heads are summed by accumulating into one output with the GEMM's
 * beta, rather than concatenated and multiplied once. Again the same
 * arithmetic, and again no concatenation op is needed.
 *
 * The cost is one GEMM dispatch per head instead of one batched call. That is
 * a performance question; see the notes on the branch.
 *
 * ### Scratch and aliasing
 *
 * reserve() sizes every intermediate for one sequence length, and forward()
 * requires exactly that length. Two of the feed-forward steps run in place --
 * activate and hadamard both write element i from element i and nothing else,
 * so aliasing input and output is safe for them and only for them.
 *
 * The per-head scratch is reused across heads. That is safe because a compute
 * encoder dispatches serially, which is what the whole stream design already
 * rests on; it is not safe to make the dispatch concurrent without giving each
 * head its own.
 */
template<typename T>
class block {
public:
    typedef typename backend<T>::tensor_ptr tensor_ptr;

    /**
     * @param heads    must divide d_model
     * @param kv_heads how many key-value heads the query heads share between
     *                 them; must divide heads.  Equal to heads is ordinary
     *                 multi-head attention
     * @param d_ff     the feed-forward width, conventionally a few times d_model
     */
    block(backend<T>& b, unsigned int d_model, unsigned int heads,
          unsigned int kv_heads, unsigned int d_ff);

    unsigned int d_model() const { return m_d_model; }
    unsigned int heads() const { return m_heads; }

    /**
     * How many key-value heads the query heads share.
     *
     * Grouped-query attention: query head h reads the key-value head
     * `h / (heads / kv_heads)`, so heads are grouped **contiguously** -- the
     * first group of query heads shares the first key-value head, and so on.
     *
     * That is a convention, and it is the one llama.cpp and every conversion
     * tool use, which is what matters since it decides how a file's weights
     * line up.  The alternative -- `h % kv_heads`, striping instead of
     * grouping -- is equally coherent and would load the same weights into
     * different places.  No test here can tell them apart; only generating text
     * and comparing it with a reference can.  Named and written down for the
     * same reason w_gate() and rope_layout are.
     */
    unsigned int kv_heads() const { return m_kv_heads; }

    /** Which key-value head a query head reads. */
    unsigned int kv_head_for(unsigned int h) const {
        return h / (m_heads / m_kv_heads);
    }
    unsigned int d_head() const { return m_d_head; }
    unsigned int d_ff() const { return m_d_ff; }

    /** (d_head x d_model), one per *query* head. */
    tensor_ptr& wq(unsigned int h) { return m_wq[h]; }

    /** (d_head x d_model), one per *key-value* head -- there are fewer. */
    tensor_ptr& wk(unsigned int h) { return m_wk[h]; }
    tensor_ptr& wv(unsigned int h) { return m_wv[h]; }

    /** (d_model x d_head), one per head; the heads are summed through these. */
    tensor_ptr& wo(unsigned int h) { return m_wo[h]; }

    /**
     * The feed-forward's three matrices, named for their roles.
     *
     * Gate and up are both (d_ff x d_model) and down is (d_model x d_ff), and
     * the only thing that distinguishes gate from up is that **silu is applied
     * to the gate**. That is a convention, not a property: swap them and the
     * result is a different but equally self-consistent network, which no test
     * here can tell from this one -- verified by mutation. It is only pinned by
     * a real model's weights, where the tensors arrive named ffn_gate, ffn_up
     * and ffn_down.
     *
     * So they are named rather than numbered. A loader that has to map
     * "ffn_gate" onto w1() can get it wrong silently; onto w_gate() it cannot.
     */
    tensor_ptr& w_gate() { return m_gate; }
    tensor_ptr& w_up() { return m_up; }
    tensor_ptr& w_down() { return m_down; }

    /** (d_model x 1) each: the learned RMS norm scales. */
    tensor_ptr& attn_norm() { return m_attn_norm; }
    tensor_ptr& ffn_norm() { return m_ffn_norm; }

    /**
     * Turn on rotary position embedding for this block's queries and keys.
     *
     * Not for the values: they carry what a position says, not which position
     * said it, and rotating them would make the thing attended *to* depend on
     * where it sat.
     *
     * Off by default, which leaves the block permutation-equivariant apart
     * from the causal mask -- reorder the input columns and the outputs
     * reorder with them.  Any real model wants this on.
     *
     * @param layout no test can check this for you; see ai::rope_layout
     */
    void set_rope(bool on, float theta = 10000.0f,
                  rope_layout layout = rope_layout::interleaved);

    /** Size every intermediate for a sequence of this length. */
    void reserve(unsigned int seq);

    /**
     * out = block(x), with x and out both (d_model x seq).
     *
     * @param base_pos the position of the first column, for decoding against a
     *                 cache where the columns being processed are not at the
     *                 start of the sequence
     */
    void forward(const tensor_ptr& x, tensor_ptr& out, bool causal = true,
                 unsigned int base_pos = 0);

private:
    backend<T>& m_b;

    unsigned int m_d_model;
    unsigned int m_heads;
    unsigned int m_kv_heads;
    unsigned int m_d_head;
    unsigned int m_d_ff;
    unsigned int m_seq = 0;

    bool m_rope = false;
    float m_theta = 10000.0f;
    rope_layout m_layout = rope_layout::interleaved;

    std::vector<tensor_ptr> m_wq, m_wk, m_wv, m_wo;
    tensor_ptr m_gate, m_down, m_up;
    tensor_ptr m_attn_norm, m_ffn_norm;

    tensor_ptr m_norm, m_q, m_scores, m_probs, m_head, m_attn;

    // One per key-value head, not one per query head: the whole point of
    // grouping is that a group's key and value are computed once and read by
    // every query head in it.
    std::vector<tensor_ptr> m_k, m_v;
    tensor_ptr m_h1, m_h3, m_ffn;
};

template<typename T>
block<T>::block(backend<T>& b, unsigned int d_model, unsigned int heads,
                unsigned int kv_heads, unsigned int d_ff)
    : m_b(b),
      m_d_model(d_model),
      m_heads(heads),
      m_kv_heads(kv_heads),
      m_d_head(heads ? d_model / heads : 0),
      m_d_ff(d_ff)
{
    if(heads == 0 || kv_heads == 0 || d_model == 0 || d_ff == 0)
        throw backend_error("block: every dimension must be non-zero");

    if(d_model % heads)
        throw backend_error("block: heads must divide d_model");

    if(heads % kv_heads)
        throw backend_error("block: kv_heads must divide heads, since each "
                            "key-value head serves a whole group of query "
                            "heads");

    for(unsigned int h = 0; h < heads; h++) {
        m_wq.push_back(b.make(m_d_head, d_model));
        m_wo.push_back(b.make(d_model, m_d_head));
    }

    // Fewer of these, which is the entire saving: TinyLlama has 32 query heads
    // and 4 key-value heads, so its attn_k is [2048, 256] where attn_q is
    // [2048, 2048].
    for(unsigned int h = 0; h < kv_heads; h++) {
        m_wk.push_back(b.make(m_d_head, d_model));
        m_wv.push_back(b.make(m_d_head, d_model));
    }

    m_gate = b.make(d_ff, d_model);
    m_up = b.make(d_ff, d_model);
    m_down = b.make(d_model, d_ff);

    m_attn_norm = b.make(d_model, 1);
    m_ffn_norm = b.make(d_model, 1);
}

template<typename T>
void block<T>::set_rope(bool on, float theta, rope_layout layout) {
    // Here rather than at the first forward(): d_head is fixed at construction,
    // so this is knowable now, and a shape error is worth having at the point
    // the caller made the choice.
    if(on && (m_d_head % 2))
        throw backend_error("block: rope rotates in planes, so d_model / heads "
                            "must be even");

    m_rope = on;
    m_theta = theta;
    m_layout = layout;
}

template<typename T>
void block<T>::reserve(unsigned int seq) {
    if(seq == 0)
        throw backend_error("block: a sequence of no positions");

    m_seq = seq;

    m_norm   = m_b.make(m_d_model, seq);
    m_q      = m_b.make(m_d_head, seq);
    m_scores = m_b.make(seq, seq);

    m_k.clear();
    m_v.clear();

    for(unsigned int h = 0; h < m_kv_heads; h++) {
        m_k.push_back(m_b.make(m_d_head, seq));
        m_v.push_back(m_b.make(m_d_head, seq));
    }

    m_probs  = m_b.make(seq, seq);
    m_head   = m_b.make(m_d_head, seq);
    m_attn   = m_b.make(m_d_model, seq);
    m_h1     = m_b.make(m_d_ff, seq);
    m_h3     = m_b.make(m_d_ff, seq);
    m_ffn    = m_b.make(m_d_model, seq);
}

template<typename T>
void block<T>::forward(const tensor_ptr& x, tensor_ptr& out, bool causal,
                       unsigned int base_pos)
{
    if(m_seq == 0)
        throw backend_error("block: forward before reserve");

    if(x->rows() != m_d_model || x->cols() != m_seq)
        throw backend_error("block: input is not d_model x the reserved length");

    if(out->rows() != m_d_model || out->cols() != m_seq)
        throw backend_error("block: output is not d_model x the reserved length");

    // --- attention, around a residual ---

    m_b.rms_norm(x, m_attn_norm, m_norm);

    // Every key and value first, once each.  Computing them inside the query
    // loop instead would give the same answer and do the work heads/kv_heads
    // times over -- which is exactly the cost grouping exists to avoid.
    for(unsigned int g = 0; g < m_kv_heads; g++) {
        m_b.multiply(m_wk[g], m_norm, m_k[g]);
        m_b.multiply(m_wv[g], m_norm, m_v[g]);

        // Keys are rotated here, once, for the same reason.  Values are not
        // rotated at all -- see set_rope.
        if(m_rope)
            m_b.rope(m_k[g], base_pos, m_theta, m_layout);
    }

    for(unsigned int h = 0; h < m_heads; h++) {
        m_b.multiply(m_wq[h], m_norm, m_q);

        if(m_rope)
            m_b.rope(m_q, base_pos, m_theta, m_layout);

        const unsigned int g = kv_head_for(h);

        attention(m_b, m_q, m_k[g], m_v[g], m_scores, m_probs, m_head, causal);

        // beta 0 for the first head and 1 for the rest, which sums the heads
        // in place of concatenating them and projecting once.
        m_b.multiply(m_wo[h], m_head, m_attn, T(1), h ? T(1) : T(0));
    }

    m_b.assign(x, out);
    m_b.add_scaled(T(1), m_attn, out);

    // --- gated feed-forward, around a second residual ---

    m_b.rms_norm(out, m_ffn_norm, m_norm);

    m_b.multiply(m_gate, m_norm, m_h1);
    m_b.multiply(m_up, m_norm, m_h3);

    // Both in place; see the note on aliasing above.  silu on the gate and not
    // on the up projection, which is the whole of what makes this SwiGLU --
    // and is a convention rather than something the arithmetic forces.
    m_b.activate(activation::silu, m_h1, m_h1);
    m_b.hadamard(m_h1, m_h3, m_h1);

    m_b.multiply(m_down, m_h1, m_ffn);

    m_b.add_scaled(T(1), m_ffn, out);
}

}
}

#endif // JLIB_AI_TRANSFORMER_HH
