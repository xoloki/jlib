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

#include <cmath>
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
    typedef typename backend<T>::quantised_ptr quantised_ptr;

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

    /**
     * One matrix each, in the file's orientation, read with multiply_tn.
     *
     * Not one per head, which is what these were until the per-head loop was
     * measured at 77% of the time to produce a token. A projection for every
     * head is a single (d_model x d_model) multiply; asking for it thirty-two
     * times costs thirty-two times the asking and the same amount of doing.
     *
     *   wq  d_model x (heads * d_head)
     *   wk  d_model x (kv_heads * d_head)
     *   wv  d_model x (kv_heads * d_head)
     *   wo  (heads * d_head) x d_model, the heads concatenated on its input
     *
     * Which is also exactly how a GGUF stores them, so loading is a copy and
     * they can stay in the quantisation they arrived in -- the row slicing
     * that stopped wo joining the others is gone with the loop that needed it.
     */
    tensor_ptr& wq() { return m_wq; }
    tensor_ptr& wk() { return m_wk; }
    tensor_ptr& wv() { return m_wv; }
    tensor_ptr& wo() { return m_wo; }

    /** The same four kept in the encoding a file used. */
    void set_wq(const quantised_ptr& q) { m_wq_q = q; m_wq.reset(); }
    void set_wk(const quantised_ptr& q) { m_wk_q = q; m_wk.reset(); }
    void set_wv(const quantised_ptr& q) { m_wv_q = q; m_wv.reset(); }
    void set_wo(const quantised_ptr& q) { m_wo_q = q; m_wo.reset(); }

    /**
     * The feed-forward's three matrices, named for their roles.
     *
     * **In the file's orientation**, which is the transpose of the obvious one:
     * gate and up are (d_model x d_ff) and down is (d_ff x d_model), each
     * multiplied with multiply_tn. That is what a GGUF holds -- see gguf.hh --
     * and holding it the same way means a weight can stay in the quantisation
     * it arrived in, whose blocks run along the contiguous dimension and would
     * not survive being transposed.
     *
     * The only thing that distinguishes gate from up is that **silu is applied
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

    /**
     * The same three weights, kept in the encoding a file used.
     *
     * Setting one **releases** the tensor above for that weight, which is the
     * point: holding both costs more than holding neither saves. The shape and
     * the arithmetic are identical, only the storage differs. These three are 69%
     * of a Llama's parameters and none of them is sliced, which is why they are
     * the ones that can be kept quantised -- see the notes on the branch for
     * what stops the attention weights joining them.
     */
    void set_gate(const quantised_ptr& q) { m_gate_q = q; m_gate.reset(); }
    void set_up(const quantised_ptr& q) { m_up_q = q; m_up.reset(); }
    void set_down(const quantised_ptr& q) { m_down_q = q; m_down.reset(); }

    bool ffn_quantised() const { return bool(m_gate_q); }

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

    /**
     * The epsilon both RMS norms are computed with.
     *
     * A model states its own -- llama.attention.layer_norm_rms_epsilon -- and
     * while every Llama so far has said 1e-5, taking it from the file costs
     * nothing and a model that said otherwise would otherwise be quietly
     * normalised wrong.
     */
    void set_eps(float eps) { m_eps = eps; }
    float eps() const { return m_eps; }

    /**
     * Keep the keys and values, so a later call need only supply what is new.
     *
     * Without this every call recomputes the whole sequence, which is what
     * generation did and is the reference this has to reproduce: with a cache,
     * greedy generation must produce the identical tokens.
     *
     * The cache is allocated for the whole context at once and filled as it
     * goes. What is past its length holds zeros, and is masked by the same
     * offset test that enforces causality -- see backend::causal_mask, which
     * is why nothing here has to tell attention how full the cache is.
     *
     * ### It grows rather than starting full
     *
     * The obvious thing is to allocate the whole context up front, and it is
     * measurably wrong. Attention reads the *allocated* cache, not the part of
     * it that holds keys, so a 2048-column cache holding ninety tokens still
     * reads 2048 columns -- for every query head, in every layer. Measured on
     * TinyLlama, forty tokens of output: 0.125s per token with a 128-column
     * cache and 0.330s with a 2048-column one, against 0.146s for no cache at
     * all. Allocating the context up front made the cache **2.3x slower than
     * not having one**.
     *
     * So the capacity starts at what the first call needs and doubles when it
     * fills, copying what is there across. That bounds the waste at 2x rather
     * than at the ratio of the context to the conversation.
     *
     * Attending over exactly the valid prefix would be better still, and needs
     * a tensor that can be a column range of another. This is the version that
     * does not need one.
     *
     * @param context the most it will ever hold -- a ceiling, not an allocation
     */
    void enable_cache(unsigned int context);

    /** Forget it, for a conversation that starts again. */
    void reset_cache() { m_cache_len = 0; }

    bool caching() const { return m_context != 0; }

    /** How many positions are in the cache. */
    unsigned int cached() const { return m_cache_len; }

    /** Size every intermediate for a sequence of this length. */
    void reserve(unsigned int seq);

    /**
     * out = block(x), with x and out both (d_model x seq).
     *
     * @param base_pos the position of the first column, for decoding against a
     *                 cache where the columns being processed are not at the
     *                 start of the sequence.  **Ignored when the cache is on**,
     *                 which knows the position better than a caller does: it
     *                 is however many tokens it already holds, and taking it
     *                 from anywhere else is a way for the two to disagree.
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

    float m_eps = 1e-5f;

    /** Non-zero once the cache is on, and then its length. */
    unsigned int m_context = 0;
    unsigned int m_cache_len = 0;

    bool m_rope = false;
    float m_theta = 10000.0f;
    rope_layout m_layout = rope_layout::interleaved;

    tensor_ptr m_wq, m_wk, m_wv, m_wo;
    quantised_ptr m_wq_q, m_wk_q, m_wv_q, m_wo_q;
    tensor_ptr m_gate, m_down, m_up;
    quantised_ptr m_gate_q, m_up_q, m_down_q;
    tensor_ptr m_attn_norm, m_ffn_norm;

    tensor_ptr m_norm, m_scores, m_probs, m_attn;

    /** Every head side by side: q is d_model tall, k and v narrower. */
    tensor_ptr m_qs, m_ks, m_vs, m_heads_out;

    /** ((kv_heads * d_head) x capacity), one tensor rather than one per head. */
    tensor_ptr m_kc, m_vc;

    /** What is allocated now, against m_context which is the ceiling. */
    unsigned int m_capacity = 0;

    void grow_cache(unsigned int need);
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

    // One matrix each, in the file's orientation.  attn_k is narrower than
    // attn_q by the ratio of key-value heads to query heads -- [2048, 256]
    // against [2048, 2048] for TinyLlama, which is grouping visible in a shape.
    m_wq = b.make(d_model, heads * m_d_head);
    m_wk = b.make(d_model, kv_heads * m_d_head);
    m_wv = b.make(d_model, kv_heads * m_d_head);
    m_wo = b.make(heads * m_d_head, d_model);

    // The file's orientation; see w_gate().
    m_gate = b.make(d_model, d_ff);
    m_up = b.make(d_model, d_ff);
    m_down = b.make(d_ff, d_model);

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
void block<T>::enable_cache(unsigned int context) {
    if(context == 0)
        throw backend_error("block: a cache with no room in it");

    m_context = context;
    m_cache_len = 0;
    m_capacity = 0;

    m_kc.reset();
    m_vc.reset();

    // Nothing allocated yet: the first forward() sizes it to what it needs.
}

template<typename T>
void block<T>::grow_cache(unsigned int need) {
    if(need <= m_capacity) return;

    if(need > m_context)
        throw backend_error("block: the cache is full");

    // Double, or jump straight to what is asked for when that is more -- a
    // prompt arrives all at once and there is no reason to reach its length in
    // steps.
    unsigned int want = m_capacity ? m_capacity * 2 : need;

    if(want < need) want = need;
    if(want > m_context) want = m_context;

    tensor_ptr k = m_b.make(m_kv_heads * m_d_head, want);
    tensor_ptr v = m_b.make(m_kv_heads * m_d_head, want);

    if(m_cache_len) {
        m_b.copy_columns(m_kc, k, 0);
        m_b.copy_columns(m_vc, v, 0);

        // Waited for: the old tensors go out of scope at the swap, and on a
        // GPU an encoded copy has not run yet when the call returns.
        m_b.wait();
    }

    m_kc.swap(k);
    m_vc.swap(v);

    m_capacity = want;

    // The scores are as tall as the cache, so they move with it.
    m_scores = m_b.make(m_capacity, m_seq * m_heads);
    m_probs = m_b.make(m_capacity, m_seq * m_heads);
}

template<typename T>
void block<T>::reserve(unsigned int seq) {
    if(seq == 0)
        throw backend_error("block: a sequence of no positions");

    if(m_context && seq > m_context)
        throw backend_error("block: more positions at once than the cache holds");

    // Nothing to do if the shape has not changed.  Decoding against a cache
    // asks for one column every time, and rebuilding a dozen identical tensors
    // per block per token is pure waste -- measured at a third of the step.
    if(seq == m_seq) return;

    m_seq = seq;

    m_norm       = m_b.make(m_d_model, seq);
    m_qs         = m_b.make(m_heads * m_d_head, seq);
    m_ks         = m_b.make(m_kv_heads * m_d_head, seq);
    m_vs         = m_b.make(m_kv_heads * m_d_head, seq);
    m_heads_out  = m_b.make(m_heads * m_d_head, seq);

    // As tall as the cache when there is one, and as wide as every head's
    // queries side by side.  grow_cache() remakes these when the capacity
    // changes.
    m_scores = m_b.make(m_capacity ? m_capacity : seq, seq * m_heads);


    m_probs  = m_b.make(m_capacity ? m_capacity : seq, seq * m_heads);
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

    m_b.rms_norm(x, m_attn_norm, m_norm, m_eps);

    // Where in the sequence these columns sit.  The cache knows; a caller
    // only knows when there is no cache to disagree with.
    const unsigned int at = m_context ? m_cache_len : base_pos;

    if(m_context) grow_cache(at + m_seq);

    // One call for every head's queries, and one each for the keys and values.
    // These were thirty-two and four calls; the arithmetic is identical and the
    // asking is not.
    if(m_wq_q) m_b.multiply_tn(m_wq_q, m_norm, m_qs);
    else m_b.multiply_tn(m_wq, m_norm, m_qs);

    if(m_wk_q) m_b.multiply_tn(m_wk_q, m_norm, m_ks);
    else m_b.multiply_tn(m_wk, m_norm, m_ks);

    if(m_wv_q) m_b.multiply_tn(m_wv_q, m_norm, m_vs);
    else m_b.multiply_tn(m_wv, m_norm, m_vs);

    // Rotated inside each head, never across the boundary between two.  Values
    // are not rotated at all -- see set_rope.
    if(m_rope) {
        m_b.rope(m_qs, at, m_theta, m_layout, m_d_head);
        m_b.rope(m_ks, at, m_theta, m_layout, m_d_head);
    }

    // Rotated before they are stored, so a key is rotated once however many
    // times it is later read.
    if(m_context) {
        m_b.copy_columns(m_ks, m_kc, at);
        m_b.copy_columns(m_vs, m_vc, at);
    }

    const tensor_ptr& keys = m_context ? m_kc : m_ks;
    const tensor_ptr& values = m_context ? m_vc : m_vs;

    // And the attention itself, four calls for every head rather than four per
    // head.  The scale rides here rather than in a pass of its own.
    m_b.attention_scores(m_qs, keys, m_scores, m_heads, m_kv_heads, m_d_head,
                         T(1.0f / std::sqrt(float(m_d_head))));

    // The offset is how many keys precede the queries, which is not the same
    // as where the queries are in the sequence. With a cache they differ only
    // because the cache holds the earlier keys; without one every key belongs
    // to this batch and the offset is zero however far along base_pos says the
    // batch sits. Passing `at` here regardless made base_pos shift the mask,
    // which the uncached rope test caught immediately -- including its control,
    // where base_pos should not have mattered at all.
    if(causal) m_b.causal_mask(m_scores, m_context ? at : 0, m_seq);

    m_b.softmax(m_scores, m_probs);

    m_b.attention_weighted(values, m_probs, m_heads_out, m_heads, m_kv_heads,
                           m_d_head);

    // The heads arrive concatenated, so summing them is one multiply against
    // the whole output projection rather than an accumulation per head.
    if(m_wo_q) m_b.multiply_tn(m_wo_q, m_heads_out, m_attn);
    else m_b.multiply_tn(m_wo, m_heads_out, m_attn);

    m_b.assign(x, out);
    m_b.add_scaled(T(1), m_attn, out);

    // --- gated feed-forward, around a second residual ---

    m_b.rms_norm(out, m_ffn_norm, m_norm, m_eps);

    if(m_gate_q) m_b.multiply_tn(m_gate_q, m_norm, m_h1);
    else m_b.multiply_tn(m_gate, m_norm, m_h1);

    if(m_up_q) m_b.multiply_tn(m_up_q, m_norm, m_h3);
    else m_b.multiply_tn(m_up, m_norm, m_h3);

    // Both in place; see the note on aliasing above.  silu on the gate and not
    // on the up projection, which is the whole of what makes this SwiGLU --
    // and is a convention rather than something the arithmetic forces.
    m_b.activate(activation::silu, m_h1, m_h1);
    m_b.hadamard(m_h1, m_h3, m_h1);

    if(m_down_q) m_b.multiply_tn(m_down_q, m_h1, m_ffn);
    else m_b.multiply_tn(m_down, m_h1, m_ffn);

    m_b.add_scaled(T(1), m_ffn, out);

    // Advanced after everything has read it, since `at` is the position these
    // columns occupy rather than the position after them.
    if(m_context) m_cache_len += m_seq;
}

}
}

#endif // JLIB_AI_TRANSFORMER_HH
