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

// Written once and instantiated per element type.  MSL has templates and
// [[host_name]], so one source serves float and half and the host looks the
// right one up by name.  The arithmetic is in float whatever T is: a half
// computed in half is no more accurate than one computed in float and
// rounded, and exp() in half is not obviously either.

template<typename T>
kernel void k_activate(device const T* in [[buffer(0)]],
                       device T* out [[buffer(1)]],
                       constant uint& kind [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) out[i] = T(apply(kind, float(in[i])));
}

template<typename T>
kernel void k_slope(device const T* in [[buffer(0)]],
                    device T* out [[buffer(1)]],
                    constant uint& kind [[buffer(2)]],
                    constant uint& n [[buffer(3)]],
                    uint i [[thread_position_in_grid]])
{
    if(i < n) out[i] = T(slope_of(kind, float(in[i])));
}

template<typename T>
kernel void k_hadamard(device const T* a [[buffer(0)]],
                       device const T* b [[buffer(1)]],
                       device T* c [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) c[i] = T(float(a[i]) * float(b[i]));
}

template<typename T>
kernel void k_subtract(device const T* a [[buffer(0)]],
                       device const T* b [[buffer(1)]],
                       device T* c [[buffer(2)]],
                       constant uint& n [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
    if(i < n) c[i] = T(float(a[i]) - float(b[i]));
}

template<typename T>
kernel void k_add_scaled(device const T* x [[buffer(0)]],
                         device T* y [[buffer(1)]],
                         constant float& alpha [[buffer(2)]],
                         constant uint& n [[buffer(3)]],
                         uint i [[thread_position_in_grid]])
{
    if(i < n) y[i] = T(float(y[i]) + alpha * float(x[i]));
}

#define INSTANTIATE(NAME, T, SUFFIX)                                        \
    template [[host_name(#NAME SUFFIX)]] kernel void NAME<T>

INSTANTIATE(k_activate, float, "_f32")(device const float*, device float*,
                                       constant uint&, constant uint&, uint);
INSTANTIATE(k_activate, half, "_f16")(device const half*, device half*,
                                      constant uint&, constant uint&, uint);
INSTANTIATE(k_slope, float, "_f32")(device const float*, device float*,
                                    constant uint&, constant uint&, uint);
INSTANTIATE(k_slope, half, "_f16")(device const half*, device half*,
                                   constant uint&, constant uint&, uint);
INSTANTIATE(k_hadamard, float, "_f32")(device const float*, device const float*,
                                       device float*, constant uint&, uint);
INSTANTIATE(k_hadamard, half, "_f16")(device const half*, device const half*,
                                      device half*, constant uint&, uint);
INSTANTIATE(k_subtract, float, "_f32")(device const float*, device const float*,
                                       device float*, constant uint&, uint);
INSTANTIATE(k_subtract, half, "_f16")(device const half*, device const half*,
                                      device half*, constant uint&, uint);
INSTANTIATE(k_add_scaled, float, "_f32")(device const float*, device float*,
                                         constant float&, constant uint&, uint);
INSTANTIATE(k_add_scaled, half, "_f16")(device const half*, device half*,
                                        constant float&, constant uint&, uint);
)METAL";

/** The per-type details: what MPS calls it, and what the kernels are named. */
template<typename T> struct traits;

template<> struct traits<float> {
    static MPSDataType mps() { return MPSDataTypeFloat32; }
    static const char* suffix() { return "_f32"; }
};

template<> struct traits<_Float16> {
    static MPSDataType mps() { return MPSDataTypeFloat16; }
    static const char* suffix() { return "_f16"; }
};

}

// ------------------------------------------------------------------ tensor

template<typename T>
struct tensor<T>::impl {
    id<MTLBuffer> buf = nil;
};

template<typename T>
tensor<T>::tensor(std::shared_ptr<device> d, unsigned int rows, unsigned int cols)
    : m_device(d),
      m_rows(rows),
      m_cols(cols),
      m_impl(new impl)
{
    if(!d)
        throw exception("no device");

    const NSUInteger bytes = (NSUInteger)rows * cols * sizeof(T);

    // Never zero: Metal refuses a zero-length buffer, and a 0xN tensor is a
    // legitimate thing to carry around even though nothing reads it.
    m_impl->buf = [d->m_impl->gpu newBufferWithLength:(bytes ? bytes : sizeof(T))
                                              options:MTLResourceStorageModeShared];

    if(m_impl->buf == nil)
        throw exception("could not allocate a device buffer");
}

template<typename T>
tensor<T>::tensor(std::shared_ptr<device> d, const math::matrix<T>& m)
    : tensor(d, m.M, m.N)
{
    write(m);
}

template<typename T>
tensor<T>::~tensor() = default;

template<typename T>
tensor<T>::tensor(tensor&&) = default;

template<typename T>
tensor<T>& tensor<T>::operator=(tensor&&) = default;

template<typename T>
math::matrix<T> tensor<T>::read() const {
    math::matrix<T> out(m_rows, m_cols);

    if(size() == 0)
        return out;

    std::memcpy(static_cast<math::buffer<T> >(out).data(),
                [m_impl->buf contents], size() * sizeof(T));

    return out;
}

template<typename T>
void tensor<T>::write(const math::matrix<T>& m) {
    if(m.M != m_rows || m.N != m_cols) {
        std::ostringstream o;
        o << "cannot write [" << m.M << "," << m.N << "] into ["
          << m_rows << "," << m_cols << "]";
        throw exception(o.str());
    }

    if(size() == 0)
        return;

    std::memcpy([m_impl->buf contents],
                static_cast<const math::buffer<T> >(m).data(),
                size() * sizeof(T));
}

// ------------------------------------------------------------------ stream

template<typename T>
struct stream<T>::impl {
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

struct pipelines {
    id<MTLComputePipelineState> activate = nil;
    id<MTLComputePipelineState> slope = nil;
    id<MTLComputePipelineState> hadamard = nil;
    id<MTLComputePipelineState> subtract = nil;
    id<MTLComputePipelineState> add_scaled = nil;
};

/**
 * The compiled kernels for one element type, built once and kept.
 *
 * The library is compiled once for the process -- it holds every
 * instantiation -- and the pipeline states are per type, which is why this is
 * a template with its own static.  Measured at 0.8 to 2.6 ms for the compile,
 * once; see the note above KERNELS.
 */
id<MTLLibrary> library(id<MTLDevice> gpu) {
    static id<MTLLibrary> lib = nil;

    if(lib != nil)
        return lib;

    NSError* err = nil;

    lib = [gpu newLibraryWithSource:[NSString stringWithUTF8String:KERNELS]
                            options:nil
                              error:&err];

    if(lib == nil) {
        const char* what = (err != nil)
            ? [[err localizedDescription] UTF8String] : "unknown";
        throw ai::backend_error(std::string("could not compile the kernels: ") + what);
    }

    return lib;
}

template<typename T>
pipelines& compiled(id<MTLDevice> gpu) {
    static pipelines p;
    static bool done = false;

    if(done)
        return p;

    id<MTLLibrary> lib = library(gpu);

    NSError* err = nil;

    struct { const char* base; __strong id<MTLComputePipelineState>* into; } wanted[] = {
        { "k_activate",   &p.activate },
        { "k_slope",      &p.slope },
        { "k_hadamard",   &p.hadamard },
        { "k_subtract",   &p.subtract },
        { "k_add_scaled", &p.add_scaled },
    };

    for(auto& w : wanted) {
        const std::string name = std::string(w.base) + traits<T>::suffix();

        id<MTLFunction> fn =
            [lib newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]];

        if(fn == nil)
            throw ai::backend_error("no kernel called " + name);

        *w.into = [gpu newComputePipelineStateWithFunction:fn error:&err];

        if(*w.into == nil)
            throw ai::backend_error("could not build a pipeline for " + name);
    }

    done = true;

    return p;
}

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

template<typename T>
void same_shape(const tensor<T>& a, const tensor<T>& b, const char* what) {
    if(a.rows() != b.rows() || a.cols() != b.cols()) {
        std::ostringstream o;
        o << what << ": [" << a.rows() << "," << a.cols() << "] against ["
          << b.rows() << "," << b.cols() << "]";
        throw ai::backend_error(o.str());
    }
}

}

template<typename T>
stream<T>::stream(std::shared_ptr<device> d)
    : m_device(d),
      m_impl(new impl)
{
    if(!d)
        throw exception("no device");

    pipelines& p = compiled<T>(d->m_impl->gpu);

    m_impl->activate = p.activate;
    m_impl->slope = p.slope;
    m_impl->hadamard = p.hadamard;
    m_impl->subtract = p.subtract;
    m_impl->add_scaled = p.add_scaled;
}

template<typename T>
stream<T>::~stream() {
    // Anything encoded and never waited on is abandoned rather than run: a
    // stream going out of scope unfinished means the caller changed its mind
    // or is unwinding, and neither wants the GPU touching those buffers after
    // the tensors have gone.
    if(m_impl->enc != nil)
        [m_impl->enc endEncoding];
}

template<typename T>
unsigned int stream<T>::pending() const { return m_impl->pending; }

template<typename T>
void stream<T>::open() {
    if(m_impl->cmd == nil)
        m_impl->cmd = [m_device->m_impl->queue commandBuffer];

    if(m_impl->enc == nil)
        m_impl->enc = [m_impl->cmd computeCommandEncoder];
}

template<typename T>
void stream<T>::close() {
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

template<typename T>
void stream<T>::activate(metal::activation kind, const tensor<T>& in, tensor<T>& out) {
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

template<typename T>
void stream<T>::slope(metal::activation kind, const tensor<T>& out_of_layer,
                      tensor<T>& out)
{
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

template<typename T>
void stream<T>::hadamard(const tensor<T>& a, const tensor<T>& b, tensor<T>& c) {
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

template<typename T>
void stream<T>::subtract(const tensor<T>& a, const tensor<T>& b, tensor<T>& c) {
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

template<typename T>
void stream<T>::add_scaled(float alpha, const tensor<T>& x, tensor<T>& y) {
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
template<typename T>
void encode_gemm(id<MTLCommandBuffer> cmd, id<MTLDevice> gpu,
                 id<MTLBuffer> ba, unsigned int arows, unsigned int acols, bool ta,
                 id<MTLBuffer> bb, unsigned int brows, unsigned int bcols, bool tb,
                 id<MTLBuffer> bc, unsigned int crows, unsigned int ccols,
                 float alpha, float beta)
{
    const NSUInteger M = crows, N = ccols;
    const NSUInteger K = ta ? arows : acols;
    const MPSDataType dt = traits<T>::mps();

    MPSMatrixDescriptor* da =
        [MPSMatrixDescriptor matrixDescriptorWithRows:acols columns:arows
                                             rowBytes:arows * sizeof(T)
                                             dataType:dt];
    MPSMatrixDescriptor* db =
        [MPSMatrixDescriptor matrixDescriptorWithRows:bcols columns:brows
                                             rowBytes:brows * sizeof(T)
                                             dataType:dt];
    MPSMatrixDescriptor* dc =
        [MPSMatrixDescriptor matrixDescriptorWithRows:ccols columns:crows
                                             rowBytes:crows * sizeof(T)
                                             dataType:dt];

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

template<typename T>
void stream<T>::multiply(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                         float alpha, float beta)
{
    if(a.cols() != b.rows() || c.rows() != a.rows() || c.cols() != b.cols())
        throw exception("multiply: shapes do not meet");

    close();

    encode_gemm<T>(m_impl->cmd, m_device->m_impl->gpu,
                   a.m_impl->buf, a.rows(), a.cols(), false,
                   b.m_impl->buf, b.rows(), b.cols(), false,
                   c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

template<typename T>
void stream<T>::multiply_tn(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                            float alpha, float beta)
{
    if(a.rows() != b.rows() || c.rows() != a.cols() || c.cols() != b.cols())
        throw exception("multiply_tn: shapes do not meet");

    close();

    encode_gemm<T>(m_impl->cmd, m_device->m_impl->gpu,
                   a.m_impl->buf, a.rows(), a.cols(), true,
                   b.m_impl->buf, b.rows(), b.cols(), false,
                   c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

template<typename T>
void stream<T>::multiply_nt(const tensor<T>& a, const tensor<T>& b, tensor<T>& c,
                            float alpha, float beta)
{
    if(a.cols() != b.cols() || c.rows() != a.rows() || c.cols() != b.rows())
        throw exception("multiply_nt: shapes do not meet");

    close();

    encode_gemm<T>(m_impl->cmd, m_device->m_impl->gpu,
                   a.m_impl->buf, a.rows(), a.cols(), false,
                   b.m_impl->buf, b.rows(), b.cols(), true,
                   c.m_impl->buf, c.rows(), c.cols(), alpha, beta);

    m_impl->pending++;
}

template<typename T>
void stream<T>::wait() {
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

// Objective-C++ cannot be a template header, so the instantiations live here.
// Adding bfloat would be a third line, a third kernel name, and nothing else.
template class tensor<float>;
template class tensor<_Float16>;
template class stream<float>;
template class stream<_Float16>;

}
}
