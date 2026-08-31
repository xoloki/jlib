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
 */

#ifndef JLIB_AI_BACKEND_HH
#define JLIB_AI_BACKEND_HH

#include <jlib/math/matrix.hh>

#include <cmath>
#include <vector>
#include <memory>
#include <limits>
#include <sstream>
#include <string>

namespace jlib {
namespace ai {

/**
 * Which nonlinearity a layer applies.
 *
 * Every one has a derivative expressible in terms of its own *output*, which
 * is what lets a backend cache one value per layer rather than two:
 *
 *   sigmoid      s (1 - s)
 *   tanh         1 - s^2
 *   relu         s > 0 ? 1 : 0
 *   leaky_relu   s > 0 ? 1 : LEAK
 */
enum class activation { sigmoid, tanh, relu, leaky_relu, silu };

/**
 * Whether f'(x) can be recovered from f(x), which is what slope() is given.
 *
 * False for silu alone, and not as an oversight: silu(x) = x * sigmoid(x) has
 * a minimum of about -0.278 near x = -1.278 and rises on both sides of it, so
 * it is not injective.  An output in (-0.278, 0) came from one of two inputs
 * with two different slopes, and nothing in the output says which.  Every other
 * activation here is monotonic, which is the property slope() quietly rests on.
 *
 * So silu is a forward-only activation until slope() is given the layer's input
 * instead of its output.  That is a change to every caller, and inference does
 * not need it -- see #TODO in the branch notes.
 */
inline bool slope_from_output(activation a) {
    return a != activation::silu;
}

/**
 * Which dimensions RoPE pairs up before rotating them.
 *
 * Both are in use and they are not compatible: a model trained under one and
 * run under the other produces fluent nonsense rather than an error.
 *
 * - `interleaved` pairs (0,1), (2,3), (4,5)... -- the original RoFormer paper,
 *   and what ggml calls the normal rope type.
 * - `split` pairs (j, j + d/2) -- what GPT-NeoX introduced and what the
 *   HuggingFace Llama implementation uses, with the checkpoint's weights
 *   permuted at conversion time to match.
 *
 * **No test here can tell them apart**, and that is not a gap in the tests:
 * every property RoPE has -- that it preserves the norm, that it is the
 * identity at position 0, that the inner product depends only on the relative
 * position -- holds for both, because both are block-diagonal rotations and
 * differ only in which coordinates share a block.  It is settled by the model,
 * so it is a parameter with a name rather than a choice buried in a kernel.
 */
enum class rope_layout { interleaved, split };

/**
 * What a backend throws.
 *
 * At namespace scope rather than nested in backend<T>: a nested class of a
 * template is a different type per instantiation, so a caller catching one
 * would have to know which element type threw it, and the free functions
 * below could not throw it at all.
 */
class backend_error : public std::exception {
public:
    backend_error(const std::string& msg = "") {
        m_msg = "jlib::ai::backend exception" + (msg.empty() ? "" : ": " + msg);
    }
    virtual ~backend_error() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};

/** The slope a leaky ReLU keeps below zero. */
inline constexpr double LEAK = 0.01;

std::string name_of(activation a);
activation activation_from_name(const std::string& s);

/**
 * Where a network's numbers live and who does the arithmetic.
 *
 * ## Why this exists
 *
 * `jlib/cuda/neural.hh` is a 399-line copy of the network with the multiplies
 * routed through cuBLAS.  It carries three bugs that have since been fixed in
 * the original -- fan-out initialisation, backpropagation through
 * already-updated weights, and a gradient that is not a mean -- and nobody
 * noticed, because it does not build without CUDA (#138).  A second fork for
 * Metal would have made a third copy and a second wrong one.
 *
 * So the training step exists once and the *device* is the parameter.  A
 * backend supplies opaque tensors and the eight operations a step performs;
 * nothing above it knows whether the numbers are in host memory or on a GPU.
 *
 * ## The contract that is easy to get wrong
 *
 * **Operations may be deferred.**  A GPU backend batches them into one
 * submission, because synchronising per operation is what made the first Metal
 * wiring slow -- measured at up to 7.9x. So a tensor's contents are undefined
 * until wait() has returned, and read() before that is a bug this cannot
 * detect for you.
 *
 * The CPU backend runs everything immediately and its wait() does nothing,
 * which means code that forgets to wait works there and fails on a GPU.  Write
 * the wait.
 */
template<typename T>
class backend {
public:
    typedef backend_error exception;

    typedef T value_type;

    /** A matrix, wherever the backend keeps it. */
    class tensor {
    public:
        virtual ~tensor() {}

        virtual unsigned int rows() const = 0;
        virtual unsigned int cols() const = 0;

        unsigned int size() const { return rows() * cols(); }

        /** Copy to the host.  Only meaningful after backend::wait(). */
        virtual math::matrix<T> read() const = 0;

        /** Copy from the host. */
        virtual void write(const math::matrix<T>& m) = 0;
    };

    typedef std::shared_ptr<tensor> tensor_ptr;

    virtual ~backend() {}

    /** What this is, for a caller that wants to say where it ran. */
    virtual std::string name() const = 0;

    virtual tensor_ptr make(unsigned int rows, unsigned int cols) = 0;
    virtual tensor_ptr make(const math::matrix<T>& m) = 0;

    /** c = alpha * a * b + beta * c */
    virtual void multiply(const tensor_ptr& a, const tensor_ptr& b,
                          tensor_ptr& c, T alpha = T(1), T beta = T(0)) = 0;

    /** c = alpha * a^T * b + beta * c */
    virtual void multiply_tn(const tensor_ptr& a, const tensor_ptr& b,
                             tensor_ptr& c, T alpha = T(1), T beta = T(0)) = 0;

    /** c = alpha * a * b^T + beta * c */
    virtual void multiply_nt(const tensor_ptr& a, const tensor_ptr& b,
                             tensor_ptr& c, T alpha = T(1), T beta = T(0)) = 0;

    virtual void activate(activation kind, const tensor_ptr& in, tensor_ptr& out) = 0;

    /** The derivative, from the layer's own output. */
    virtual void slope(activation kind, const tensor_ptr& out_of_layer,
                       tensor_ptr& out) = 0;

    virtual void hadamard(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) = 0;
    virtual void subtract(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) = 0;

    /** y += alpha * x */
    virtual void add_scaled(T alpha, const tensor_ptr& x, tensor_ptr& y) = 0;

    /** dst = src.  Same shape, no reallocation. */
    virtual void assign(const tensor_ptr& src, tensor_ptr& dst) = 0;

    /**
     * Softmax down each column.
     *
     * A column is a sample -- matrices here are (features x batch) -- so this
     * reduces over features and leaves each column summing to one.
     *
     * The maximum is subtracted first.  exp() of a score of 90 overflows a
     * float and every score in that column becomes nan; subtracting the
     * column's own maximum shifts the largest exponent to zero, which changes
     * nothing about the result and everything about whether it exists.  In
     * fp16 the ceiling is 65504, which arrives at a score of about 11.
     */
    virtual void softmax(const tensor_ptr& in, tensor_ptr& out) = 0;

    /**
     * Set everything strictly below the diagonal to -infinity, in place.
     *
     * For causal attention, where a query may not see a key that comes after
     * it.  Strictly *below* the diagonal, which is the transpose of how this
     * is usually drawn: softmax here reduces down a column, so a column has to
     * be one query's distribution over keys, which puts the key index on the
     * rows.  A query at i must not see a key at j > i, and j > i is row >
     * column.  See attention.hh, which is the only caller and says the same
     * thing from the other end.
     *
     * -infinity rather than a large negative number, because softmax subtracts
     * the column maximum and exp(-inf - m) is exactly zero for finite m.  A
     * fully masked column would give nan; this mask cannot produce one, since
     * element (c,c) is always kept.
     */
    virtual void causal_mask(tensor_ptr& s) = 0;

    /**
     * Pick columns out of a table: out[:,i] = table[:,ids[i]].
     *
     * The embedding lookup.  A token is an index into a table of vectors, and
     * a column is a sample here, so a token's vector is a column and this is a
     * column gather.  GGUF stores token_embd.weight as [d_model, vocab], which
     * lands as exactly that table with no rearrangement.
     *
     * ids are host-side ints rather than a tensor because they are indices,
     * not arithmetic: routing them through T would lose them.  fp16 represents
     * integers exactly only to 2048, and vocabularies are far larger than
     * that, so a token id in a half is a silently wrong token.
     *
     * @throws backend_error if any id is outside the table
     */
    virtual void gather(const tensor_ptr& table, const std::vector<int>& ids,
                        tensor_ptr& out) = 0;

    /**
     * Rotary position embedding, in place.
     *
     * Rotates each column by an angle that grows with its position, in
     * `rows/2` independent two-dimensional planes, each turning at its own
     * rate.  Applied to the queries and the keys after their projections and
     * before the scores -- never to the values, which carry content rather
     * than position.
     *
     * What it buys is that the *score* between a query and a key comes to
     * depend on the distance between them rather than on where the pair sits:
     * rotating by m and by n leaves an inner product that is a function of
     * n - m alone.  That is the whole idea, and it is what the tests check.
     *
     * A column is a position, so column c is at `base_pos + c`.  base_pos is
     * for decoding one token at a time against a cache, where the single
     * column being processed is at position n rather than 0.
     *
     * @param theta  the frequency base, 10000 by convention
     * @param layout which dimensions get paired; see rope_layout, and note
     *               that no test can check this one for you
     */
    virtual void rope(tensor_ptr& x, unsigned int base_pos = 0,
                      float theta = 10000.0f,
                      rope_layout layout = rope_layout::interleaved) = 0;

    /**
     * Root-mean-square normalisation down each column, scaled per feature.
     *
     * out[r,c] = in[r,c] / sqrt(mean(in[:,c]^2) + eps) * weight[r]
     *
     * What a transformer uses in place of layer normalisation: no mean
     * subtraction and no bias, which makes it cheaper and, in practice, no
     * worse.  weight is one column of `rows` entries -- the learned scale --
     * and is taken as an argument rather than applied afterwards because
     * there is no broadcast operation to apply it with.
     */
    virtual void rms_norm(const tensor_ptr& in, const tensor_ptr& weight,
                          tensor_ptr& out, float eps = 1e-5f) = 0;

    /** Everything encoded so far has finished when this returns. */
    virtual void wait() = 0;
};

/**
 * The default: host memory and math::matrix.
 *
 * Every operation runs immediately, so wait() has nothing to do.  Kept simple
 * on purpose -- it is the reference the GPU backends are tested against, and
 * anything clever in here would be something to be wrong about twice.
 */
template<typename T>
class host_backend : public backend<T> {
public:
    typedef typename backend<T>::tensor_ptr tensor_ptr;

    std::string name() const { return "host"; }

    tensor_ptr make(unsigned int rows, unsigned int cols);
    tensor_ptr make(const math::matrix<T>& m);

    void multiply(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                  T alpha = T(1), T beta = T(0));
    void multiply_tn(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                     T alpha = T(1), T beta = T(0));
    void multiply_nt(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                     T alpha = T(1), T beta = T(0));

    void activate(activation kind, const tensor_ptr& in, tensor_ptr& out);
    void slope(activation kind, const tensor_ptr& out_of_layer, tensor_ptr& out);
    void hadamard(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c);
    void subtract(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c);
    void add_scaled(T alpha, const tensor_ptr& x, tensor_ptr& y);
    void assign(const tensor_ptr& src, tensor_ptr& dst);
    void softmax(const tensor_ptr& in, tensor_ptr& out);
    void causal_mask(tensor_ptr& s);
    void gather(const tensor_ptr& table, const std::vector<int>& ids,
                tensor_ptr& out);
    void rope(tensor_ptr& x, unsigned int base_pos = 0, float theta = 10000.0f,
              rope_layout layout = rope_layout::interleaved);
    void rms_norm(const tensor_ptr& in, const tensor_ptr& weight,
                  tensor_ptr& out, float eps = 1e-5f);

    void wait() {}

    /** The matrix behind a host tensor, for the backend's own use. */
    static math::matrix<T>& at(const tensor_ptr& t);
};

/** f(x), elementwise.  Public because the tests compare against it. */
template<typename T>
math::matrix<T> activate_matrix(activation a, const math::matrix<T>& in);

/** f'(x) from f's output, elementwise. */
template<typename T>
math::matrix<T> slope_matrix(activation a, const math::matrix<T>& out);


// ---------------------------------------------------------------------------
// Definitions.  Templates, so they live here rather than in backend.cc, which
// keeps only the two functions that do not depend on the element type.
// ---------------------------------------------------------------------------

/**
 * What to compute an activation in, given what it is stored as.
 *
 * float for float and _Float16 -- computing a half in half is no more accurate
 * than computing it in float and rounding, and it keeps the host answer beside
 * the GPU's, which works in float.  double keeps its own precision: going
 * through float there would quietly throw away half the digits.
 */
template<typename T>
struct compute_in { typedef float type; };

template<> struct compute_in<double> { typedef double type; };

template<typename T>
math::matrix<T> activate_matrix(activation a, const math::matrix<T>& in) {
    typedef typename compute_in<T>::type C;

    math::matrix<T> out(in.M, in.N);

    for(uint r = 0; r < in.M; r++) {
        for(uint c = 0; c < in.N; c++) {
            // Through float: std::exp and std::tanh have no _Float16 overload,
            // and a half computed in half is no more accurate than one
            // computed in float and rounded.
            const C x = C(in(r, c));

            switch(a) {
            case activation::sigmoid:    out(r,c) = T(C(1) / (C(1) + std::exp(-x))); break;
            case activation::tanh:       out(r,c) = T(std::tanh(x));                 break;
            case activation::relu:       out(r,c) = T((x > 0) ? x : C(0));           break;
            case activation::leaky_relu: out(r,c) = T((x > 0) ? x : C(LEAK) * x);    break;
            // x * sigmoid(x).  Smooth, non-monotonic, and what a modern
            // feed-forward gates with -- see swiglu in transformer.hh.
            case activation::silu:       out(r,c) = T(x / (C(1) + std::exp(-x))); break;
            }
        }
    }

    return out;
}

template<typename T>
math::matrix<T> slope_matrix(activation a, const math::matrix<T>& out) {
    typedef typename compute_in<T>::type C;

    // Before the loop, not inside it: there is no per-element answer to give.
    if(!slope_from_output(a))
        throw backend_error("slope: this activation is not invertible from its "
                            "output, so its derivative cannot be recovered from "
                            "one -- it is forward-only");

    math::matrix<T> d(out.M, out.N);

    for(uint r = 0; r < out.M; r++) {
        for(uint c = 0; c < out.N; c++) {
            const C s = C(out(r, c));

            switch(a) {
            case activation::sigmoid:    d(r,c) = T(s * (C(1) - s));      break;
            case activation::tanh:       d(r,c) = T(C(1) - s * s);        break;
            case activation::relu:       d(r,c) = T((s > 0) ? C(1) : C(0)); break;
            case activation::leaky_relu: d(r,c) = T((s > 0) ? C(1) : C(LEAK)); break;
            case activation::silu:       break;   // refused above
            }
        }
    }

    return d;
}

/** A tensor that is just a matrix. */
template<typename T>
class host_tensor : public backend<T>::tensor {
public:
    host_tensor(unsigned int rows, unsigned int cols) : m(rows, cols) {}
    host_tensor(const math::matrix<T>& from) : m(from) {}

    unsigned int rows() const { return m.M; }
    unsigned int cols() const { return m.N; }

    math::matrix<T> read() const { return m; }

    void write(const math::matrix<T>& from) {
        if(from.M != m.M || from.N != m.N) {
            std::ostringstream o;
            o << "cannot write [" << from.M << "," << from.N << "] into ["
              << m.M << "," << m.N << "]";
            throw backend_error(o.str());
        }

        m = from;
    }

    math::matrix<T> m;
};

template<typename T>
math::matrix<T>& host_backend<T>::at(const tensor_ptr& t) {
    host_tensor<T>* h = dynamic_cast<host_tensor<T>*>(t.get());

    // A tensor from another backend.  Checked rather than static_cast: the
    // two are the same type at the call site, and the consequence otherwise
    // is reading a GPU buffer as host memory.
    if(h == 0)
        throw backend_error("that tensor did not come from this backend");

    return h->m;
}

template<typename T>
typename backend<T>::tensor_ptr host_backend<T>::make(unsigned int rows,
                                                      unsigned int cols)
{
    return tensor_ptr(new host_tensor<T>(rows, cols));
}

template<typename T>
typename backend<T>::tensor_ptr host_backend<T>::make(const math::matrix<T>& m) {
    return tensor_ptr(new host_tensor<T>(m));
}

namespace detail {

/** out = alpha * p + beta * out, which every multiply below ends with. */
template<typename T>
void blend(const math::matrix<T>& p, math::matrix<T>& out, T alpha, T beta) {
    typedef typename compute_in<T>::type C;

    for(uint r = 0; r < out.M; r++)
        for(uint c = 0; c < out.N; c++)
            out(r,c) = T(C(alpha) * C(p(r,c)) + C(beta) * C(out(r,c)));
}

}

template<typename T>
void host_backend<T>::multiply(const tensor_ptr& a, const tensor_ptr& b,
                               tensor_ptr& c, T alpha, T beta)
{
    detail::blend(at(a) * at(b), at(c), alpha, beta);
}

template<typename T>
void host_backend<T>::multiply_tn(const tensor_ptr& a, const tensor_ptr& b,
                                  tensor_ptr& c, T alpha, T beta)
{
    detail::blend(at(a).transpose() * at(b), at(c), alpha, beta);
}

template<typename T>
void host_backend<T>::multiply_nt(const tensor_ptr& a, const tensor_ptr& b,
                                  tensor_ptr& c, T alpha, T beta)
{
    detail::blend(at(a) * at(b).transpose(), at(c), alpha, beta);
}

template<typename T>
void host_backend<T>::activate(activation kind, const tensor_ptr& in, tensor_ptr& out) {
    at(out) = activate_matrix(kind, at(in));
}

template<typename T>
void host_backend<T>::slope(activation kind, const tensor_ptr& out_of_layer,
                            tensor_ptr& out)
{
    at(out) = slope_matrix(kind, at(out_of_layer));
}

template<typename T>
void host_backend<T>::hadamard(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) {
    at(c) = at(a) ^ at(b);
}

template<typename T>
void host_backend<T>::subtract(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) {
    at(c) = at(a) - at(b);
}

template<typename T>
void host_backend<T>::add_scaled(T alpha, const tensor_ptr& x, tensor_ptr& y) {
    math::matrix<T>& out = at(y);
    const math::matrix<T>& in = at(x);

    typedef typename compute_in<T>::type C;

    for(uint r = 0; r < out.M; r++)
        for(uint c = 0; c < out.N; c++)
            out(r,c) = T(C(out(r,c)) + C(alpha) * C(in(r,c)));
}

template<typename T>
void host_backend<T>::assign(const tensor_ptr& src, tensor_ptr& dst) {
    at(dst) = at(src);
}

template<typename T>
void host_backend<T>::softmax(const tensor_ptr& in, tensor_ptr& out) {
    const math::matrix<T>& x = at(in);
    math::matrix<T>& y = at(out);

    if(x.M != y.M || x.N != y.N)
        throw backend_error("softmax: shapes differ");

    // In double regardless of T.  This is the reference the GPU is checked
    // against, so it should be the more accurate of the two rather than
    // matching its rounding.
    //
    // Which moves the overflow this guards against but does not remove it:
    // exp() in double survives to about 709 rather than 88, so the
    // max-subtraction is still load-bearing here and is still tested.
    for(uint c = 0; c < x.N; c++) {
        double m = -std::numeric_limits<double>::infinity();

        for(uint r = 0; r < x.M; r++)
            m = std::max(m, double(x(r,c)));

        double sum = 0;

        for(uint r = 0; r < x.M; r++) {
            const double e = std::exp(double(x(r,c)) - m);

            y(r,c) = T(e);
            sum += e;
        }

        // A column of all -inf would divide by zero.  Cannot arise from a
        // finite input, since subtracting the maximum leaves at least one
        // exponent at zero and so at least one term at one.
        for(uint r = 0; r < x.M; r++)
            y(r,c) = T(double(y(r,c)) / sum);
    }
}

template<typename T>
void host_backend<T>::causal_mask(tensor_ptr& s) {
    math::matrix<T>& x = at(s);

    // Through float, not std::numeric_limits<T>: _Float16 is a compiler
    // extension and numeric_limits is not specialised for it.  Converting a
    // float infinity to half gives a half infinity.
    const T neg_inf = T(-std::numeric_limits<float>::infinity());

    for(uint c = 0; c < x.N; c++)
        for(uint r = c + 1; r < x.M; r++)
            x(r,c) = neg_inf;
}

template<typename T>
void host_backend<T>::gather(const tensor_ptr& table, const std::vector<int>& ids,
                             tensor_ptr& out)
{
    const math::matrix<T>& t = at(table);
    math::matrix<T>& o = at(out);

    if(o.M != t.M || o.N != ids.size())
        throw backend_error("gather: out must be the table's height by the "
                            "number of ids");

    for(std::size_t i = 0; i < ids.size(); i++) {
        if(ids[i] < 0 || std::size_t(ids[i]) >= t.N) {
            std::ostringstream e;

            e << "gather: token id " << ids[i] << " is outside a table of "
              << t.N;

            throw backend_error(e.str());
        }

        for(uint r = 0; r < t.M; r++)
            o(r, uint(i)) = t(r, uint(ids[i]));
    }
}

template<typename T>
void host_backend<T>::rope(tensor_ptr& x, unsigned int base_pos, float theta,
                           rope_layout layout)
{
    math::matrix<T>& m = at(x);

    if(m.M % 2)
        throw backend_error("rope: rotates in planes, so it needs an even "
                            "number of rows");

    const uint half = m.M / 2;

    // In double, like softmax and for the same reason: this is the reference
    // the GPU is checked against.  It matters more here than elsewhere -- the
    // angle is position * frequency and grows without bound, so cos and sin of
    // it lose precision at long sequence lengths, and the host should be the
    // one that loses less.
    for(uint c = 0; c < m.N; c++) {
        const double pos = double(base_pos + c);

        for(uint j = 0; j < half; j++) {
            const uint a = (layout == rope_layout::split) ? j : 2 * j;
            const uint b = (layout == rope_layout::split) ? j + half : 2 * j + 1;

            const double freq = std::pow(double(theta),
                                         -2.0 * double(j) / double(m.M));
            const double angle = pos * freq;

            const double co = std::cos(angle);
            const double si = std::sin(angle);

            const double xa = double(m(a,c));
            const double xb = double(m(b,c));

            m(a,c) = T(xa * co - xb * si);
            m(b,c) = T(xa * si + xb * co);
        }
    }
}

template<typename T>
void host_backend<T>::rms_norm(const tensor_ptr& in, const tensor_ptr& weight,
                               tensor_ptr& out, float eps)
{
    const math::matrix<T>& x = at(in);
    const math::matrix<T>& w = at(weight);
    math::matrix<T>& y = at(out);

    if(x.M != y.M || x.N != y.N)
        throw backend_error("rms_norm: shapes differ");

    if(w.M != x.M || w.N != 1)
        throw backend_error("rms_norm: the weight must be one column of rows entries");

    for(uint c = 0; c < x.N; c++) {
        double ss = 0;

        for(uint r = 0; r < x.M; r++)
            ss += double(x(r,c)) * double(x(r,c));

        const double inv = 1.0 / std::sqrt(ss / double(x.M) + double(eps));

        for(uint r = 0; r < x.M; r++)
            y(r,c) = T(double(x(r,c)) * inv * double(w(r,0)));
    }
}

}
}

#endif // JLIB_AI_BACKEND_HH
