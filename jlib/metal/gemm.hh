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

#ifndef JLIB_METAL_GEMM_HH
#define JLIB_METAL_GEMM_HH

#include <jlib/metal/device.hh>

#include <jlib/math/matrix.hh>

#include <memory>

namespace jlib {
namespace metal {

/**
 * C = alpha * A * B + beta * C, on the GPU.
 *
 * **Float only.**  Metal Shading Language has no double -- the compiler
 * refuses it in as many words -- so there is no `gemm<double>` here and there
 * cannot be one, where jlib/cuda/gemm.hh has one because cuBLAS supports fp64.
 * A caller that needs fp64 stays on the CPU; see math::operator*.
 *
 * That is less of a restriction than it looks for the work this is for.
 * Nothing trains or runs inference in fp64, and an LLM is fp16 or bf16.
 *
 * ## Why an object rather than a function
 *
 * Because the setup is worth keeping. Building the MPS kernel and wrapping
 * the buffers costs more than the multiply for small matrices, so a caller
 * that multiplies the same shapes repeatedly -- which is what a training loop
 * is -- should pay for it once. `operator()` on a live object reuses
 * everything the shape allows.
 *
 * ## Why MPS rather than a kernel of our own
 *
 * MPSMatrixMultiplication is a tuned GEMM that ships with the OS, and a
 * hand-written one would lose to it. There are no .metal files in this
 * library and no shader compilation in the build for exactly that reason.
 * A custom kernel is worth writing when MPS has no equivalent, not before.
 */
class matrix_multiply {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::metal::matrix_multiply exception" +
                (msg.empty() ? "" : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    explicit matrix_multiply(std::shared_ptr<device> d = device::shared());

    ~matrix_multiply();

    matrix_multiply(const matrix_multiply&) = delete;
    matrix_multiply& operator=(const matrix_multiply&) = delete;

    /**
     * C = alpha * A * B + beta * C.
     *
     * Shapes are checked: A is MxK, B is KxN, C is MxN, and a mismatch throws
     * rather than reading past anything.
     *
     * Synchronous -- it waits for the GPU before returning, so C holds the
     * result when it does. Unified memory removes the copy, not the ordering.
     */
    void operator()(const math::matrix<float>& a,
                    const math::matrix<float>& b,
                    math::matrix<float>& c,
                    float alpha = 1.0f, float beta = 0.0f);

    /** C = A * B, sized for the caller. */
    math::matrix<float> operator()(const math::matrix<float>& a,
                                   const math::matrix<float>& b);

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

/** One multiply, for a caller with no loop to amortise the setup over. */
math::matrix<float> multiply(const math::matrix<float>& a,
                             const math::matrix<float>& b);

}
}

#endif // JLIB_METAL_GEMM_HH
