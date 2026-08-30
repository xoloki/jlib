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

#include <jlib/metal/tensor.hh>
#include <jlib/metal/impl.hh>

#include <cstring>
#include <sstream>
#include <vector>

namespace jlib {
namespace metal {

namespace {

/**
 * The elementwise kernels, compiled at load.
 *
 * Source rather than a precompiled .metallib: that would mean .metal files and
 * a compile step in the build, and this is five small kernels.
 *
 * Measured, on an M5: **0.8 to 2.6 ms**, once, on the first stream a process
 * makes.  Every stream after it is free -- the library and the five pipeline
 * states are built once and kept.  The spread is the system's own shader
 * cache warming across runs; the 2.6ms is the coldest observed.
 *
 * So precompiling would buy about a millisecond of startup, which is not a
 * reason to put a shader toolchain in the build.  That changes if the kernel
 * count grows a lot, or if something needs to run where compiling at load is
 * not allowed.
 *
 * The activation codes match jlib::metal::activation, and the test asserts
 * they match jlib::ai::activation too.
 */
const char* const KERNELS = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant float LEAK = 0.01f;

inline float apply(uint kind, float x) {
    switch(kind) {
    case 0: return 1.0f / (1.0f + exp(-x));
    case 1: return tanh(x);
    case 2: return (x > 0.0f) ? x : 0.0f;
    default: return (x > 0.0f) ? x : LEAK * x;
    }
}

inline float slope_of(uint kind, float s) {
    switch(kind) {
    case 0: return s * (1.0f - s);
    case 1: return 1.0f - s * s;
    case 2: return (s > 0.0f) ? 1.0f : 0.0f;
    default: return (s > 0.0f) ? 1.0f : LEAK;
    }
}

kernel void k_activate(device const float* in [[buffer(0)]],
                       device float* out [[buffer(1)]],
                       constant uint& kind [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) out[i] = apply(kind, in[i]);
}

kernel void k_slope(device const float* in [[buffer(0)]],
                    device float* out [[buffer(1)]],
                    constant uint& kind [[buffer(2)]],
                    constant uint& n [[buffer(3)]],
                    uint i [[thread_position_in_grid]])
{
    if(i < n) out[i] = slope_of(kind, in[i]);
}

kernel void k_hadamard(device const float* a [[buffer(0)]],
                       device const float* b [[buffer(1)]],
                       device float* c [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) c[i] = a[i] * b[i];
}

kernel void k_subtract(device const float* a [[buffer(0)]],
                       device const float* b [[buffer(1)]],
                       device float* c [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) c[i] = a[i] - b[i];
}

kernel void k_add_scaled(device const float* x [[buffer(0)]],
                         device float* y [[buffer(1)]],
                         constant float& alpha [[buffer(2)]],
                         constant uint& n [[buffer(3)]],
                         uint i [[thread_position_in_grid]])
{
    if(i < n) y[i] = y[i] + alpha * x[i];
}
)METAL";

}

// ------------------------------------------------------------------ tensor

struct tensor::impl {
    id<MTLBuffer> buf = nil;
};

tensor::tensor(std::shared_ptr<device> d, unsigned int rows, unsigned int cols)
    : m_device(d),
      m_rows(rows),
      m_cols(cols),
      m_impl(new impl)
{
    if(!d)
        throw exception("no device");

    const NSUInteger bytes = (NSUInteger)rows * cols * sizeof(float);

    // Never zero: Metal refuses a zero-length buffer, and a 0xN tensor is a
    // legitimate thing to carry around even though nothing reads it.
    m_impl->buf = [d->m_impl->gpu newBufferWithLength:(bytes ? bytes : sizeof(float))
                                              options:MTLResourceStorageModeShared];

    if(m_impl->buf == nil)
        throw exception("could not allocate a device buffer");
}

tensor::tensor(std::shared_ptr<device> d, const math::matrix<float>& m)
    : tensor(d, m.M, m.N)
{
    write(m);
}

tensor::~tensor() = default;

tensor::tensor(tensor&&) = default;
tensor& tensor::operator=(tensor&&) = default;

math::matrix<float> tensor::read() const {
    math::matrix<float> out(m_rows, m_cols);

    if(size() == 0)
        return out;

    std::memcpy(static_cast<math::buffer<float> >(out).data(),
                [m_impl->buf contents], size() * sizeof(float));

    return out;
}

void tensor::write(const math::matrix<float>& m) {
    if(m.M != m_rows || m.N != m_cols) {
        std::ostringstream o;
        o << "cannot write [" << m.M << "," << m.N << "] into ["
          << m_rows << "," << m_cols << "]";
        throw exception(o.str());
    }

    if(size() == 0)
        return;

    std::memcpy([m_impl->buf contents],
                static_cast<const math::buffer<float> >(m).data(),
                size() * sizeof(float));
}

// ------------------------------------------------------------------ stream

struct stream::impl {
    id<MTLCommandBuffer> cmd = nil;
    id<MTLComputeCommandEncoder> enc = nil;

    id<MTLComputePipelineState> activate = nil;
    id<MTLComputePipelineState> slope = nil;
    id<MTLComputePipelineState> hadamard = nil;
    id<MTLComputePipelineState> subtract = nil;
    id<MTLComputePipelineState> add_scaled = nil;

    unsigned int pending = 0;
};

namespace {

/**
 * The compiled kernels, built once per device and kept.
 *
 * A library and five pipeline states are not free to build and are identical
 * every time, so a process that makes a stream per training step should not
 * pay for them per step.
 */
struct pipelines {
    id<MTLComputePipelineState> activate = nil;
    id<MTLComputePipelineState> slope = nil;
    id<MTLComputePipelineState> hadamard = nil;
    id<MTLComputePipelineState> subtract = nil;
    id<MTLComputePipelineState> add_scaled = nil;
};

pipelines& compiled(id<MTLDevice> gpu) {
    static pipelines p;
    static bool done = false;

    if(done)
        return p;

    NSError* err = nil;

    id<MTLLibrary> lib =
        [gpu newLibraryWithSource:[NSString stringWithUTF8String:KERNELS]
                          options:nil
                            error:&err];

    if(lib == nil) {
        const char* what = (err != nil)
            ? [[err localizedDescription] UTF8String] : "unknown";
        throw stream::exception(std::string("could not compile the kernels: ") + what);
    }

    struct { const char* name; __strong id<MTLComputePipelineState>* into; } wanted[] = {
        { "k_activate",   &p.activate },
        { "k_slope",      &p.slope },
        { "k_hadamard",   &p.hadamard },
        { "k_subtract",   &p.subtract },
        { "k_add_scaled", &p.add_scaled },
    };

    for(auto& w : wanted) {
        id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:w.name]];

        if(fn == nil)
            throw stream::exception(std::string("no kernel called ") + w.name);

        *w.into = [gpu newComputePipelineStateWithFunction:fn error:&err];

        if(*w.into == nil)
            throw stream::exception(std::string("could not build a pipeline for ") + w.name);
    }

    done = true;

    return p;
}

}

stream::stream(std::shared_ptr<device> d)
    : m_device(d),
      m_impl(new impl)
{
    if(!d)
        throw exception("no device");

    pipelines& p = compiled(d->m_impl->gpu);

    m_impl->activate = p.activate;
    m_impl->slope = p.slope;
    m_impl->hadamard = p.hadamard;
    m_impl->subtract = p.subtract;
    m_impl->add_scaled = p.add_scaled;
}

stream::~stream() {
    // Anything encoded and never waited on is abandoned rather than run: a
    // stream going out of scope unfinished means the caller changed its mind
    // or is unwinding, and neither wants the GPU touching those buffers after
    // the tensors have gone.
    if(m_impl->enc != nil)
        [m_impl->enc endEncoding];
}

unsigned int stream::pending() const { return m_impl->pending; }

void stream::open() {
    if(m_impl->cmd == nil)
        m_impl->cmd = [m_device->m_impl->queue commandBuffer];

    if(m_impl->enc == nil)
        m_impl->enc = [m_impl->cmd computeCommandEncoder];
}

void stream::close() {
    // MPS encodes into the command buffer directly rather than into a compute
    // encoder, so an open one has to be ended first.  The next elementwise op
    // opens another.
    if(m_impl->enc != nil) {
        [m_impl->enc endEncoding];
        m_impl->enc = nil;
    }

    if(m_impl->cmd == nil)
        m_impl->cmd = [m_device->m_impl->queue commandBuffer];
}

// -------------------------------------------------------- encoding helpers

namespace {

/** One thread per element, rounded up to the pipeline's preferred width. */
void dispatch(id<MTLComputeCommandEncoder> enc,
              id<MTLComputePipelineState> pipe,
              unsigned int n)
{
    const NSUInteger width = [pipe threadExecutionWidth];
    const NSUInteger groups = (n + width - 1) / width;

    [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
}

void same_shape(const tensor& a, const tensor& b, const char* what) {
    if(a.rows() != b.rows() || a.cols() != b.cols()) {
        std::ostringstream o;
        o << what << ": [" << a.rows() << "," << a.cols() << "] against ["
          << b.rows() << "," << b.cols() << "]";
        throw stream::exception(o.str());
    }
}

}

void stream::activate(activation kind, const tensor& in, tensor& out) {
    same_shape(in, out, "activate");

    open();

    const unsigned int n = in.size();
    const unsigned int k = static_cast<unsigned int>(kind);

    [m_impl->enc setComputePipelineState:m_impl->activate];
    [m_impl->enc setBuffer:in.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&k length:sizeof(k) atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->activate, n);

    m_impl->pending++;
}

void stream::slope(activation kind, const tensor& out_of_layer, tensor& out) {
    same_shape(out_of_layer, out, "slope");

    open();

    const unsigned int n = out_of_layer.size();
    const unsigned int k = static_cast<unsigned int>(kind);

    [m_impl->enc setComputePipelineState:m_impl->slope];
    [m_impl->enc setBuffer:out_of_layer.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&k length:sizeof(k) atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->slope, n);

    m_impl->pending++;
}

void stream::hadamard(const tensor& a, const tensor& b, tensor& c) {
    same_shape(a, b, "hadamard");
    same_shape(a, c, "hadamard");

    open();

    const unsigned int n = a.size();

    [m_impl->enc setComputePipelineState:m_impl->hadamard];
    [m_impl->enc setBuffer:a.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:b.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:c.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->hadamard, n);

    m_impl->pending++;
}

void stream::subtract(const tensor& a, const tensor& b, tensor& c) {
    same_shape(a, b, "subtract");
    same_shape(a, c, "subtract");

    open();

    const unsigned int n = a.size();

    [m_impl->enc setComputePipelineState:m_impl->subtract];
    [m_impl->enc setBuffer:a.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:b.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:c.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->subtract, n);

    m_impl->pending++;
}

void stream::add_scaled(float alpha, const tensor& x, tensor& y) {
    same_shape(x, y, "add_scaled");

    open();

    const unsigned int n = x.size();

    [m_impl->enc setComputePipelineState:m_impl->add_scaled];
    [m_impl->enc setBuffer:x.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:y.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&alpha length:sizeof(alpha) atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->add_scaled, n);

    m_impl->pending++;
}

// ----------------------------------------------------------------- multiply

namespace {

/**
 * MPS is row-major and math::matrix is column-major, so every operand is read
 * as its own transpose and the operands are swapped -- (A B)^T = B^T A^T.  The
 * same argument as gemm.mm, which has it at length; the difference here is
 * that the buffers already live on the device, so there is nothing to copy.
 */
void encode_gemm(id<MTLCommandBuffer> cmd, id<MTLDevice> gpu,
                 id<MTLBuffer> ba, unsigned int arows, unsigned int acols, bool ta,
                 id<MTLBuffer> bb, unsigned int brows, unsigned int bcols, bool tb,
                 id<MTLBuffer> bc, unsigned int crows, unsigned int ccols,
                 float alpha, float beta)
{
    // In the transposed world the left operand is B and the right is A.
    const NSUInteger M = crows, N = ccols;
    const NSUInteger K = ta ? arows : acols;

    MPSMatrixDescriptor* da =
        [MPSMatrixDescriptor matrixDescriptorWithRows:acols columns:arows
                                             rowBytes:arows * sizeof(float)
                                             dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* db =
        [MPSMatrixDescriptor matrixDescriptorWithRows:bcols columns:brows
                                             rowBytes:brows * sizeof(float)
                                             dataType:MPSDataTypeFloat32];
    MPSMatrixDescriptor* dc =
        [MPSMatrixDescriptor matrixDescriptorWithRows:ccols columns:crows
                                             rowBytes:crows * sizeof(float)
                                             dataType:MPSDataTypeFloat32];

    MPSMatrix* ma = [[MPSMatrix alloc] initWithBuffer:ba descriptor:da];
    MPSMatrix* mb = [[MPSMatrix alloc] initWithBuffer:bb descriptor:db];
    MPSMatrix* mc = [[MPSMatrix alloc] initWithBuffer:bc descriptor:dc];

    // Reading column-major as row-major already transposes, so a requested
    // transpose is the *absence* of one in this world and vice versa.
    MPSMatrixMultiplication* k =
        [[MPSMatrixMultiplication alloc] initWithDevice:gpu
                                         transposeLeft:tb
                                        transposeRight:ta
                                            resultRows:N
                                         resultColumns:M
                                       interiorColumns:K
                                                 alpha:alpha
                                                  beta:beta];

    [k encodeToCommandBuffer:cmd leftMatrix:mb rightMatrix:ma resultMatrix:mc];
}

}

void stream::multiply(const tensor& a, const tensor& b, tensor& c,
                      float alpha, float beta)
{
    if(a.cols() != b.rows() || c.rows() != a.rows() || c.cols() != b.cols())
        throw exception("multiply: shapes do not meet");

    close();

    encode_gemm(m_impl->cmd, m_device->m_impl->gpu,
                a.m_impl->buf, a.rows(), a.cols(), false,
                b.m_impl->buf, b.rows(), b.cols(), false,
                c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

void stream::multiply_tn(const tensor& a, const tensor& b, tensor& c,
                         float alpha, float beta)
{
    if(a.rows() != b.rows() || c.rows() != a.cols() || c.cols() != b.cols())
        throw exception("multiply_tn: shapes do not meet");

    close();

    encode_gemm(m_impl->cmd, m_device->m_impl->gpu,
                a.m_impl->buf, a.rows(), a.cols(), true,
                b.m_impl->buf, b.rows(), b.cols(), false,
                c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

void stream::multiply_nt(const tensor& a, const tensor& b, tensor& c,
                         float alpha, float beta)
{
    if(a.cols() != b.cols() || c.rows() != a.rows() || c.cols() != b.rows())
        throw exception("multiply_nt: shapes do not meet");

    close();

    encode_gemm(m_impl->cmd, m_device->m_impl->gpu,
                a.m_impl->buf, a.rows(), a.cols(), false,
                b.m_impl->buf, b.rows(), b.cols(), true,
                c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

void stream::wait() {
    if(m_impl->enc != nil) {
        [m_impl->enc endEncoding];
        m_impl->enc = nil;
    }

    if(m_impl->cmd == nil) {
        m_impl->pending = 0;
        return;
    }

    [m_impl->cmd commit];
    [m_impl->cmd waitUntilCompleted];

    const bool failed = ([m_impl->cmd status] == MTLCommandBufferStatusError);

    m_impl->cmd = nil;
    m_impl->pending = 0;

    if(failed)
        throw exception("the command buffer failed");
}

}
}
