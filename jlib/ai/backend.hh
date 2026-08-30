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
enum class activation { sigmoid, tanh, relu, leaky_relu };

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
            }
        }
    }

    return out;
}

template<typename T>
math::matrix<T> slope_matrix(activation a, const math::matrix<T>& out) {
    typedef typename compute_in<T>::type C;

    math::matrix<T> d(out.M, out.N);

    for(uint r = 0; r < out.M; r++) {
        for(uint c = 0; c < out.N; c++) {
            const C s = C(out(r, c));

            switch(a) {
            case activation::sigmoid:    d(r,c) = T(s * (C(1) - s));      break;
            case activation::tanh:       d(r,c) = T(C(1) - s * s);        break;
            case activation::relu:       d(r,c) = T((s > 0) ? C(1) : C(0)); break;
            case activation::leaky_relu: d(r,c) = T((s > 0) ? C(1) : C(LEAK)); break;
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
