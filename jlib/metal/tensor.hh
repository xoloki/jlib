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

#ifndef JLIB_METAL_TENSOR_HH
#define JLIB_METAL_TENSOR_HH

#include <jlib/metal/device.hh>

#include <jlib/math/matrix.hh>

#include <jlib/ai/backend.hh>

#include <memory>
#include <string>

namespace jlib {
namespace metal {

/**
 * Which nonlinearity a kernel applies.
 *
 * Deliberately its own enum rather than ai::activation.  jlib/metal builds
 * after jlib/ai and could name it, but a GPU backend depending on the neural
 * library is the wrong way round: this layer knows about numbers, not about
 * networks.  Four enumerators is a cheap duplication and the values are
 * asserted equal in the test.
 */
// Order matters: metal::backend casts ai::activation straight across, so these
// two enumerations have to stay in the same order as well as agree by name.
enum class activation { sigmoid, tanh, relu, leaky_relu, silu };

/** The slope a leaky ReLU keeps below zero.  Matches ai::LEAK. */
inline constexpr float LEAK = 0.01f;

/**
 * The element types this module supports.
 *
 * float and _Float16, and nothing else.  Metal Shading Language has no double
 * -- the compiler refuses it in as many words -- so metal::tensor<double> must
 * fail with something a reader can act on rather than a page of template
 * errors.  bfloat compiles in MSL too and is the better type for inference;
 * adding it is a third instantiation and a third kernel name, not a design
 * change.
 *
 * _Float16 is a compiler extension rather than standard C++: clang and gcc
 * both have it, C23 standardises it, C++ has not yet.  Verified working with
 * math::matrix on Apple clang arm64 and gcc 13 x86_64.
 */
template<typename T> struct supported { static constexpr bool value = false; };
template<> struct supported<float> { static constexpr bool value = true; };
template<> struct supported<_Float16> { static constexpr bool value = true; };

/**
 * A matrix that lives on the GPU.
 *
 * The whole reason this exists.  gemm.hh takes math::matrix, so every call
 * uploads its operands, computes, waits, and downloads the result -- and a
 * training step makes about eight of those, so the data crosses the boundary
 * sixteen times per step to do work that never needed to leave.  Profiling a
 * Metal-accelerated network showed the time inside matrix_multiply rather than
 * in the arithmetic around it, which is what that costs.
 *
 * A tensor is uploaded once and stays.  read() is deliberately explicit and
 * deliberately awkward: it is the expensive thing, and it should look like it.
 *
 * Column-major, matching math::matrix -- see stream::multiply for why that
 * costs nothing.
 */
/**
 * A q8_0 weight living on the device, holding the file's bytes unchanged.
 *
 * Separate from tensor<T> because it is not one: there is no element type to
 * template on, the storage is blocks rather than values, and the only thing
 * that can be done with it is to be the left operand of a multiply_tn. Keeping
 * it a different type means nothing can pass it where a tensor is wanted and
 * get silently wrong arithmetic.
 */
class qweight {
public:
    /**
     * @param rows the contiguous dimension, which for a GGUF weight is its
     *        input width
     * @param blocks 34 bytes per 32 values, exactly as a file holds them
     */
    qweight(std::shared_ptr<device> d, unsigned int rows, unsigned int cols,
            const void* blocks, std::size_t bytes);

    ~qweight();

    unsigned int rows() const { return m_rows; }
    unsigned int cols() const { return m_cols; }

    /** How many bytes it occupies on the device. */
    std::size_t bytes() const { return m_bytes; }

private:
    struct impl;

    std::shared_ptr<device> m_device;
    std::shared_ptr<impl> m_impl;

    unsigned int m_rows = 0;
    unsigned int m_cols = 0;
    std::size_t m_bytes = 0;

    template<typename U> friend class stream;
};

template<typename T>
class tensor {
    static_assert(supported<T>::value,
                  "metal::tensor supports float and _Float16.  Metal Shading "
                  "Language has no double, so there is no fp64 path to add.");

public:
    typedef ai::backend_error exception;

    /** Uninitialised, on the device. */
    tensor(std::shared_ptr<device> d, unsigned int rows, unsigned int cols);

    /** Uploaded from the host. */
    tensor(std::shared_ptr<device> d, const math::matrix<T>& m);

    ~tensor();

    tensor(const tensor&) = delete;
    tensor& operator=(const tensor&) = delete;

    tensor(tensor&&);
    tensor& operator=(tensor&&);

    unsigned int rows() const { return m_rows; }
    unsigned int cols() const { return m_cols; }
    unsigned int size() const { return m_rows * m_cols; }

    /**
     * Copy back to the host.
     *
     * Only correct after the work that wrote it has completed -- see
     * stream::wait().  Reading a tensor a stream is still building is a race,
     * and nothing here can detect it for you.
     */
    math::matrix<T> read() const;

    /** Copy in from the host, overwriting. */
    void write(const math::matrix<T>& m);

private:
    std::shared_ptr<device> m_device;

    unsigned int m_rows = 0;
    unsigned int m_cols = 0;

    struct impl;
    std::unique_ptr<impl> m_impl;

    template<typename> friend class stream;
};

/**
 * A sequence of GPU work, encoded once and waited on once.
 *
 * This is the point of the module.  Every operation here encodes into the same
 * command buffer, so a whole forward and backward pass costs one dispatch and
 * one synchronisation rather than one of each per multiply.
 *
 * Nothing has happened until wait() returns.  A tensor read before that holds
 * whatever it held before, which is the one sharp edge in the design and the
 * price of not stalling between every operation.
 */
template<typename T>
class stream {
    static_assert(supported<T>::value, "metal::stream supports float and _Float16");

public:
    typedef ai::backend_error exception;

    explicit stream(std::shared_ptr<device> d = device::shared());

    ~stream();

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /** c = alpha * a * b + beta * c. */
    void multiply(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                  float alpha = 1.0f, float beta = 0.0f);

    /** c = alpha * a^T * b + beta * c, without materialising the transpose. */
    void multiply_tn(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                     float alpha = 1.0f, float beta = 0.0f);

    /** c = alpha * a * b^T + beta * c. */
    void multiply_nt(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                     float alpha = 1.0f, float beta = 0.0f);

    /** out = f(in). */
    void activate(activation kind, const tensor<T>& in, tensor<T>& out);

    /** out = f'(x), taken from f's own output.  See ai::activation. */
    void slope(activation kind, const tensor<T>& out_of_layer, tensor<T>& out);

    /** c = a * b, elementwise. */
    void hadamard(const tensor<T>& a, const tensor<T>& b, tensor<T>& c);

    /** c = a - b. */
    void subtract(const tensor<T>& a, const tensor<T>& b, tensor<T>& c);

    /** y += alpha * x. */
    void add_scaled(float alpha, const tensor<T>& x, tensor<T>& y);

    /** Softmax down each column, with the column's maximum subtracted. */
    void softmax(const tensor<T>& in, tensor<T>& out);

    /** Mask below the diagonal, offset by how many keys precede the queries. */
    void causal_mask(tensor<T>& s, unsigned int key_offset);

    /** Copy src's columns into dst starting at a column. */
    void copy_columns(const tensor<T>& src, tensor<T>& dst,
                      unsigned int dst_first);

    /** Rotary position embedding, in place; see ai::backend::rope. */
    void rope(tensor<T>& x, unsigned int base_pos, float theta, bool split);

    /** Column gather: out[:,i] = table[:,ids[i]]. */
    void gather(const tensor<T>& table, const std::vector<int>& ids,
                tensor<T>& out);

    /** y = alpha * W^T x + beta * y, with W held quantised. */
    void multiply_tn(const qweight& w, const tensor<T>& x, tensor<T>& y,
                     float alpha, float beta);

    /** RMS normalisation down each column, scaled by a per-row weight. */
    void rms_norm(const tensor<T>& in, const tensor<T>& weight, tensor<T>& out,
                  float eps = 1e-5f);

    /** Commit everything encoded so far and block until the GPU is done. */
    void wait();

    /** How many operations are waiting.  Zero after wait(). */
    unsigned int pending() const;

private:
    /** Open a compute encoder if there is not one already. */
    void open();

    /** Close any open compute encoder, for an op that encodes directly. */
    void close();

    std::shared_ptr<device> m_device;

    struct impl;
    std::unique_ptr<impl> m_impl;
};

}
}

#endif // JLIB_METAL_TENSOR_HH
