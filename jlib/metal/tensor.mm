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

#include <cstdlib>

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

/**
 * tanh that does not return NaN for a large argument.
 *
 * MSL's tanh appears to be (exp(2x)-1)/(exp(2x)+1): exp(2x) overflows to
 * infinity once x is past about 44, and inf/inf is a NaN.  Measured, this
 * device returns 1 at x=20 and NaN at x=128, and *only for positive* x --
 * negative arguments drive exp(2x) to zero and come out at -1 correctly,
 * which is the asymmetry that identified it.
 *
 * tanh(20) is 1 to within 1e-17, far inside float, so clamping the argument
 * is exact rather than an approximation.  The host's std::tanh has no such
 * problem, so without this the two backends disagree -- which is how it was
 * found: Gemma 2's GeGLU feeds tanh arguments in the hundreds, and every
 * logit came out NaN on the GPU and finite on the CPU.
 */
inline float safe_tanh(float x) {
    return tanh(clamp(x, -20.0f, 20.0f));
}

inline float apply(uint kind, float x) {
    switch(kind) {
    case 0: return 1.0f / (1.0f + exp(-x));
    case 1: return safe_tanh(x);
    case 2: return (x > 0.0f) ? x : 0.0f;
    case 3: return (x > 0.0f) ? x : LEAK * x;
    case 4: return x / (1.0f + exp(-x));                       // silu
    // gelu, tanh approximation -- "gelu_pytorch_tanh", which is what Gemma
    // saw.  Kept off default so that adding another kind cannot silently
    // arrive here as something else.
    default: return 0.5f * x *
                 (1.0f + safe_tanh(0.7978845608028654f *
                                   (x + 0.044715f * x * x * x)));
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

template<typename T>
kernel void k_softcap(device T* x [[buffer(0)]],
                      constant float& cap [[buffer(1)]],
                      constant uint& n [[buffer(2)]],
                      uint i [[thread_position_in_grid]])
{
    // In float whatever T is: the point of the cap is to keep the value in
    // range, so computing it in the range it is escaping would be circular.
    // safe_tanh for the same reason: a logit of 2200 against a cap of 50 is
    // an argument of 44, and that is where MSL's tanh stops being finite.
    if(i < n) x[i] = T(cap * safe_tanh(float(x[i]) / cap));
}

// One thread per element, and the bias index is the row -- so the same value
// is read by every column, which is what a broadcast is.
template<typename T>
kernel void k_add_columns(device const T* bias [[buffer(0)]],
                          device T* y [[buffer(1)]],
                          constant uint& rows [[buffer(2)]],
                          constant uint& n [[buffer(3)]],
                          uint i [[thread_position_in_grid]])
{
    if(i < n) y[i] = T(float(y[i]) + float(bias[i % rows]));
}

// The reductions.  One thread per *column*, looping down the rows.
//
// Parallel across columns and serial within one, which is the right shape for
// what this is for: a batch or a set of attention heads gives many columns,
// and the feature dimension is what a column is.  A threadgroup reduction per
// column would beat it when there are few very tall columns; that is a
// measurement nobody has taken, and this is the version that is obviously
// correct.

/**
 * exp(x - max) / sum, down each column.
 *
 * **A SIMD group to a column.**  One thread meant three serial passes over
 * the column with a dependent load each step and nothing to hide the latency
 * behind -- the same shape that made rms_norm the most expensive kernel in a
 * decode step until #192.
 *
 * Lane L takes rows L, L+32, ...; `simd_max` folds the maximum and `simd_sum`
 * the total.  Each lane reads back only what it wrote, so the second and
 * third passes need no barrier beyond the lockstep the folds already impose.
 *
 * In float whatever T is: exp() of a score that fp16 can hold still overflows
 * fp16, which is what the max subtraction is for in the first place.
 */
template<typename T>
kernel void k_softmax(device const T* in [[buffer(0)]],
                      device T* out [[buffer(1)]],
                      constant uint& rows [[buffer(2)]],
                      constant uint& cols [[buffer(3)]],
                      uint gid [[thread_position_in_grid]],
                      uint lane [[thread_index_in_simdgroup]])
{
    const uint c = gid / 32;

    if(c >= cols) return;

    // Column-major, so a column is contiguous.
    device const T* x = in + (ulong)c * rows;
    device T* y = out + (ulong)c * rows;

    // The maximum first.  exp() of a large score overflows and takes the whole
    // column to nan with it; subtracting the column's own maximum puts the
    // largest exponent at zero and changes nothing else.
    float m = -INFINITY;

    for(uint r = lane; r < rows; r += 32)
        m = max(m, float(x[r]));

    m = simd_max(m);

    float sum = 0.0f;

    for(uint r = lane; r < rows; r += 32) {
        const float e = exp(float(x[r]) - m);

        y[r] = T(e);
        sum += e;
    }

    sum = simd_sum(sum);

    for(uint r = lane; r < rows; r += 32)
        y[r] = T(float(y[r]) / sum);
}

/**
 * x / rms(x) * w, down each column.
 *
 * **A SIMD group to a column, not a thread.**  One thread meant 2048 loads in
 * a dependency chain with nothing to hide the latency behind, and it cost
 * ~200us per call whatever the width -- 260us at one column, 201us at five
 * hundred, because the threads simply overlapped identical stalls.
 *
 * That made it **the largest single cost in a decode step**: 36% of one
 * layer's kernel time on its own, and a layer calls it twice, against 14% for
 * the widest q8 matmul.  A 2048-element normalisation was costing more than
 * an 11.5M-multiply-accumulate matrix product.  See #190.
 *
 * Lane L takes rows L, L+32, ..., `simd_sum` folds the partial sums of
 * squares, and the scaling pass is spread the same way.  The reduction is in
 * float whatever T is: a sum of squares over a few thousand features
 * overflows fp16 long before the values themselves do.
 */
template<typename T>
kernel void k_rms_norm(device const T* in [[buffer(0)]],
                       device const T* w [[buffer(1)]],
                       device T* out [[buffer(2)]],
                       constant uint& rows [[buffer(3)]],
                       constant uint& cols [[buffer(4)]],
                       constant float& eps [[buffer(5)]],
                       uint gid [[thread_position_in_grid]],
                       uint lane [[thread_index_in_simdgroup]])
{
    const uint c = gid / 32;

    if(c >= cols) return;

    device const T* x = in + (ulong)c * rows;
    device T* y = out + (ulong)c * rows;

    float ss = 0.0f;

    for(uint r = lane; r < rows; r += 32) {
        const float v = float(x[r]);

        ss += v * v;
    }

    const float inv = rsqrt(simd_sum(ss) / float(rows) + eps);

    for(uint r = lane; r < rows; r += 32)
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
/**
 * How many columns of x one thread carries.
 *
 * The kernel below reads a weight block once and uses it for this many
 * columns, so the weight traffic falls by this factor.  Eight because the
 * accumulators and the unpacked block have to live in registers: at eight it
 * is 8 floats of accumulator and 32 of block per thread, and going wider
 * starts spilling and costs more than the traffic it saves.
 *
 * One is exactly the old kernel.  Decode passes ncols = 1 and takes the same
 * path it always did, with the tile loop running once.
 */
constant uint Q8_TILE = 8;

/**
 * How many lanes share one output row, fixed when the pipeline is built.
 *
 * A **function constant**, not a kernel argument, and that distinction is the
 * whole of it: passed as an argument the inner loop's stride becomes `b +=
 * lanes` with `lanes` unknown, the compiler stops unrolling, and prefill went
 * from 7.5s to 13.6s for a value that never varies within a dispatch.
 * Specialised here it is a literal, and the two pipelines are built from the
 * same source with 1 and 32.
 */
constant uint Q8_LANES [[function_constant(0)]];

/**
 * q8_0 blocks out to a plain tensor, one block per thread.
 *
 * The layout is already right: a qweight holds K values contiguously per
 * output row, and a (K x N) column-major tensor holds a column contiguously.
 * Output row j *is* column j, so this is a straight unpack with no transpose
 * and no gather.
 *
 * It exists because `k_q8_gemv` is eight times slower than MPS at prefill
 * shapes -- 1.5 TFLOP/s against 13 on the same device and the same matrices
 * (#286) -- and one pass over the weights buys the difference, once the batch
 * is wide enough to spread it over. See stream::multiply_tn.
 */
/**
 * The blocks of the requested columns, copied together, in whatever encoding
 * they are already in.
 *
 * **The size of a block is the whole of what a quantised gather needs to know
 * about the format.** Every layout jlib reads puts its blocks along the
 * contiguous dimension, so a column of a (K x N) table is exactly K/vals
 * whole blocks and a lookup is a run of block copies -- no unpacking, no
 * partial blocks, nothing per-format. That is why there is one of these and
 * not three.
 *
 * What comes out is a (K x ids) table in the same encoding, small enough to
 * unpack with the ordinary kernel afterwards: 512 tokens of Qwen2.5-Coder 7B
 * is 1.8 M values against the table's 545 M.
 *
 * One thread per (column, block), copying a byte at a time -- a q6_K block is
 * 210 bytes, which is not a multiple of four, so nothing wider is safe for
 * all three formats.
 */
kernel void k_qblocks_gather(device const uchar* table [[buffer(0)]],
                             device const int* ids [[buffer(1)]],
                             device uchar* out [[buffer(2)]],
                             constant uint& nb [[buffer(3)]],
                             constant uint& bytes [[buffer(4)]],
                             constant uint& units [[buffer(5)]],
                             uint gid [[thread_position_in_grid]])
{
    if(gid >= units) return;

    const uint i = gid / nb;          // which requested column
    const uint b = gid % nb;          // which block of it

    device const uchar* src = table + ((ulong)ids[i] * nb + b) * bytes;
    device uchar* dst = out + ((ulong)i * nb + b) * bytes;

    for(uint k = 0; k < bytes; k++) dst[k] = src[k];
}

/**
 * The little-endian f16 at `p`, which every quantised block begins or ends
 * with.
 *
 * Through a `ushort` rather than inline, because `a | (b << 8)` promotes to
 * int and `as_type<half>` refuses an int -- in as many words, at compile time.
 */
static inline float half_le(device const uchar* p)
{
    const ushort bits = ushort(p[0]) | ushort(ushort(p[1]) << 8);

    return float(as_type<half>(bits));
}

/**
 * q4_K's sub-block scale and min, unpacked.  ggml's `get_scale_min_k4`.
 *
 * Twelve bytes hold eight six-bit scales and eight six-bit mins: the first
 * four of each in their own byte's low six bits, the last four split, four
 * bits in one byte and the top two borrowed from the high end of an earlier
 * one.  The host reference is `scale_min_k4` in ai/quant.hh and this is a
 * transliteration of it -- if one changes the other has to.
 */
static inline void scale_min_k4(int j, device const uchar* q,
                                thread uchar& d, thread uchar& m)
{
    if(j < 4) {
        d = q[j] & 63;
        m = q[j + 4] & 63;
    }
    else {
        d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        m = (q[j + 4] >>  4) | ((q[j    ] >> 6) << 4);
    }
}

/**
 * A q4_K super-block out to 256 plain values, one block per thread.
 *
 * Affine rather than symmetric: `d*q - m`, with both the scale and the min
 * themselves quantised against a per-super-block f16. That second tier is
 * what buys the accuracy at four bits (#196), and it is the whole difference
 * from q8_0's flat block.
 *
 * The destination index is the same argument as k_q8_dequant's: a qweight
 * holds K values contiguously per output row and a (K x N) column-major
 * tensor holds a column contiguously, so 256 values land contiguously and the
 * offset is the block number times 256.
 */
template<typename T>
kernel void k_q4k_dequant(device const uchar* w [[buffer(0)]],
                          device T* out [[buffer(1)]],
                          constant uint& K [[buffer(2)]],
                          constant uint& units [[buffer(3)]],
                          constant uint& first [[buffer(4)]],
                          uint gid [[thread_position_in_grid]])
{
    if(gid >= units) return;

    // **A thread per sub-block, not per super-block.** One thread per 256
    // values launches eight times fewer threads than k_q8_dequant does for
    // the same weight, each doing eight times the work in sequence -- which
    // cost 1.7x on prefill before this was split. Thirty-two values is also
    // where q4_K's scale changes, so the split is free.
    const uint b  = gid / 8;
    const uint sb = gid % 8;

    device const uchar* p = w + ((ulong)first + b) * 144;

    const float d    = half_le(p);
    const float dmin = half_le(p + 2);

    device const uchar* sc = p + 4;
    device const uchar* q  = p + 16;

    uchar sq = 0, sm = 0;

    scale_min_k4(int(sb), sc, sq, sm);

    const float d1 = d * float(sq), o1 = dmin * float(sm);

    // Group sb/2 holds the low nibbles of its 32 bytes and then the high
    // ones, which is the order dequantise_q4_k writes them in.
    device const uchar* qq = q + (sb / 2) * 32;
    const bool high = (sb & 1) != 0;

    device T* y = out + (ulong)b * 256 + sb * 32;

    for(uint i = 0; i < 32; i++)
        y[i] = T(d1 * float(high ? (qq[i] >> 4) : (qq[i] & 0xF)) - o1);
}

/**
 * A q6_K super-block out to 256 plain values, one block per thread.
 *
 * Symmetric, with six bits stored unsigned meaning [-32, 31] -- which is what
 * the 32 below is. The six bits are split across two arrays: four low bits in
 * `ql` and two high bits packed four-to-a-byte in `qh`.
 *
 * Host reference: `dequantise_q6_k` in ai/quant.hh.
 */
template<typename T>
kernel void k_q6k_dequant(device const uchar* w [[buffer(0)]],
                          device T* out [[buffer(1)]],
                          constant uint& K [[buffer(2)]],
                          constant uint& units [[buffer(3)]],
                          constant uint& first [[buffer(4)]],
                          uint gid [[thread_position_in_grid]])
{
    if(gid >= units) return;

    // Sixteen values a thread, for the same reason as q4_K above and because
    // sixteen is where q6_K's scale changes -- the reference's `is = l / 16`.
    const uint b  = gid / 16;
    const uint sb = gid % 16;

    const uint h = sb / 8;
    const uint g = (sb % 8) / 2;
    const uint k = sb & 1;

    device const uchar* p = w + ((ulong)first + b) * 210;

    device const uchar* ql = p;
    device const uchar* qh = p + 128;
    device const char*  sc = (device const char*)(p + 192);

    const float ds = half_le(p + 208) * float(sc[h * 8 + g * 2 + k]);

    device const uchar* qlp = ql + h * 64 + (g % 2) * 32 + k * 16;
    device const uchar* qhp = qh + h * 32 + k * 16;

    const uint shift = g * 2;
    const bool high = g >= 2;

    device T* y = out + (ulong)b * 256 + h * 128 + g * 32 + k * 16;

    // The 32 is the zero point: six bits stored unsigned mean [-32, 31].
    for(uint i = 0; i < 16; i++) {
        const uint lo = high ? (qlp[i] >> 4) : (qlp[i] & 0xF);

        y[i] = T(ds * float(int(lo | (((qhp[i] >> shift) & 3) << 4)) - 32));
    }
}

template<typename T>
kernel void k_q8_dequant(device const uchar* w [[buffer(0)]],
                         device T* out [[buffer(1)]],
                         constant uint& K [[buffer(2)]],
                         constant uint& blocks [[buffer(3)]],
                         constant uint& first [[buffer(4)]],
                         uint gid [[thread_position_in_grid]])
{
    if(gid >= blocks) return;

    // `first` is where this dispatch's column block starts in the weight;
    // the destination is always indexed from zero, because the scratch holds
    // only the block.  See multiply_tn(qweight...).
    device const uchar* p = w + ((ulong)first + gid) * 34;

    const ushort bits = ushort(p[0]) | (ushort(p[1]) << 8);
    const float d = float(as_type<half>(bits));

    device const char* q = (device const char*)(p + 2);

    // Block `gid` is block `gid % (K/32)` of output row `gid / (K/32)`, and
    // that row is a whole column of the destination -- so the 32 values land
    // contiguously and the index is just the block number times 32.
    device T* o = out + (ulong)gid * 32;

    for(uint i = 0; i < 32; i++) o[i] = T(d * float(q[i]));
}

/**
 * y = alpha * (W^T x) + beta * y, with W held as q8_0 and never expanded.
 *
 * The dequantisation is inside the multiply -- a weight block is 34 bytes,
 * one f16 scale and thirty-two signed quants, and it is turned into floats in
 * registers and used there.  That much is #164's.
 *
 * **The tiling is what makes it a GEMM rather than N GEMVs.**  It was one
 * thread per output element, each streaming a whole weight row from global
 * memory, so every column of x re-read all 1.1 GB of weights: at 2048 columns
 * that is 2.2 TB of traffic where 1.1 GB would do, and prefill cost a flat
 * ~7 ms per token however long the prompt -- the signature of no reuse at all.
 * See #158.
 *
 * Each thread now owns one output row and Q8_TILE columns, so a block is
 * fetched once and spent eight times.
 *
 * ## And a SIMD group per row rather than a thread
 *
 * Tiling fixed prefill and did nothing for decode, because decode's problem
 * is the opposite shape. With one column there is one thread per output row
 * and no more: 2048 for q and o, **256 for k and v**. That is far too little
 * to fill this GPU, and each of those threads walked all K/32 = 64 blocks
 * serially with every iteration waiting on the last.
 *
 * So a row *may* be carried by a SIMD group instead: lane L takes blocks
 * L, L+32, ..., and `simd_sum` folds the partials. Thirty-two times the
 * threads and a dependency chain of two rather than sixty-four.
 *
 * **Only when there are too few rows to fill the machine**, which is what
 * `lanes` selects. Prefill already has one unit per row *per column tile* and
 * needs no help; giving it a reduction as well costs 19-26%. The caller
 * decides and passes 1 or 32.
 *
 * Measured before this: a repacked weight layout and four-wide loads changed
 * nothing at all, which is what said the limit was never the bytes. See #190.
 */
/**
 * q4_K, one thread (or SIMD group) per output row and Q8_TILE columns.
 *
 * The same shape as k_q8_gemv -- see it for why the tiling and the optional
 * SIMD reduction exist -- with two differences that are the format's.
 *
 * **A super-block is unpacked 32 values at a time, not 256.** 256 floats is a
 * kilobyte of registers per thread and would spill; 32 is both what fits and
 * what q4_K's scale granularity actually is, since each of the eight
 * sub-blocks carries its own six-bit scale and min.
 *
 * **The dot product is affine.** A value is `d*q - m`, so its contribution is
 * `d*sum(q*x) - m*sum(x)` and the running sum of x is needed alongside the
 * running product. q8_0 has no analogue of that term.
 */
template<typename T>
kernel void k_q4k_gemv(device const uchar* w [[buffer(0)]],
                       device const T* x [[buffer(1)]],
                       device T* y [[buffer(2)]],
                       constant uint& K [[buffer(3)]],
                       constant uint& N [[buffer(4)]],
                       constant uint& ncols [[buffer(5)]],
                       constant float& alpha [[buffer(6)]],
                       constant float& beta [[buffer(7)]],
                       uint gid [[thread_position_in_grid]],
                       uint sl [[thread_index_in_simdgroup]])
{
    const uint tiles = (ncols + Q8_TILE - 1) / Q8_TILE;

    const uint unit = (Q8_LANES == 1) ? gid : gid / 32;
    const uint lane = (Q8_LANES == 1) ? 0u : sl;

    const uint j = unit % N;
    const uint t = unit / N;

    if(t >= tiles) return;

    const uint c0 = t * Q8_TILE;
    const uint have = min(Q8_TILE, ncols - c0);

    const uint nb = K / 256;

    device const uchar* base = w + (ulong)j * nb * 144;

    float sum[Q8_TILE];

    for(uint i = 0; i < Q8_TILE; i++) sum[i] = 0.0f;

    // **The lanes are spread over sub-blocks, not super-blocks.**
    //
    // A SIMD group shares a row by taking every Q8_LANES'th block, which for
    // q8_0 means 32 lanes over K/32 = 64 blocks at K=2048. A K-quant block is
    // 256 values, so the same loop would be 32 lanes over *eight* blocks and
    // twenty-four of them would do nothing -- measured at 37 GB/s where q8_0
    // manages 155 on the same shape.
    //
    // A sub-block is the right unit: eight per super-block here, so K=2048 is
    // 64 of them and every lane has work again. The super-block header is
    // re-read per sub-block, which is four bytes out of a cache line that the
    // quants themselves are about to need anyway.
    const uint subs = nb * 8;

    for(uint g = lane; g < subs; g += Q8_LANES) {
        const uint b  = g / 8;
        const uint sb = g % 8;

        device const uchar* p = base + (ulong)b * 144;

        const float d    = half_le(p);
        const float dmin = half_le(p + 2);

        device const uchar* sc = p + 4;
        device const uchar* q  = p + 16;

        {
            uchar sq = 0, sm = 0;

            scale_min_k4(int(sb), sc, sq, sm);

            const float d1 = d * float(sq), o1 = dmin * float(sm);

            device const uchar* qq = q + (sb / 2) * 32;
            const bool high = (sb & 1) != 0;

            float qs[32];

            for(uint i = 0; i < 32; i++)
                qs[i] = float(high ? (qq[i] >> 4) : (qq[i] & 0xF));

            for(uint cc = 0; cc < have; cc++) {
                device const T* xc =
                    x + (ulong)(c0 + cc) * K + (ulong)b * 256 + sb * 32;

                float acc = 0.0f, xs = 0.0f;

                for(uint i = 0; i < 32; i++) {
                    const float xi = float(xc[i]);

                    acc += qs[i] * xi;
                    xs  += xi;
                }

                sum[cc] += d1 * acc - o1 * xs;
            }
        }
    }

    for(uint cc = 0; cc < have; cc++) {
        const float total = (Q8_LANES == 1) ? sum[cc] : simd_sum(sum[cc]);

        if(lane != 0) continue;

        const ulong at = (ulong)(c0 + cc) * N + j;

        y[at] = T(beta == 0.0f ? alpha * total
                               : alpha * total + beta * float(y[at]));
    }
}

/**
 * q6_K, one thread (or SIMD group) per output row and Q8_TILE columns.
 *
 * **Sixteen values at a time, because that is q6_K's scale granularity** --
 * sixteen int8 sub-block scales over 256 values. The reference's `is = l / 16`
 * is exactly that boundary.
 *
 * The index arithmetic is the awkward part and is worth stating rather than
 * reading off the shifts. A value's position `v` decomposes as a half
 * `h = v/128`, a group `g = (v%128)/32` and a sixteen `k`, and then
 *
 *     low nibble if g < 2, high otherwise
 *     ql at  h*64 + (g%2)*32 + k*16
 *     qh at  h*32 + k*16, taking two bits at shift g*2
 *     scale  sc[h*8 + g*2 + k]
 *
 * which is `dequantise_q6_k`'s four unrolled cases rewritten as one. The 32
 * is the zero point: six bits are stored unsigned and mean [-32, 31].
 */
template<typename T>
kernel void k_q6k_gemv(device const uchar* w [[buffer(0)]],
                       device const T* x [[buffer(1)]],
                       device T* y [[buffer(2)]],
                       constant uint& K [[buffer(3)]],
                       constant uint& N [[buffer(4)]],
                       constant uint& ncols [[buffer(5)]],
                       constant float& alpha [[buffer(6)]],
                       constant float& beta [[buffer(7)]],
                       uint gid [[thread_position_in_grid]],
                       uint sl [[thread_index_in_simdgroup]])
{
    const uint tiles = (ncols + Q8_TILE - 1) / Q8_TILE;

    const uint unit = (Q8_LANES == 1) ? gid : gid / 32;
    const uint lane = (Q8_LANES == 1) ? 0u : sl;

    const uint j = unit % N;
    const uint t = unit / N;

    if(t >= tiles) return;

    const uint c0 = t * Q8_TILE;
    const uint have = min(Q8_TILE, ncols - c0);

    const uint nb = K / 256;

    device const uchar* base = w + (ulong)j * nb * 210;

    float sum[Q8_TILE];

    for(uint i = 0; i < Q8_TILE; i++) sum[i] = 0.0f;

    // Sixteen sub-blocks per super-block, spread over the lanes for the same
    // reason as q4_K above: one lane per 256 values leaves most of a SIMD
    // group idle.
    const uint subs = nb * 16;

    for(uint g = lane; g < subs; g += Q8_LANES) {
        const uint b  = g / 16;
        const uint sb = g % 16;

        device const uchar* p = base + (ulong)b * 210;

        device const uchar* ql = p;
        device const uchar* qh = p + 128;
        device const char*  sc = (device const char*)(p + 192);

        const float d = half_le(p + 208);

        {
            const uint h = sb / 8;
            const uint g = (sb % 8) / 2;
            const uint k = sb & 1;

            const float ds = d * float(sc[h * 8 + g * 2 + k]);

            device const uchar* qlp = ql + h * 64 + (g % 2) * 32 + k * 16;
            device const uchar* qhp = qh + h * 32 + k * 16;

            const uint shift = g * 2;
            const bool high = g >= 2;

            float qs[16];

            for(uint i = 0; i < 16; i++) {
                const uint lo = high ? (qlp[i] >> 4) : (qlp[i] & 0xF);

                qs[i] = float(int(lo | (((qhp[i] >> shift) & 3) << 4)) - 32);
            }

            for(uint cc = 0; cc < have; cc++) {
                device const T* xc = x + (ulong)(c0 + cc) * K +
                    (ulong)b * 256 + h * 128 + g * 32 + k * 16;

                float acc = 0.0f;

                for(uint i = 0; i < 16; i++) acc += qs[i] * float(xc[i]);

                sum[cc] += ds * acc;
            }
        }
    }

    for(uint cc = 0; cc < have; cc++) {
        const float total = (Q8_LANES == 1) ? sum[cc] : simd_sum(sum[cc]);

        if(lane != 0) continue;

        const ulong at = (ulong)(c0 + cc) * N + j;

        y[at] = T(beta == 0.0f ? alpha * total
                               : alpha * total + beta * float(y[at]));
    }
}

template<typename T>
kernel void k_q8_gemv(device const uchar* w [[buffer(0)]],
                      device const T* x [[buffer(1)]],
                      device T* y [[buffer(2)]],
                      constant uint& K [[buffer(3)]],
                      constant uint& N [[buffer(4)]],
                      constant uint& ncols [[buffer(5)]],
                      constant float& alpha [[buffer(6)]],
                      constant float& beta [[buffer(7)]],
                      uint gid [[thread_position_in_grid]],
                      uint sl [[thread_index_in_simdgroup]])
{
    const uint tiles = (ncols + Q8_TILE - 1) / Q8_TILE;

    // Either one thread to a (row, column-tile), or a whole SIMD group to
    // one.  Uniform across the dispatch, so the branch is free.
    const uint unit = (Q8_LANES == 1) ? gid : gid / 32;
    const uint lane = (Q8_LANES == 1) ? 0u : sl;

    const uint j = unit % N;
    const uint t = unit / N;

    if(t >= tiles) return;

    const uint c0 = t * Q8_TILE;
    const uint have = min(Q8_TILE, ncols - c0);

    const uint nb = K / 32;

    device const uchar* base = w + (ulong)j * nb * 34;

    float sum[Q8_TILE];

    for(uint i = 0; i < Q8_TILE; i++) sum[i] = 0.0f;

    for(uint b = lane; b < nb; b += Q8_LANES) {
        device const uchar* p = base + (ulong)b * 34;

        const ushort bits = ushort(p[0]) | (ushort(p[1]) << 8);
        const float d = float(as_type<half>(bits));

        device const char* q = (device const char*)(p + 2);

        // Unpacked once, here, and then spent on every column in the tile.
        // This array is the whole optimisation: without it each column would
        // go back to global memory for the same thirty-two bytes.
        float qs[32];

        for(uint i = 0; i < 32; i++) qs[i] = float(q[i]);

        for(uint cc = 0; cc < have; cc++) {
            device const T* xc = x + (ulong)(c0 + cc) * K + b * 32;

            float acc = 0.0f;

            for(uint i = 0; i < 32; i++) acc += qs[i] * float(xc[i]);

            sum[cc] += d * acc;
        }
    }

    for(uint cc = 0; cc < have; cc++) {
        // Each lane holds part of this row's dot product when they split it.
        const float total = (Q8_LANES == 1) ? sum[cc] : simd_sum(sum[cc]);

        if(lane != 0) continue;

        const ulong at = (ulong)(c0 + cc) * N + j;

        // beta of zero does not read y, which is what BLAS specifies -- and
        // what the host got wrong until a cache left -infinity in an output.
        y[at] = T(beta == 0.0f ? alpha * total
                               : alpha * total + beta * float(y[at]));
    }
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
INSTANTIATE(k_softcap, float, "_f32")(device float*, constant float&,
                                      constant uint&, uint);
INSTANTIATE(k_softcap, half, "_f16")(device half*, constant float&,
                                     constant uint&, uint);

INSTANTIATE(k_add_columns, float, "_f32")(device const float*, device float*,
                                          constant uint&, constant uint&, uint);
INSTANTIATE(k_add_columns, half, "_f16")(device const half*, device half*,
                                         constant uint&, constant uint&, uint);

INSTANTIATE(k_add_scaled, half, "_f16")(device const half*, device half*,
                                        constant float&, constant uint&, uint);
INSTANTIATE(k_softmax, float, "_f32")(device const float*, device float*,
                                      constant uint&, constant uint&,
                                      uint, uint);
INSTANTIATE(k_softmax, half, "_f16")(device const half*, device half*,
                                     constant uint&, constant uint&,
                                     uint, uint);
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
INSTANTIATE(k_q8_dequant, float, "_f32")(device const uchar*, device float*,
                                          constant uint&, constant uint&,
                                          constant uint&, uint);
INSTANTIATE(k_q8_dequant, half, "_f16")(device const uchar*, device half*,
                                        constant uint&, constant uint&,
                                        constant uint&, uint);
INSTANTIATE(k_q4k_dequant, float, "_f32")(device const uchar*, device float*,
                                      constant uint&, constant uint&,
                                      constant uint&, uint);
INSTANTIATE(k_q4k_dequant, half, "_f16")(device const uchar*, device half*,
                                      constant uint&, constant uint&,
                                      constant uint&, uint);
INSTANTIATE(k_q6k_dequant, float, "_f32")(device const uchar*, device float*,
                                      constant uint&, constant uint&,
                                      constant uint&, uint);
INSTANTIATE(k_q6k_dequant, half, "_f16")(device const uchar*, device half*,
                                      constant uint&, constant uint&,
                                      constant uint&, uint);
INSTANTIATE(k_q4k_gemv, float, "_f32")(device const uchar*, device const float*,
                                       device float*, constant uint&,
                                       constant uint&, constant uint&,
                                       constant float&, constant float&,
                                       uint, uint);
INSTANTIATE(k_q4k_gemv, half, "_f16")(device const uchar*, device const half*,
                                      device half*, constant uint&,
                                      constant uint&, constant uint&,
                                      constant float&, constant float&,
                                      uint, uint);
INSTANTIATE(k_q6k_gemv, float, "_f32")(device const uchar*, device const float*,
                                       device float*, constant uint&,
                                       constant uint&, constant uint&,
                                       constant float&, constant float&,
                                       uint, uint);
INSTANTIATE(k_q6k_gemv, half, "_f16")(device const uchar*, device const half*,
                                      device half*, constant uint&,
                                      constant uint&, constant uint&,
                                      constant float&, constant float&,
                                      uint, uint);

INSTANTIATE(k_q8_gemv, float, "_f32")(device const uchar*, device const float*,
                                      device float*, constant uint&,
                                      constant uint&, constant uint&,
                                      constant float&, constant float&,
                                      uint, uint);
INSTANTIATE(k_q8_gemv, half, "_f16")(device const uchar*, device const half*,
                                     device half*, constant uint&,
                                     constant uint&, constant uint&,
                                     constant float&, constant float&,
                                     uint, uint);
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
                                       constant uint&, constant float&,
                                       uint, uint);
INSTANTIATE(k_rms_norm, half, "_f16")(device const half*, device const half*,
                                      device half*, constant uint&,
                                      constant uint&, constant float&,
                                      uint, uint);
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
    id<MTLComputePipelineState> add_columns = nil;
    id<MTLComputePipelineState> softcap = nil;
    id<MTLComputePipelineState> softmax = nil;
    id<MTLComputePipelineState> causal_mask = nil;
    id<MTLComputePipelineState> copy_columns = nil;
    id<MTLComputePipelineState> rope = nil;
    id<MTLComputePipelineState> gather = nil;
    id<MTLComputePipelineState> q8_gemv = nil;        // one thread per row
    id<MTLComputePipelineState> q8_dequant = nil;     // one thread per block
    id<MTLComputePipelineState> q4k_dequant = nil;    // one thread per block
    id<MTLComputePipelineState> q6k_dequant = nil;    // one thread per block
    id<MTLComputePipelineState> qblocks_gather = nil; // one thread per block
    id<MTLComputePipelineState> q8_gemv_simd = nil;   // a SIMD group per row
    id<MTLComputePipelineState> q4k_gemv = nil;
    id<MTLComputePipelineState> q4k_gemv_simd = nil;
    id<MTLComputePipelineState> q6k_gemv = nil;
    id<MTLComputePipelineState> q6k_gemv_simd = nil;

    id<MTLComputePipelineState> attn_scores = nil;
    id<MTLComputePipelineState> attn_weighted = nil;
    id<MTLComputePipelineState> rms_norm = nil;

    /**
     * Weights unpacked for MPS, one per shape rather than one per weight.
     *
     * Per *shape* because a model has a handful of them and dozens of weights:
     * holding a dequantised copy of every weight would double the model in
     * memory, and holding one buffer for all of them would reallocate on every
     * matmul. A layer's seven matrices reuse four shapes, and every layer
     * reuses the same four.
     *
     * The cost this accepts is re-unpacking the same weight on every pass. It
     * is one read of the weights against a GEMM that is eight times what the
     * quantised kernel manages, and #286 has the arithmetic.
     *
     * **It is not free in memory, and the amount is worth knowing**: one f16
     * buffer per distinct shape, which for Qwen2.5-Coder 7B is
     *
     *     3584 x  3584    25.7 MB      3584 x   512     3.7 MB
     *     3584 x 18944   135.8 MB     18944 x  3584   135.8 MB
     *
     * -- 301 MB on top of an 8.1 GB model. A single buffer sized to the
     * largest shape would be 136 MB, because the two big ones hold the same
     * number of elements, and it would be exactly as safe for the reason
     * below. It is not done that way because `tensor` owns its shape and the
     * saving has not been shown to matter; if it ever does, that is the shape
     * of the fix.
     *
     * **Why sharing one scratch between weights is safe.** A command buffer
     * runs its encoders in the order they were created, and each multiply
     * encodes its unpack before its GEMM. So weight B's unpack cannot land on
     * weight A's operand: A's GEMM is already encoded ahead of it. That is an
     * argument rather than an observation, so ai_backend_test makes it a test
     * -- two weights of one shape, both encoded before either is read.
     */
    std::map<std::pair<unsigned int, unsigned int>,
             std::unique_ptr<tensor<T> > > dequantised;



    // Buffers made for one encoded operation and needed until the command
    // buffer has run.  Metal does retain what an encoder binds, so this is
    // belt and braces -- but the failure it guards against is a use-after-free
    // that would show up as an occasional wrong answer, which is the worst
    // kind to go looking for later.
    NSMutableArray* held = nil;

    unsigned int pending = 0;
};

namespace {

/**
 * Below how many units the q8 multiply splits each row across a SIMD group.
 *
 * A unit is one output row for one tile of columns, and it is one thread
 * unless this says otherwise.  Decode produces very few -- one column means
 * one unit per row, and k and v have 256 -- which does not fill the machine
 * however fast each thread is.  Prefill produces N per column tile and needs
 * no help.
 *
 * The number is measured rather than reasoned: see the comment on the
 * dispatch.
 */
/**
 * The batch at which dequantising and calling MPS beats the q8 kernel.
 *
 * `k_q8_gemv` runs at 1.2-1.6 TFLOP/s at prefill shapes where MPS does 11-13
 * on the same matrices (#286). Dequantising first costs one pass over the
 * weights, which a wide batch spreads thin and a narrow one cannot: at a
 * single column the pass is eight times the whole matmul it would replace.
 *
 * So there is a crossover, and it is measured rather than assumed. The gate
 * matrix (2048->5632), best of five batches, each path forced with this
 * override:
 *
 *     cols     k_q8_gemv    dequantise + MPS
 *        8       216.6 us            452.3 us
 *       16       423.1 us            483.7 us
 *       24       552.4 us            482.0 us     <- crossover
 *       32       803.5 us            476.8 us
 *       64      1335.0 us            534.0 us
 *      128      2493.6 us            720.4 us
 *      256      4756.1 us            721.7 us
 *      512      8863.2 us           1129.4 us     <- 7.9x
 *
 * Two different curves, which is why a threshold works at all: the kernel is
 * linear in columns because it re-reads the weights per tile, and this path is
 * nearly flat because the unpack is fixed and the GEMM after it is close to
 * free. 24 is where they cross and 24 is the default.
 *
 * Overridable for the same reason q8_reduce_below() is: comparing two paths
 * in one process is the only way to compare them without a rebuild and a
 * different thermal state in between -- and the table above is that
 * comparison. 0 turns dequantisation off entirely.
 */
unsigned int q8_dequant_above() {
    static const unsigned int n = [] {
        const char* e = std::getenv("JLIB_Q8_DEQUANT_ABOVE");

        return e ? unsigned(std::atoi(e)) : 24u;
    }();

    return n;
}

/**
 * How many bytes of unpacked weight to hold at once.
 *
 * **This is what replaced the cap.** Unpacking a whole weight made the scratch
 * as large as the weight -- 1.09 GB for Qwen2.5-Coder 7B's output head -- and
 * that is permanent GPU residency competing with the model itself. #286 dealt
 * with it by refusing to unpack anything too big, which is a workaround with
 * two costs: the biggest matrix in the model stays on the slow path, and the
 * threshold is a number fitted to one machine.
 *
 * Unpacking a block of output rows at a time removes the question. The traffic
 * is identical -- every weight byte is still read once, written once and read
 * once -- and the scratch is this, whatever the weight's size. A model's
 * largest is then a few tens of megabytes rather than a gigabyte, so there is
 * nothing to refuse and no threshold to fit.
 *
 * The number itself is measured; see the branch that added it. Overridable
 * because it trades scratch against how many GEMMs a multiply becomes, and
 * that trade is a property of the device.
 */
unsigned long q8_dequant_budget() { return dequant_budget(); }

unsigned int q8_reduce_below() {
    // Overridable so the two paths can be compared inside one process, which
    // is the only way to compare them without a rebuild and a different
    // thermal state in between.  0 turns the reduction off entirely.
    static const unsigned int n = [] {
        const char* e = std::getenv("JLIB_Q8_REDUCE_BELOW");

        return e ? unsigned(std::atoi(e)) : 16384u;
    }();

    return n;
}

struct pipelines {
    id<MTLComputePipelineState> activate = nil;
    id<MTLComputePipelineState> slope = nil;
    id<MTLComputePipelineState> hadamard = nil;
    id<MTLComputePipelineState> subtract = nil;
    id<MTLComputePipelineState> add_scaled = nil;
    id<MTLComputePipelineState> add_columns = nil;
    id<MTLComputePipelineState> softcap = nil;
    id<MTLComputePipelineState> softmax = nil;
    id<MTLComputePipelineState> causal_mask = nil;
    id<MTLComputePipelineState> copy_columns = nil;
    id<MTLComputePipelineState> rope = nil;
    id<MTLComputePipelineState> gather = nil;
    id<MTLComputePipelineState> q8_gemv = nil;        // one thread per row
    id<MTLComputePipelineState> q8_dequant = nil;     // one thread per block
    id<MTLComputePipelineState> q4k_dequant = nil;    // one thread per block
    id<MTLComputePipelineState> q6k_dequant = nil;    // one thread per block
    id<MTLComputePipelineState> qblocks_gather = nil; // one thread per block
    id<MTLComputePipelineState> q8_gemv_simd = nil;   // a SIMD group per row
    id<MTLComputePipelineState> q4k_gemv = nil;
    id<MTLComputePipelineState> q4k_gemv_simd = nil;
    id<MTLComputePipelineState> q6k_gemv = nil;
    id<MTLComputePipelineState> q6k_gemv_simd = nil;
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
        { "k_add_columns", &p.add_columns },
        { "k_softcap", &p.softcap },
        { "k_softmax",    &p.softmax },
        { "k_causal_mask", &p.causal_mask },
        { "k_copy_columns", &p.copy_columns },
        { "k_rope",       &p.rope },
        { "k_gather",     &p.gather },
        { "k_attn_scores", &p.attn_scores },
        { "k_attn_weighted", &p.attn_weighted },
        { "k_rms_norm",   &p.rms_norm },
        { "k_q8_dequant", &p.q8_dequant },
        { "k_q4k_dequant", &p.q4k_dequant },
        { "k_q6k_dequant", &p.q6k_dequant },
    };

    // The q8 multiply is built twice from one source, specialised on how many
    // lanes share a row.  See Q8_LANES.
    for(unsigned int lanes : { 1u, 32u }) {
        MTLFunctionConstantValues* cv = [MTLFunctionConstantValues new];

        [cv setConstantValue:&lanes type:MTLDataTypeUInt atIndex:0];

        // Three formats, each built at both lane counts: q8_0 and the two
        // K-quants a Q4_K_M mixes.  One file needs both of the latter.
        struct { const char* base;
                 __strong id<MTLComputePipelineState>* one;
                 __strong id<MTLComputePipelineState>* simd; } gemvs[] = {
            { "k_q8_gemv",  &p.q8_gemv,  &p.q8_gemv_simd  },
            { "k_q4k_gemv", &p.q4k_gemv, &p.q4k_gemv_simd },
            { "k_q6k_gemv", &p.q6k_gemv, &p.q6k_gemv_simd },
        };

        for(auto& g : gemvs) {
            const std::string name = std::string(g.base) + traits<T>::suffix();

            id<MTLFunction> fn =
                [lib newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]
                          constantValues:cv
                                   error:&err];

            if(fn == nil)
                throw ai::backend_error("no kernel called " + name);

            id<MTLComputePipelineState> built =
                [gpu newComputePipelineStateWithFunction:fn error:&err];

            if(built == nil)
                throw ai::backend_error("could not build a pipeline for " + name);

            *(lanes == 1 ? g.one : g.simd) = built;
        }
    }

    {
        // No suffix: this one copies bytes and is not templated on T, so
        // there is one of it rather than one per element type.
        id<MTLFunction> fn = [lib newFunctionWithName:@"k_qblocks_gather"];

        if(fn == nil)
            throw ai::backend_error("no kernel called k_qblocks_gather");

        p.qblocks_gather =
            [gpu newComputePipelineStateWithFunction:fn error:&err];

        if(p.qblocks_gather == nil)
            throw ai::backend_error("could not build a pipeline for "
                                    "k_qblocks_gather");
    }

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
    m_impl->add_columns = p.add_columns;
    m_impl->softcap = p.softcap;
    m_impl->softmax = p.softmax;
    m_impl->causal_mask = p.causal_mask;
    m_impl->copy_columns = p.copy_columns;
    m_impl->rope = p.rope;
    m_impl->gather = p.gather;
    m_impl->q8_gemv = p.q8_gemv;
    m_impl->q8_dequant = p.q8_dequant;
    m_impl->q4k_dequant = p.q4k_dequant;
    m_impl->q6k_dequant = p.q6k_dequant;
    m_impl->qblocks_gather = p.qblocks_gather;


    m_impl->q8_gemv_simd = p.q8_gemv_simd;
    m_impl->q4k_gemv = p.q4k_gemv;
    m_impl->q4k_gemv_simd = p.q4k_gemv_simd;
    m_impl->q6k_gemv = p.q6k_gemv;
    m_impl->q6k_gemv_simd = p.q6k_gemv_simd;
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
void stream<T>::softcap(tensor<T>& x, float cap) {
    if(cap == 0) return;

    open();

    const unsigned int n = x.size();

    [m_impl->enc setComputePipelineState:m_impl->softcap];
    [m_impl->enc setBuffer:x.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBytes:&cap length:sizeof(cap) atIndex:1];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:2];

    dispatch(m_impl->enc, m_impl->softcap, n);

    m_impl->pending++;
}

template<typename T>
void stream<T>::add_columns(const tensor<T>& bias, tensor<T>& y) {
    if(bias.rows() != y.rows() || bias.cols() != 1)
        throw std::runtime_error("add_columns: the bias must be one column of "
                                 "rows entries");

    open();

    const unsigned int rows = y.rows();
    const unsigned int n = y.size();

    [m_impl->enc setComputePipelineState:m_impl->add_columns];
    [m_impl->enc setBuffer:bias.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:y.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:2];
    [m_impl->enc setBytes:&n length:sizeof(n) atIndex:3];

    dispatch(m_impl->enc, m_impl->add_columns, n);

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
    // A SIMD group to a column now; see the kernel.
    dispatch(m_impl->enc, m_impl->softmax, cols * 32);

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

namespace {

unsigned long& dequant_budget_bytes() {
    static unsigned long n = [] {
        const char* e = std::getenv("JLIB_Q8_DEQUANT_BUDGET");

        return e ? (unsigned long)std::atol(e) * 1024 * 1024
                 : 64ul * 1024 * 1024;
    }();

    return n;
}

}

unsigned long dequant_budget() { return dequant_budget_bytes(); }

void dequant_budget(unsigned long bytes) { dequant_budget_bytes() = bytes; }

qweight::qweight(std::shared_ptr<device> d, ai::quant fmt, unsigned int rows,
                 unsigned int cols, const void* blocks, std::size_t bytes)
    : m_format(fmt),
      m_device(d),
      m_impl(new impl),
      m_rows(rows),
      m_cols(cols),
      m_bytes(bytes)
{
    const std::size_t n = std::size_t(rows) * cols;

    const std::size_t vals = ai::quant_values(fmt);
    const std::size_t size = ai::quant_bytes(fmt);

    if(n % vals)
        throw std::runtime_error("jlib::metal::qweight: the element count is "
                                 "not a multiple of " + ai::quant_name(fmt) +
                                 "'s block size");

    if(bytes != (n / vals) * size)
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
tensor<T>& stream<T>::dequantised(const qweight& w, unsigned int K,
                                  unsigned int first, unsigned int cols,
                                  unsigned int width)
{
    // Keyed by the *block* shape rather than the weight's, which is the whole
    // point: a model has a handful of K values and one width, so this holds a
    // few tens of megabytes however large the weights are.
    std::unique_ptr<tensor<T> >& slot =
        m_impl->dequantised[std::make_pair(K, width)];

    if(!slot) slot.reset(new tensor<T>(m_device, K, width));

    open();

    const unsigned int vals = ai::quant_values(w.format());

    id<MTLComputePipelineState> pipe = nil;
    unsigned int per_block = 1;

    switch(w.format()) {
    case ai::quant::q8_0: pipe = m_impl->q8_dequant;  per_block = 1;  break;
    case ai::quant::q4_K: pipe = m_impl->q4k_dequant; per_block = 8;  break;
    case ai::quant::q6_K: pipe = m_impl->q6k_dequant; per_block = 16; break;
    }

    const unsigned int per_col = K / vals;          // blocks in one column
    const unsigned int blocks = per_col * cols;
    const unsigned int units = blocks * per_block;

    // Where this block of columns starts in the weight.  Passed as a value
    // rather than by binding the buffer at an offset, because a q8_0 column
    // is 34 * K/32 bytes and that is not reliably four-byte aligned, which
    // setBuffer:offset: requires.
    const unsigned int first_block = first * per_col;

    [m_impl->enc setComputePipelineState:pipe];
    [m_impl->enc setBuffer:w.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:slot->m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&K length:sizeof(K) atIndex:2];
    [m_impl->enc setBytes:&units length:sizeof(units) atIndex:3];
    [m_impl->enc setBytes:&first_block length:sizeof(first_block) atIndex:4];

    dispatch(m_impl->enc, pipe, units);

    m_impl->pending++;

    return *slot;
}

template<typename T>
void stream<T>::gather(const qweight& table, const std::vector<int>& ids,
                       tensor<T>& out)
{
    const unsigned int rows = table.rows();
    const unsigned int n = static_cast<unsigned int>(ids.size());

    if(out.rows() != rows || out.cols() != n)
        throw typename tensor<T>::exception("gather: out must be the table's "
                                            "height by the number of ids");

    // Here, where there is somewhere to throw from -- the kernel indexes with
    // whatever it is given and a bad id would read another column silently.
    for(std::size_t i = 0; i < ids.size(); i++) {
        if(ids[i] < 0 || static_cast<unsigned int>(ids[i]) >= table.cols()) {
            std::ostringstream e;

            e << "gather: token id " << ids[i] << " is outside a table of "
              << table.cols();

            throw typename tensor<T>::exception(e.str());
        }
    }

    if(n == 0) return;

    const ai::quant fmt = table.format();

    const unsigned int vals = ai::quant_values(fmt);
    const unsigned int size = ai::quant_bytes(fmt);

    if(rows % vals)
        throw typename tensor<T>::exception("gather: the table's height is not "
                                            "a multiple of the block size");

    const unsigned int nb = rows / vals;          // blocks in one column

    open();

    id<MTLBuffer> idbuf =
        [m_device->m_impl->gpu newBufferWithBytes:ids.data()
                                           length:ids.size() * sizeof(int)
                                          options:MTLResourceStorageModeShared];

    // The picked columns, still encoded.  One buffer per call rather than a
    // kept scratch: it is the size of the *prompt*, not of the table -- 1 MB
    // for 512 tokens of a 7B -- and holding one would mean holding it for the
    // life of the model to save an allocation that does not show up.
    id<MTLBuffer> packed =
        [m_device->m_impl->gpu newBufferWithLength:(NSUInteger)nb * size * n
                                           options:MTLResourceStorageModePrivate];

    if(idbuf == nil || packed == nil)
        throw typename tensor<T>::exception("gather: could not allocate");

    [m_impl->held addObject:idbuf];
    [m_impl->held addObject:packed];

    const unsigned int copies = nb * n;

    [m_impl->enc setComputePipelineState:m_impl->qblocks_gather];
    [m_impl->enc setBuffer:table.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:idbuf offset:0 atIndex:1];
    [m_impl->enc setBuffer:packed offset:0 atIndex:2];
    [m_impl->enc setBytes:&nb length:sizeof(nb) atIndex:3];
    [m_impl->enc setBytes:&size length:sizeof(size) atIndex:4];
    [m_impl->enc setBytes:&copies length:sizeof(copies) atIndex:5];

    dispatch(m_impl->enc, m_impl->qblocks_gather, copies);

    m_impl->pending++;

    // Then the ordinary unpack over what was picked, which is why there is no
    // gather kernel per format.  Encoders run in the order they were created,
    // so the copy above is complete before this reads it.
    id<MTLComputePipelineState> pipe = nil;
    unsigned int per_block = 1;

    switch(fmt) {
    case ai::quant::q8_0: pipe = m_impl->q8_dequant;  per_block = 1;  break;
    case ai::quant::q4_K: pipe = m_impl->q4k_dequant; per_block = 8;  break;
    case ai::quant::q6_K: pipe = m_impl->q6k_dequant; per_block = 16; break;
    }

    const unsigned int units = copies * per_block;

    const unsigned int from_start = 0;   // `packed` holds only what was asked

    [m_impl->enc setComputePipelineState:pipe];
    [m_impl->enc setBuffer:packed offset:0 atIndex:0];
    [m_impl->enc setBuffer:out.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBytes:&rows length:sizeof(rows) atIndex:2];
    [m_impl->enc setBytes:&units length:sizeof(units) atIndex:3];
    [m_impl->enc setBytes:&from_start length:sizeof(from_start) atIndex:4];

    dispatch(m_impl->enc, pipe, units);

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

    // A SIMD group to a column now; see the kernel.
    dispatch(m_impl->enc, m_impl->rms_norm, cols * 32);

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
/**
 * @param cstride elements between one column of C and the next in the buffer,
 *        when C is a *row block* of a taller matrix rather than the whole of
 *        one.  0 means C is contiguous and the stride is its own height.
 * @param coffset elements from the start of `bc` to C's first element
 *
 * Those two are what let a caller produce rows [j0, j0+B) of an (N x ncols)
 * result without a separate buffer and a copy: MPS takes a row stride
 * independent of the column count, so a block of a column-major matrix is
 * describable in place.  See multiply_tn(qweight...), which is the only
 * caller that needs it.
 */
template<typename T>
void encode_gemm(id<MTLCommandBuffer> cmd, id<MTLDevice> gpu,
                 id<MTLBuffer> ba, unsigned int arows, unsigned int acols, bool ta,
                 id<MTLBuffer> bb, unsigned int brows, unsigned int bcols, bool tb,
                 id<MTLBuffer> bc, unsigned int crows, unsigned int ccols,
                 float alpha, float beta,
                 unsigned int cstride = 0, unsigned int coffset = 0)
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
    // C's rows are `cstride` elements apart, which is its own height unless
    // the caller is writing a block of a taller matrix.
    const unsigned int cpitch = cstride ? cstride : crows;

    MPSMatrixDescriptor* dc =
        [MPSMatrixDescriptor matrixDescriptorWithRows:ccols columns:crows
                                             rowBytes:cpitch * sizeof(T)
                                             dataType:dt];

    MPSMatrix* ma = [[MPSMatrix alloc] initWithBuffer:ba descriptor:da];
    MPSMatrix* mb = [[MPSMatrix alloc] initWithBuffer:bb descriptor:db];
    MPSMatrix* mc = [[MPSMatrix alloc] initWithBuffer:bc
                                               offset:coffset * sizeof(T)
                                           descriptor:dc];

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
void stream<T>::multiply_tn(const qweight& w, const tensor<T>& x, tensor<T>& y,
                            float alpha, float beta)
{
    const unsigned int K = w.rows();
    const unsigned int N = w.cols();

    if(x.rows() != K)
        throw typename tensor<T>::exception("quantised multiply_tn: the input is not "
                                            "as tall as the weight is wide");

    if(y.rows() != N || y.cols() != x.cols())
        throw typename tensor<T>::exception("quantised multiply_tn: the output shape "
                                            "does not match");

    const unsigned int ncols = x.cols();

    // **Wide enough to pay for unpacking the weights first.**
    //
    // MPS does 11-13 TFLOP/s on these matrices and the kernel below does 1.5,
    // so for a prefill batch the fastest thing this can do is stop being
    // clever: dequantise into a scratch and hand the problem to the GEMM that
    // is already linked in. One pass over the weights, spread over every
    // column in the batch.
    //
    // Not at decode, where the batch is one column and that pass would cost
    // eight times the matmul it replaces. See q8_dequant_above().
    if(q8_dequant_above() && ncols >= q8_dequant_above()) {
        // **A block of output rows at a time.**
        //
        // y = w^T x, so rows [j0, j0 + b) of y are columns [j0, j0 + b) of w
        // -- and a column of a quantised weight is whole blocks, contiguous.
        // So the weight is unpacked a piece at a time into a scratch bounded
        // by q8_dequant_budget() rather than by the weight, and each piece is
        // multiplied straight into its own rows of y.
        //
        // No copy afterwards: those rows are a strided submatrix of a
        // column-major y, which MPS describes with a row stride and an offset.
        const unsigned long each = (unsigned long)K * sizeof(T);

        unsigned int width =
            (unsigned int)std::max(1ul, q8_dequant_budget() / each);

        // A multiple of 128 so the offset into y stays 256-byte aligned, and
        // never wider than the weight.
        width = std::max(128u, width & ~127u);
        width = std::min(width, N);

        for(unsigned int j0 = 0; j0 < N; j0 += width) {
            const unsigned int b = std::min(width, N - j0);

            tensor<T>& part = dequantised(w, K, j0, b, width);

            close();

            encode_gemm<T>(m_impl->cmd, m_device->m_impl->gpu,
                           part.m_impl->buf, K, b, true,
                           x.m_impl->buf, K, ncols, false,
                           y.m_impl->buf, b, ncols, alpha, beta,
                           N, j0);

            m_impl->pending++;
        }

        return;
    }

    open();

    // One thread per output row per tile of columns.  A "unit" is that pair.
    const unsigned int tiles = (ncols + 8 - 1) / 8;
    const unsigned int units = N * tiles;

    // A SIMD group per row only when there are too few units to fill the GPU.
    // Decode is the case that needs it: one column means one unit per output
    // row, and k and v have 256 of them.  Prefill already has a unit per row
    // per column tile, and a reduction there only costs -- measured at 19-26%
    // before this was made conditional.  See Q8_LANES and q8_reduce_below.
    const bool reduce = units < q8_reduce_below();

    id<MTLComputePipelineState> pipe = nil;

    switch(w.format()) {
    case ai::quant::q8_0:
        pipe = reduce ? m_impl->q8_gemv_simd  : m_impl->q8_gemv;  break;
    case ai::quant::q4_K:
        pipe = reduce ? m_impl->q4k_gemv_simd : m_impl->q4k_gemv; break;
    case ai::quant::q6_K:
        pipe = reduce ? m_impl->q6k_gemv_simd : m_impl->q6k_gemv; break;
    }

    [m_impl->enc setComputePipelineState:pipe];
    [m_impl->enc setBuffer:w.m_impl->buf offset:0 atIndex:0];
    [m_impl->enc setBuffer:x.m_impl->buf offset:0 atIndex:1];
    [m_impl->enc setBuffer:y.m_impl->buf offset:0 atIndex:2];
    [m_impl->enc setBytes:&K length:sizeof(K) atIndex:3];
    [m_impl->enc setBytes:&N length:sizeof(N) atIndex:4];
    [m_impl->enc setBytes:&ncols length:sizeof(ncols) atIndex:5];
    [m_impl->enc setBytes:&alpha length:sizeof(alpha) atIndex:6];
    [m_impl->enc setBytes:&beta length:sizeof(beta) atIndex:7];

    dispatch(m_impl->enc, pipe, units * (reduce ? 32u : 1u));

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

    // Read before the buffer is let go, or the reason goes with it.
    const std::string why = failed ? command_buffer_error(m_impl->cmd)
                                   : std::string();

    m_impl->cmd = nil;
    m_impl->pending = 0;

    [m_impl->held removeAllObjects];

    if(failed) throw exception(why);
}

// Objective-C++ cannot be a template header, so the instantiations live here.
// Adding bfloat would be a third line, a third kernel name, and nothing else.
template class tensor<float>;
template class tensor<_Float16>;
template class stream<float>;
template class stream<_Float16>;

}
}
