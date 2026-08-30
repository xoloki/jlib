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
enum class activation { sigmoid, tanh, relu, leaky_relu };

/** The slope a leaky ReLU keeps below zero.  Matches ai::LEAK. */
inline constexpr float LEAK = 0.01f;

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
class tensor {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::metal::tensor exception" + (msg.empty() ? "" : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /** Uninitialised, on the device. */
    tensor(std::shared_ptr<device> d, unsigned int rows, unsigned int cols);

    /** Uploaded from the host. */
    tensor(std::shared_ptr<device> d, const math::matrix<float>& m);

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
    math::matrix<float> read() const;

    /** Copy in from the host, overwriting. */
    void write(const math::matrix<float>& m);

private:
    std::shared_ptr<device> m_device;

    unsigned int m_rows = 0;
    unsigned int m_cols = 0;

    struct impl;
    std::unique_ptr<impl> m_impl;

    friend class stream;
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
class stream {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::metal::stream exception" + (msg.empty() ? "" : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    explicit stream(std::shared_ptr<device> d = device::shared());

    ~stream();

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    /** c = alpha * a * b + beta * c. */
    void multiply(const tensor& a, const tensor& b, tensor& c,
                  float alpha = 1.0f, float beta = 0.0f);

    /** c = alpha * a^T * b + beta * c, without materialising the transpose. */
    void multiply_tn(const tensor& a, const tensor& b, tensor& c,
                     float alpha = 1.0f, float beta = 0.0f);

    /** c = alpha * a * b^T + beta * c. */
    void multiply_nt(const tensor& a, const tensor& b, tensor& c,
                     float alpha = 1.0f, float beta = 0.0f);

    /** out = f(in). */
    void activate(activation kind, const tensor& in, tensor& out);

    /** out = f'(x), taken from f's own output.  See ai::activation. */
    void slope(activation kind, const tensor& out_of_layer, tensor& out);

    /** c = a * b, elementwise. */
    void hadamard(const tensor& a, const tensor& b, tensor& c);

    /** c = a - b. */
    void subtract(const tensor& a, const tensor& b, tensor& c);

    /** y += alpha * x. */
    void add_scaled(float alpha, const tensor& x, tensor& y);

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
