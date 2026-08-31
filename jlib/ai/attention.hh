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

#ifndef JLIB_AI_ATTENTION_HH
#define JLIB_AI_ATTENTION_HH

#include <jlib/ai/backend.hh>

#include <cmath>

namespace jlib {
namespace ai {

/**
 * Scaled dot-product attention for one head.
 *
 * out = V . softmax(mask(K^T Q / sqrt(d)))
 *
 * **Not a virtual on backend.** Every step is already a primitive, so this is
 * one implementation that runs wherever the backend runs, rather than one per
 * backend to keep in agreement. A fused kernel would be faster -- that is what
 * FlashAttention is -- and would be a backend method when somebody measures a
 * reason for it.
 *
 * ### Shapes, and why they are the transpose of the usual drawing
 *
 * A column is a sample everywhere in this library, so here a column is a
 * *position*: q, k and v are (d x sequence), not (sequence x d).
 *
 *   q       (d x Tq)     one column per query position
 *   k       (d x Tk)     one column per key position
 *   v       (dv x Tk)    one column per key position, and dv need not equal d
 *   scores  (Tk x Tq)    scratch: row is the key, column the query
 *   probs   (Tk x Tq)    scratch: each column sums to one
 *   out     (dv x Tq)
 *
 * That falls out of softmax reducing down a column: a column has to be one
 * query's distribution over keys, so the key index goes on the rows, and
 * scores = K^T Q rather than Q K^T. It is worth stating plainly because every
 * paper draws it the other way, and because it is what decides which triangle
 * causal_mask clears.
 *
 * The consequence is that no transpose and no batched GEMM are needed:
 * multiply_tn already computes K^T Q, and 1/sqrt(d) rides along as its alpha.
 * Heads are separate tensors and a separate call each, which costs one GEMM
 * dispatch per head; batching them is a performance question, not a
 * correctness one.
 *
 * ### scratch
 *
 * scores and probs are the caller's, not allocated here. A transformer reuses
 * them across every layer and every step, and allocating a (Tk x Tq) tensor
 * per call would dominate. They must not alias each other or any input; that
 * is unchecked and untested.
 *
 * @param causal whether a query may see keys that come after it
 */
template<typename T>
void attention(backend<T>& b,
               const typename backend<T>::tensor_ptr& q,
               const typename backend<T>::tensor_ptr& k,
               const typename backend<T>::tensor_ptr& v,
               typename backend<T>::tensor_ptr& scores,
               typename backend<T>::tensor_ptr& probs,
               typename backend<T>::tensor_ptr& out,
               bool causal = true)
{
    const unsigned int d  = q->rows();
    const unsigned int tq = q->cols();
    const unsigned int tk = k->cols();
    const unsigned int dv = v->rows();

    if(k->rows() != d)
        throw backend_error("attention: q and k must have the same depth");

    if(v->cols() != tk)
        throw backend_error("attention: v must have one column per key");

    if(scores->rows() != tk || scores->cols() != tq ||
       probs->rows() != tk || probs->cols() != tq)
        throw backend_error("attention: scratch must be keys x queries");

    if(out->rows() != dv || out->cols() != tq)
        throw backend_error("attention: out must be v's depth x queries");

    // The scale rides along as the GEMM's alpha rather than as a pass of its
    // own.  1/sqrt(d) keeps the dot products from growing with depth, which is
    // what would otherwise drive softmax into the flat region where every
    // gradient is zero.
    b.multiply_tn(k, q, scores, T(1.0f / std::sqrt(float(d))), T(0));

    if(causal)
        b.causal_mask(scores);

    b.softmax(scores, probs);

    // out[:,i] = sum_j probs[j,i] * v[:,j] -- a plain multiply, because probs
    // already has the key index on the rows.
    b.multiply(v, probs, out);
}

}
}

#endif // JLIB_AI_ATTENTION_HH
