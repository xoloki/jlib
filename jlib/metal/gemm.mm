/* -*- mode: ObjC++ c-basic-offset: 4 -*-
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

#include <jlib/metal/gemm.hh>
#include <jlib/metal/impl.hh>

#include <cstring>
#include <sstream>

namespace jlib {
namespace metal {

struct matrix_multiply::impl {
    std::shared_ptr<device> dev;

    // The kernel is built for a shape and reused while the shape holds, which
    // is the whole reason this is an object.  A training loop multiplies the
    // same sizes thousands of times.
    MPSMatrixMultiplication* kernel = nil;

    NSUInteger rows = 0, columns = 0, interior = 0;
    double alpha = 0, beta = 0;
};

matrix_multiply::matrix_multiply(std::shared_ptr<device> d)
    : m_impl(new impl)
{
    if(!d)
        throw exception("no device");

    m_impl->dev = d;
}

matrix_multiply::~matrix_multiply() = default;

void matrix_multiply::operator()(const math::matrix<float>& a,
                                 const math::matrix<float>& b,
                                 math::matrix<float>& c,
                                 float alpha, float beta)
{
    if(a.N != b.M || c.M != a.M || c.N != b.N) {
        std::ostringstream o;
        o << "cannot multiply [" << a.M << "," << a.N << "] by ["
          << b.M << "," << b.N << "] into [" << c.M << "," << c.N << "]";
        throw exception(o.str());
    }

    const NSUInteger M = a.M, K = a.N, N = b.N;

    if(M == 0 || K == 0 || N == 0)
        return;

    // math::matrix is column-major -- element (r,c) lives at rep[c * M + r],
    // which the header says is so the data can go to GL without copying.  MPS
    // is row-major.  Rather than transpose anything, read each buffer as the
    // row-major matrix it already is: a column-major MxN read row-major as
    // NxM *is* that matrix's transpose, for free.
    //
    // So with Ar = A^T (K x M) and Br = B^T (N x K), the identity
    //
    //     (A B)^T = B^T A^T
    //
    // says Br * Ar is C^T as an N x M row-major matrix -- and C^T read back
    // row-major into a column-major MxN buffer is C.  No transpose, no copy,
    // one multiply with the operands swapped.
    id<MTLDevice> gpu = m_impl->dev->m_impl->gpu;

    const NSUInteger left_rows = N, left_cols = K;    // B^T
    const NSUInteger right_rows = K, right_cols = M;  // A^T
    const NSUInteger out_rows = N, out_cols = M;      // C^T

    const float* pa = static_cast<const math::buffer<float> >(a).data();
    const float* pb = static_cast<const math::buffer<float> >(b).data();
    float* pc = static_cast<math::buffer<float> >(c).data();

    // Copied into shared buffers rather than aliased.  newBufferWithBytesNoCopy
    // would be a genuine zero copy on unified memory, but it requires the
    // pointer to be page aligned and the length a page multiple, and
    // math::array allocates with new T[].  Making math::buffer able to
    // allocate page-aligned is the follow-up that turns this into no copy at
    // all; until then the copy is honest and small.
    const NSUInteger asize = (NSUInteger)a.M * a.N * sizeof(float);
    const NSUInteger bsize = (NSUInteger)b.M * b.N * sizeof(float);
    const NSUInteger csize = (NSUInteger)c.M * c.N * sizeof(float);

    id<MTLBuffer> ba = [gpu newBufferWithBytes:pa length:asize
                                       options:MTLResourceStorageModeShared];
    id<MTLBuffer> bb = [gpu newBufferWithBytes:pb length:bsize
                                       options:MTLResourceStorageModeShared];

    // beta != 0 means C is read as well as written, so it has to go up.
    id<MTLBuffer> bc = (beta != 0.0f)
        ? [gpu newBufferWithBytes:pc length:csize
                          options:MTLResourceStorageModeShared]
        : [gpu newBufferWithLength:csize options:MTLResourceStorageModeShared];

    if(ba == nil || bb == nil || bc == nil)
        throw exception("could not allocate a device buffer");

    MPSMatrixDescriptor* da =
        [MPSMatrixDescriptor matrixDescriptorWithRows:right_rows
                                              columns:right_cols
                                             rowBytes:right_cols * sizeof(float)
                                             dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* db =
        [MPSMatrixDescriptor matrixDescriptorWithRows:left_rows
                                              columns:left_cols
                                             rowBytes:left_cols * sizeof(float)
                                             dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* dc =
        [MPSMatrixDescriptor matrixDescriptorWithRows:out_rows
                                              columns:out_cols
                                             rowBytes:out_cols * sizeof(float)
                                             dataType:MPSDataTypeFloat32];

    MPSMatrix* ma = [[MPSMatrix alloc] initWithBuffer:ba descriptor:da];
    MPSMatrix* mb = [[MPSMatrix alloc] initWithBuffer:bb descriptor:db];
    MPSMatrix* mc = [[MPSMatrix alloc] initWithBuffer:bc descriptor:dc];

    if(m_impl->kernel == nil ||
       m_impl->rows != out_rows || m_impl->columns != out_cols ||
       m_impl->interior != K ||
       m_impl->alpha != alpha || m_impl->beta != beta)
    {
        m_impl->kernel =
            [[MPSMatrixMultiplication alloc] initWithDevice:gpu
                                             transposeLeft:NO
                                            transposeRight:NO
                                                resultRows:out_rows
                                             resultColumns:out_cols
                                           interiorColumns:K
                                                     alpha:alpha
                                                      beta:beta];

        m_impl->rows = out_rows;
        m_impl->columns = out_cols;
        m_impl->interior = K;
        m_impl->alpha = alpha;
        m_impl->beta = beta;
    }

    id<MTLCommandBuffer> cmd = [m_impl->dev->m_impl->queue commandBuffer];

    [m_impl->kernel encodeToCommandBuffer:cmd
                               leftMatrix:mb
                              rightMatrix:ma
                             resultMatrix:mc];

    [cmd commit];

    // Unified memory removes the copy, not the ordering: the GPU's writes are
    // visible when the command buffer completes and not before.
    [cmd waitUntilCompleted];

    if([cmd status] == MTLCommandBufferStatusError)
        throw exception("the command buffer failed");

    std::memcpy(pc, [bc contents], csize);
}

math::matrix<float> matrix_multiply::operator()(const math::matrix<float>& a,
                                                const math::matrix<float>& b)
{
    math::matrix<float> c(a.M, b.N);

    (*this)(a, b, c);

    return c;
}

math::matrix<float> multiply(const math::matrix<float>& a,
                             const math::matrix<float>& b)
{
    matrix_multiply mm;

    return mm(a, b);
}

}
}
