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
    case 3: return (x > 0.0f) ? x : LEAK * x;
    // silu.  Kept off default so that adding another kind cannot silently
    // arrive here as a leaky relu.
    default: return x / (1.0f + exp(-x));
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

// The reductions.  One thread per *column*, looping down the rows.
//
// Parallel across columns and serial within one, which is the right shape for
// what this is for: a batch or a set of attention heads gives many columns,
// and the feature dimension is what a column is.  A threadgroup reduction per
// column would beat it when there are few very tall columns; that is a
// measurement nobody has taken, and this is the version that is obviously
// correct.

template<typename T>
kernel void k_softmax(device const T* in [[buffer(0)]],
                      device T* out [[buffer(1)]],
                      constant uint& rows [[buffer(2)]],
                      constant uint& cols [[buffer(3)]],
                      uint c [[thread_position_in_grid]])
{
    if(c >= cols) return;

    // Column-major, so a column is contiguous.
    device const T* x = in + (ulong)c * rows;
    device T* y = out + (ulong)c * rows;

    // The maximum first.  exp() of a large score overflows and takes the whole
    // column to nan with it; subtracting the column's own maximum puts the
    // largest exponent at zero and changes nothing else.
    float m = -INFINITY;

    for(uint r = 0; r < rows; r++)
        m = max(m, float(x[r]));

    float sum = 0.0f;

    for(uint r = 0; r < rows; r++) {
        const float e = exp(float(x[r]) - m);

        y[r] = T(e);
        sum += e;
    }

    for(uint r = 0; r < rows; r++)
        y[r] = T(float(y[r]) / sum);
}

template<typename T>
kernel void k_rms_norm(device const T* in [[buffer(0)]],
                       device const T* w [[buffer(1)]],
                       device T* out [[buffer(2)]],
                       constant uint& rows [[buffer(3)]],
                       constant uint& cols [[buffer(4)]],
                       constant float& eps [[buffer(5)]],
                       uint c [[thread_position_in_grid]])
{
    if(c >= cols) return;

    device const T* x = in + (ulong)c * rows;
    device T* y = out + (ulong)c * rows;

    // Accumulated in float even when T is half: a sum of squares over a few
    // thousand features overflows fp16 long before the values themselves do.
    float ss = 0.0f;

    for(uint r = 0; r < rows; r++) {
        const float v = float(x[r]);
        ss += v * v;
    }

    const float inv = rsqrt(ss / float(rows) + eps);

    for(uint r = 0; r < rows; r++)
        y[r] = T(float(x[r]) * inv * float(w[r]));
}

/**
 * Causal masking, in place.
 *
 * Row is the key position and column the query position -- the transpose of
 * how attention is usually drawn -- because softmax here reduces down a column,
 * so a column has to be one query's distribution over keys.  A query at i may
 * not see a key at j > i, and j > i is row > column, so what goes is everything
 * strictly *below* the diagonal.  The usual presentation masks above it; this
 * is the same mask seen from the other side.
 *
 * -infinity rather than a large negative number: softmax subtracts the column
 * maximum, so exp(-inf - m) is exactly 0 for any finite m.  A column with no
 * unmasked entry would give -inf - -inf = nan, which causal masking cannot
 * produce -- element (c,c) is always kept, so every column has at least one.
 */
template<typename T>
kernel void k_causal_mask(device T* s [[buffer(0)]],
                          constant uint& rows [[buffer(1)]],
                          constant uint& cols [[buffer(2)]],
                          constant uint& key_offset [[buffer(3)]],
                          constant uint& queries [[buffer(4)]],
                          uint c [[thread_position_in_grid]])
{
    if(c >= cols) return;

    // Which query this column is, when every head's scores sit side by side.
    const uint per_head = queries ? queries : cols;
    const uint i = c % per_head;

    device T* x = s + (ulong)c * rows;

    for(uint r = i + key_offset + 1; r < rows; r++)
        x[r] = T(-INFINITY);
}

/**
 * Every head's scores in one dispatch; see ai::backend::attention_scores.
 *
 * One thread per element of the result. The head index is arithmetic here
 * rather than a separate call, which is the whole point: the per-head loop this
 * replaces was 77% of the time to produce a token, almost all of it the cost of
 * asking rather than of doing.
 */
template<typename T>
kernel void k_attn_scores(device const T* q [[buffer(0)]],
                          device const T* k [[buffer(1)]],
                          device T* s [[buffer(2)]],
                          constant uint& queries [[buffer(3)]],
                          constant uint& keys [[buffer(4)]],
                          constant uint& heads [[buffer(5)]],
                          constant uint& group [[buffer(6)]],
                          constant uint& d_head [[buffer(7)]],
                          constant float& scale [[buffer(8)]],
                          uint gid [[thread_position_in_grid]])
{
    const uint total = keys * queries * heads;

    if(gid >= total) return;

    const uint j = gid % keys;
    const uint rest = gid / keys;
    const uint i = rest % queries;
    const uint h = rest / queries;

    const uint g = h / group;

    device const T* qc = q + (ulong)i * heads * d_head + (ulong)h * d_head;
    device const T* kc = k + (ulong)j * (heads / group) * d_head + (ulong)g * d_head;

    float sum = 0.0f;

    for(uint d = 0; d < d_head; d++)
        sum += float(qc[d]) * float(kc[d]);

    s[(ulong)(h * queries + i) * keys + j] = T(sum * scale);
}

/** The weighted sum over values, every head at once. */
template<typename T>
kernel void k_attn_weighted(device const T* v [[buffer(0)]],
                            device const T* p [[buffer(1)]],
                            device T* out [[buffer(2)]],
                            constant uint& queries [[buffer(3)]],
                            constant uint& keys [[buffer(4)]],
                            constant uint& heads [[buffer(5)]],
                            constant uint& group [[buffer(6)]],
                            constant uint& d_head [[buffer(7)]],
                            uint gid [[thread_position_in_grid]])
{
    const uint total = heads * d_head * queries;

    if(gid >= total) return;

    const uint d = gid % d_head;
    const uint rest = gid / d_head;
    const uint h = rest % heads;
    const uint i = rest / heads;

    const uint g = h / group;
    const uint kv_heads = heads / group;

    device const T* pc = p + (ulong)(h * queries + i) * keys;

    float sum = 0.0f;

    for(uint j = 0; j < keys; j++)
        sum += float(v[(ulong)j * kv_heads * d_head + g * d_head + d]) * float(pc[j]);

    out[(ulong)i * heads * d_head + h * d_head + d] = T(sum);
}

/** Copy src's columns into dst starting at a column; see ai::backend. */
template<typename T>
kernel void k_copy_columns(device const T* src [[buffer(0)]],
                           device T* dst [[buffer(1)]],
                           constant uint& rows [[buffer(2)]],
                           constant uint& n [[buffer(3)]],
                           constant uint& first [[buffer(4)]],
                           uint i [[thread_position_in_grid]])
{
    if(i >= rows * n) return;

    const uint c = i / rows;
    const uint r = i % rows;

    dst[(ulong)(first + c) * rows + r] = src[(ulong)c * rows + r];
}

/**
 * Rotary position embedding, in place.  One thread per (column, plane).
 *
 * The angle is computed in float where the host uses double.  For the sequence
 * lengths anything here runs at the two agree to well under fp16's resolution;
 * at tens of thousands of positions the float angle would start to drift, and
 * the fix then is a precomputed table rather than more precision here.
 */
template<typename T>
kernel void k_rope(device T* x [[buffer(0)]],
                   constant uint& rows [[buffer(1)]],
                   constant uint& cols [[buffer(2)]],
                   constant uint& base_pos [[buffer(3)]],
                   constant float& theta [[buffer(4)]],
                   constant uint& split [[buffer(5)]],
                   constant uint& d_head [[buffer(6)]],
                   uint i [[thread_position_in_grid]])
{
    // Not `half`: that is the fp16 type in MSL, and naming a variable after it
    // fails to compile in a way whose message points at the declaration rather
    // than at the name.
    // One head unless told otherwise: a rotation happens inside a head and
    // never across the boundary between two stacked in the same column.
    const uint dh = d_head ? d_head : rows;
    const uint planes = dh / 2;
    const uint per_col = (rows / dh) * planes;

    if(i >= cols * per_col) return;

    const uint c = i / per_col;
    const uint within = i % per_col;
    const uint head = within / planes;
    const uint j = within % planes;

    const uint base = head * dh;
    const uint a = base + (split ? j : 2 * j);
    const uint b = base + (split ? j + planes : 2 * j + 1);

    const float freq = pow(theta, -2.0f * float(j) / float(dh));
    const float angle = float(base_pos + c) * freq;

    const float co = cos(angle);
    const float si = sin(angle);

    device T* col = x + (ulong)c * rows;

    const float xa = float(col[a]);
    const float xb = float(col[b]);

    col[a] = T(xa * co - xb * si);
    col[b] = T(xa * si + xb * co);
}

/**
 * Column gather: out[:,i] = table[:,ids[i]].
 *
 * One thread per output element rather than per column, because the columns
 * are d_model long and there are only as many of them as there are tokens.
 * Bounds on the ids are checked on the host before this runs -- a kernel has no
 * way to refuse, and an out-of-range column here would read whatever else is in
 * the buffer.
 */
template<typename T>
kernel void k_gather(device const T* table [[buffer(0)]],
                     device T* out [[buffer(1)]],
                     device const int* ids [[buffer(2)]],
                     constant uint& rows [[buffer(3)]],
                     constant uint& n [[buffer(4)]],
                     uint i [[thread_position_in_grid]])
{
    if(i >= rows * n) return;

    const uint c = i / rows;
    const uint r = i % rows;

    out[(ulong)c * rows + r] = table[(ulong)ids[c] * rows + r];
}

/**
 * c = alpha * W^T b + beta * c, with W held as q8_0 exactly as a file wrote it.
 *
 * A block is a two-byte scale then thirty-two signed quants, 34 bytes for 32
 * values, and the file's bytes are used unchanged -- so the scale is read two
 * bytes at a time and reassembled rather than loaded as a half, which at an
 * offset of 34n would be misaligned.
 *
 * One thread per output element. That is enough because the work is bound by
 * reading W, not by arithmetic: it moves 1.06 bytes per parameter where fp16
 * moves 2, and measured against MPS on a 2048-square matrix-vector product it
 * takes 39us to MPS's 68us at about the same bandwidth.
 */
template<typename T>
kernel void k_q8_gemv(device const uchar* w [[buffer(0)]],
                      device const T* x [[buffer(1)]],
                      device T* y [[buffer(2)]],
                      constant uint& K [[buffer(3)]],
                      constant uint& N [[buffer(4)]],
                      constant uint& ncols [[buffer(5)]],
                      constant float& alpha [[buffer(6)]],
                      constant float& beta [[buffer(7)]],
                      uint gid [[thread_position_in_grid]])
{
    const uint j = gid % N;
    const uint c = gid / N;

    if(c >= ncols) return;

    const uint nb = K / 32;

    device const uchar* base = w + (ulong)j * nb * 34;
    device const T* xc = x + (ulong)c * K;

    float sum = 0.0f;

    for(uint b = 0; b < nb; b++) {
        device const uchar* p = base + (ulong)b * 34;

        const ushort bits = ushort(p[0]) | (ushort(p[1]) << 8);
        const float d = float(as_type<half>(bits));

        device const char* q = (device const char*)(p + 2);

        float acc = 0.0f;

        for(uint i = 0; i < 32; i++)
            acc += float(q[i]) * float(xc[b * 32 + i]);

        sum += d * acc;
    }

    const ulong at = (ulong)c * N + j;

    // beta of zero does not read y, which is what BLAS specifies -- and what
    // the host got wrong until a cache left -infinity in an output.
    y[at] = T(beta == 0.0f ? alpha * sum
                           : alpha * sum + beta * float(y[at]));
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
INSTANTIATE(k_softmax, float, "_f32")(device const float*, device float*,
                                      constant uint&, constant uint&, uint);
INSTANTIATE(k_softmax, half, "_f16")(device const half*, device half*,
                                     constant uint&, constant uint&, uint);
INSTANTIATE(k_causal_mask, float, "_f32")(device float*, constant uint&,
                                          constant uint&, constant uint&,
                                          constant uint&, uint);
INSTANTIATE(k_causal_mask, half, "_f16")(device half*, constant uint&,
                                         constant uint&, constant uint&,
                                         constant uint&, uint);
INSTANTIATE(k_attn_scores, float, "_f32")(device const float*, device const float*,
                                          device float*, constant uint&,
                                          constant uint&, constant uint&,
                                          constant uint&, constant uint&,
                                          constant float&, uint);
INSTANTIATE(k_attn_scores, half, "_f16")(device const half*, device const half*,
                                         device half*, constant uint&,
                                         constant uint&, constant uint&,
                                         constant uint&, constant uint&,
                                         constant float&, uint);
INSTANTIATE(k_attn_weighted, float, "_f32")(device const float*, device const float*,
                                            device float*, constant uint&,
                                            constant uint&, constant uint&,
                                            constant uint&, constant uint&, uint);
INSTANTIATE(k_attn_weighted, half, "_f16")(device const half*, device const half*,
                                           device half*, constant uint&,
                                           constant uint&, constant uint&,
                                           constant uint&, constant uint&, uint);
INSTANTIATE(k_copy_columns, float, "_f32")(device const float*, device float*,
                                           constant uint&, constant uint&,
                                           constant uint&, uint);
INSTANTIATE(k_copy_columns, half, "_f16")(device const half*, device half*,
                                          constant uint&, constant uint&,
                                          constant uint&, uint);
INSTANTIATE(k_q8_gemv, float, "_f32")(device const uchar*, device const float*,
                                      device float*, constant uint&,
                                      constant uint&, constant uint&,
                                      constant float&, constant float&, uint);
INSTANTIATE(k_q8_gemv, half, "_f16")(device const uchar*, device const half*,
                                     device half*, constant uint&,
                                     constant uint&, constant uint&,
                                     constant float&, constant float&, uint);
INSTANTIATE(k_gather, float, "_f32")(device const float*, device float*,
                                     device const int*, constant uint&,
                                     constant uint&, uint);
INSTANTIATE(k_gather, half, "_f16")(device const half*, device half*,
                                    device const int*, constant uint&,
                                    constant uint&, uint);
INSTANTIATE(k_rope, float, "_f32")(device float*, constant uint&, constant uint&,
                                   constant uint&, constant float&,
                                   constant uint&, constant uint&, uint);
INSTANTIATE(k_rope, half, "_f16")(device half*, constant uint&, constant uint&,
                                  constant uint&, constant float&,
                                  constant uint&, constant uint&, uint);
INSTANTIATE(k_rms_norm, float, "_f32")(device const float*, device const float*,
                                       device float*, constant uint&,
                                       constant uint&, constant float&, uint);
INSTANTIATE(k_rms_norm, half, "_f16")(device const half*, device const half*,
                                      device half*, constant uint&,
                                      constant uint&, constant float&, uint);
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
    id<MTLComputePipelineState> softmax = nil;
    id<MTLComputePipelineState> causal_mask = nil;
    id<MTLComputePipelineState> copy_columns = nil;
    id<MTLComputePipelineState> rope = nil;
    id<MTLComputePipelineState> gather = nil;
    id<MTLComputePipelineState> q8_gemv = nil;
    id<MTLComputePipelineState> attn_scores = nil;
    id<MTLComputePipelineState> attn_weighted = nil;
    id<MTLComputePipelineState> rms_norm = nil;

    // Buffers made for one encoded operation and needed until the command
    // buffer has run.  Metal does retain what an encoder binds, so this is
    // belt and braces -- but the failure it guards against is a use-after-free
    // that would show up as an occasional wrong answer, which is the worst
    // kind to go looking for later.
    NSMutableArray* held = nil;

    unsigned int pending = 0;
};

namespace {

struct pipelines {
    id<MTLComputePipelineState> activate = nil;
    id<MTLComputePipelineState> slope = nil;
    id<MTLComputePipelineState> hadamard = nil;
    id<MTLComputePipelineState> subtract = nil;
    id<MTLComputePipelineState> add_scaled = nil;
    id<MTLComputePipelineState> softmax = nil;
    id<MTLComputePipelineState> causal_mask = nil;
    id<MTLComputePipelineState> copy_columns = nil;
    id<MTLComputePipelineState> rope = nil;
    id<MTLComputePipelineState> gather = nil;
    id<MTLComputePipelineState> q8_gemv = nil;
    id<MTLComputePipelineState> attn_scores = nil;
    id<MTLComputePipelineState> attn_weighted = nil;
    id<MTLComputePipelineState> rms_norm = nil;
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
        { "k_softmax",    &p.softmax },
        { "k_causal_mask", &p.causal_mask },
        { "k_copy_columns", &p.copy_columns },
        { "k_rope",       &p.rope },
        { "k_gather",     &p.gather },
        { "k_q8_gemv",    &p.q8_gemv },
        { "k_attn_scores", &p.attn_scores },
        { "k_attn_weighted", &p.attn_weighted },
        { "k_rms_norm",   &p.rms_norm },
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
    m_impl->softmax = p.softmax;
    m_impl->causal_mask = p.causal_mask;
    m_impl->copy_columns = p.copy_columns;
    m_impl->rope = p.rope;
    m_impl->gather = p.gather;
    m_impl->q8_gemv = p.q8_gemv;
    m_impl->attn_scores = p.attn_scores;
    m_impl->attn_weighted = p.attn_weighted;
    m_impl->held = [NSMutableArray array];
    m_impl->rms_norm = p.rms_norm;
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

template<typename T>
void stream<T>::softmax(const tensor<T>& in, tensor<T>& out) {
    same_shape(in, out, "softmax");

    open();

    const unsigned int rows = in.rows();
    const unsigned int cols = in.cols();

    [m_impl->enc setComputePipelineState:m_impl->softmax];
    [m_impl->enc setBuffer:in.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:2];
    [m_impl->enc setBytes:&cols length:sizeof(cols) atIndex:3];

    // One thread per column, not per element.
    dispatch(m_impl->enc, m_impl->softmax, cols);

    m_impl->pending++;
}

template<typename T>
void stream<T>::causal_mask(tensor<T>& s, unsigned int key_offset,
                            unsigned int queries)
{
    open();

    const unsigned int rows = s.rows();
    const unsigned int cols = s.cols();

    [m_impl->enc setComputePipelineState:m_impl->causal_mask];
    [m_impl->enc setBuffer:s.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:1];
    [m_impl->enc setBytes:&cols length:sizeof(cols) atIndex:2];
    [m_impl->enc setBytes:&key_offset length:sizeof(key_offset) atIndex:3];
    [m_impl->enc setBytes:&queries length:sizeof(queries) atIndex:4];

    dispatch(m_impl->enc, m_impl->causal_mask, cols);

    m_impl->pending++;
}

template<typename T>
void stream<T>::copy_columns(const tensor<T>& src, tensor<T>& dst,
                             unsigned int dst_first)
{
    if(src.rows() != dst.rows())
        throw typename tensor<T>::exception("copy_columns: the two must be the "
                                            "same height");

    if(dst_first + src.cols() > dst.cols())
        throw typename tensor<T>::exception("copy_columns: the columns would "
                                            "not fit");

    if(src.cols() == 0) return;

    open();

    const unsigned int rows = src.rows();
    const unsigned int n = src.cols();

    [m_impl->enc setComputePipelineState:m_impl->copy_columns];
    [m_impl->enc setBuffer:src.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:dst.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];
    [m_impl->enc setBytes:&dst_first length:sizeof(dst_first) atIndex:4];

    dispatch(m_impl->enc, m_impl->copy_columns, rows * n);

    m_impl->pending++;
}

template<typename T>
void stream<T>::rope(tensor<T>& x, unsigned int base_pos, float theta,
                     bool split, unsigned int d_head)
{
    if(x.rows() % 2)
        throw typename tensor<T>::exception("rope: rotates in planes, so it "
                                            "needs an even number of rows");

    open();

    const unsigned int rows = x.rows();
    const unsigned int cols = x.cols();
    const unsigned int is_split = split ? 1u : 0u;

    [m_impl->enc setComputePipelineState:m_impl->rope];
    [m_impl->enc setBuffer:x.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:1];
    [m_impl->enc setBytes:&cols length:sizeof(cols) atIndex:2];
    [m_impl->enc setBytes:&base_pos length:sizeof(base_pos) atIndex:3];
    [m_impl->enc setBytes:&theta length:sizeof(theta) atIndex:4];
    [m_impl->enc setBytes:&is_split length:sizeof(is_split) atIndex:5];
    [m_impl->enc setBytes:&d_head length:sizeof(d_head) atIndex:6];

    // One thread per rotation plane per column, not per column: d_head is
    // small and the sequence can be long, so this is where the parallelism is.
    dispatch(m_impl->enc, m_impl->rope, cols * (rows / 2));

    m_impl->pending++;
}

template<typename T>
void stream<T>::gather(const tensor<T>& table, const std::vector<int>& ids,
                       tensor<T>& out)
{
    const unsigned int rows = table.rows();
    const unsigned int n = static_cast<unsigned int>(ids.size());

    if(out.rows() != rows || out.cols() != n)
        throw typename tensor<T>::exception("gather: out must be the table's "
                                            "height by the number of ids");

    // Here, where there is somewhere to throw from.
    for(std::size_t i = 0; i < ids.size(); i++) {
        if(ids[i] < 0 || static_cast<unsigned int>(ids[i]) >= table.cols()) {
            std::ostringstream e;

            e << "gather: token id " << ids[i] << " is outside a table of "
              << table.cols();

            throw typename tensor<T>::exception(e.str());
        }
    }

    if(n == 0) return;

    open();

    id<MTLBuffer> idbuf =
        [m_device->m_impl->gpu newBufferWithBytes:ids.data()
                                           length:ids.size() * sizeof(int)
                                          options:MTLResourceStorageModeShared];

    [m_impl->held addObject:idbuf];

    [m_impl->enc setComputePipelineState:m_impl->gather];
    [m_impl->enc setBuffer:table.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:idbuf offset:0 atIndex:2];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:3];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:4];

    dispatch(m_impl->enc, m_impl->gather, rows * n);

    m_impl->pending++;
}

struct qweight::impl {
    id<MTLBuffer> buf = nil;
};

qweight::qweight(std::shared_ptr<device> d, unsigned int rows, unsigned int cols,
                 const void* blocks, std::size_t bytes)
    : m_device(d),
      m_impl(new impl),
      m_rows(rows),
      m_cols(cols),
      m_bytes(bytes)
{
    const std::size_t n = std::size_t(rows) * cols;

    if(n % 32)
        throw std::runtime_error("jlib::metal::qweight: the element count is "
                                 "not a multiple of the block size");

    if(bytes != (n / 32) * 34)
        throw std::runtime_error("jlib::metal::qweight: the bytes do not match "
                                 "the shape");

    m_impl->buf = [d->m_impl->gpu newBufferWithBytes:blocks
                                              length:bytes
                                             options:MTLResourceStorageModeShared];

    if(m_impl->buf == nil)
        throw std::runtime_error("jlib::metal::qweight: could not allocate");
}

qweight::~qweight() {}

template<typename T>
void stream<T>::multiply_tn(const qweight& w, const tensor<T>& x, tensor<T>& y,
                            float alpha, float beta)
{
    const unsigned int K = w.rows();
    const unsigned int N = w.cols();

    if(x.rows() != K)
        throw typename tensor<T>::exception("q8 multiply_tn: the input is not "
                                            "as tall as the weight is wide");

    if(y.rows() != N || y.cols() != x.cols())
        throw typename tensor<T>::exception("q8 multiply_tn: the output shape "
                                            "does not match");

    open();

    const unsigned int ncols = x.cols();

    [m_impl->enc setComputePipelineState:m_impl->q8_gemv];
    [m_impl->enc setBuffer:w.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:x.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:y.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&K length:sizeof(K) atIndex:3];
    [m_impl->enc setBytes:&N length:sizeof(N) atIndex:4];
    [m_impl->enc setBytes:&ncols length:sizeof(ncols) atIndex:5];
    [m_impl->enc setBytes:&alpha length:sizeof(alpha) atIndex:6];
    [m_impl->enc setBytes:&beta length:sizeof(beta) atIndex:7];

    dispatch(m_impl->enc, m_impl->q8_gemv, N * ncols);

    m_impl->pending++;
}

template<typename T>
void stream<T>::attention_scores(const tensor<T>& q, const tensor<T>& k,
                                 tensor<T>& s, unsigned int heads,
                                 unsigned int kv_heads, unsigned int d_head,
                                 float scale)
{
    open();

    const unsigned int queries = q.cols();
    const unsigned int keys = k.cols();
    const unsigned int group = heads / kv_heads;

    [m_impl->enc setComputePipelineState:m_impl->attn_scores];
    [m_impl->enc setBuffer:q.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:k.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:s.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&queries length:sizeof(queries) atIndex:3];
    [m_impl->enc setBytes:&keys length:sizeof(keys) atIndex:4];
    [m_impl->enc setBytes:&heads length:sizeof(heads) atIndex:5];
    [m_impl->enc setBytes:&group length:sizeof(group) atIndex:6];
    [m_impl->enc setBytes:&d_head length:sizeof(d_head) atIndex:7];
    [m_impl->enc setBytes:&scale length:sizeof(scale) atIndex:8];

    dispatch(m_impl->enc, m_impl->attn_scores, keys * queries * heads);

    m_impl->pending++;
}

template<typename T>
void stream<T>::attention_weighted(const tensor<T>& v, const tensor<T>& p,
                                   tensor<T>& out, unsigned int heads,
                                   unsigned int kv_heads, unsigned int d_head)
{
    open();

    const unsigned int keys = v.cols();
    const unsigned int queries = out.cols();
    const unsigned int group = heads / kv_heads;

    [m_impl->enc setComputePipelineState:m_impl->attn_weighted];
    [m_impl->enc setBuffer:v.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:p.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&queries length:sizeof(queries) atIndex:3];
    [m_impl->enc setBytes:&keys length:sizeof(keys) atIndex:4];
    [m_impl->enc setBytes:&heads length:sizeof(heads) atIndex:5];
    [m_impl->enc setBytes:&group length:sizeof(group) atIndex:6];
    [m_impl->enc setBytes:&d_head length:sizeof(d_head) atIndex:7];

    dispatch(m_impl->enc, m_impl->attn_weighted, heads * d_head * queries);

    m_impl->pending++;
}

template<typename T>
void stream<T>::rms_norm(const tensor<T>& in, const tensor<T>& weight,
                         tensor<T>& out, float eps)
{
    same_shape(in, out, "rms_norm");

    if(weight.rows() != in.rows() || weight.cols() != 1)
        throw ai::backend_error("rms_norm: the weight must be one column of rows entries");

    open();

    const unsigned int rows = in.rows();
    const unsigned int cols = in.cols();

    [m_impl->enc setComputePipelineState:m_impl->rms_norm];
    [m_impl->enc setBuffer:in.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:weight.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:3];
    [m_impl->enc setBytes:&cols length:sizeof(cols) atIndex:4];
    [m_impl->enc setBytes:&eps length:sizeof(eps) atIndex:5];

    dispatch(m_impl->enc, m_impl->rms_norm, cols);

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

    [m_impl->held removeAllObjects];

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
