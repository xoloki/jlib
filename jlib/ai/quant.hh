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
 *
 */

#ifndef JLIB_AI_QUANT_HH
#define JLIB_AI_QUANT_HH

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace jlib {
namespace ai {

/**
 * The quantised block formats jlib reads, and how to unpack one block.
 *
 * This is here rather than in gguf.cc because three things need it and they
 * cannot share code any other way: the file reader, which dequantises a whole
 * tensor on the host; the host backend, which is the reference the GPU is
 * tested against; and -- as the reference beside it rather than as shared
 * code -- the Metal kernels, which must be written again in MSL.
 *
 * **The bit positions are ggml's, not a re-derivation.** They are checked
 * against the reference dequantiser bit for bit in `tests/kquant_values.hh`,
 * which is the only reason to believe any of it.
 */
enum class quant {
    /** One f16 scale then 32 signed bytes: 34 bytes for 32 values. */
    q8_0,

    /**
     * 144 bytes for 256 values: 128 nibbles, 12 packed bytes holding six-bit
     * scales *and* mins, and 2 f16.
     *
     * Affine -- `d*q - m` -- so a block can represent a range that does not
     * straddle zero. That is the part with no analogue in the others.
     */
    q4_K,

    /**
     * 210 bytes for 256 values: 128 low nibbles, 64 high bit-pairs, 16 int8
     * sub-block scales and one f16.
     *
     * Symmetric, with six bits stored unsigned meaning [-32, 31].
     */
    q6_K
};

/**
 * Why the K-quants are two-tier and q8_0 is not.
 *
 * A q8_0 block is flat: one scale and thirty-two bytes. A K-quant block is
 * 256 weights sharing one f16 super-block scale, with *sub-block* scales
 * quantised against it. That second tier is what buys the accuracy at four
 * bits, and it is the whole of the difference.
 *
 * Q4_K_M -- the format most models ship in -- is a **mixture**, some tensors
 * q4_K and some q6_K, so reading one file needs both.
 */
const std::uint64_t QK_K = 256;

/** How many values one block of this format holds. */
inline unsigned int quant_values(quant q)
{
    return q == quant::q8_0 ? 32u : 256u;
}

/** How many bytes one block of this format occupies. */
inline unsigned int quant_bytes(quant q)
{
    switch(q) {
    case quant::q8_0: return 34u;
    case quant::q4_K: return 144u;
    case quant::q6_K: return 210u;
    }

    return 0u;
}

/** The name a GGUF file uses. */
inline std::string quant_name(quant q)
{
    switch(q) {
    case quant::q8_0: return "q8_0";
    case quant::q4_K: return "q4_K";
    case quant::q6_K: return "q6_K";
    }

    return "?";
}

/** An f16 read from an unaligned offset, which every block has. */
inline float half_at(const char* p)
{
    _Float16 h;

    std::memcpy(&h, p, sizeof(h));

    return float(h);
}

/**
 * q4_K's sub-block scale and min, unpacked.
 *
 * Twelve bytes hold eight six-bit scales and eight six-bit mins.  The first
 * four of each sit in their own byte's low six bits; the last four are split,
 * four bits in one byte and the top two bits borrowed from the high end of an
 * earlier one.  This is ggml's `get_scale_min_k4` and the bit positions are
 * its, not a re-derivation.
 */
inline void scale_min_k4(int j, const unsigned char* q,
                         unsigned char& d, unsigned char& m)
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

/** One q8_0 block to 32 floats. */
inline void dequantise_q8_0(const char* block, float* into)
{
    const float d = half_at(block);

    const signed char* q = reinterpret_cast<const signed char*>(block + 2);

    for(unsigned int i = 0; i < 32; i++) into[i] = d * float(q[i]);
}

/** One q4_K super-block to 256 floats. */
inline void dequantise_q4_k(const char* block, float* into)
{
    const float d    = half_at(block);          // scale of the scales
    const float dmin = half_at(block + 2);      // scale of the mins

    const unsigned char* sc =
        reinterpret_cast<const unsigned char*>(block + 4);    // 12
    const unsigned char* q =
        reinterpret_cast<const unsigned char*>(block + 16);   // 128

    float* y = into;

    // Four groups of 64, each byte of q holding two weights.  Affine: the min
    // is subtracted rather than the weights being centred.
    for(int j = 0; j < 4; j++) {
        unsigned char s1 = 0, m1 = 0, s2 = 0, m2 = 0;

        scale_min_k4(j * 2,     sc, s1, m1);
        scale_min_k4(j * 2 + 1, sc, s2, m2);

        const float d1 = d * float(s1), o1 = dmin * float(m1);
        const float d2 = d * float(s2), o2 = dmin * float(m2);

        for(int l = 0; l < 32; l++) *y++ = d1 * float(q[l] & 0xF) - o1;
        for(int l = 0; l < 32; l++) *y++ = d2 * float(q[l] >> 4)  - o2;

        q += 32;
    }
}

/** One q6_K super-block to 256 floats. */
inline void dequantise_q6_k(const char* block, float* into)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(block);

    const unsigned char* ql = p;              // 128 low nibbles
    const unsigned char* qh = p + 128;        // 64 high bit-pairs
    const signed char* sc =
        reinterpret_cast<const signed char*>(p + 192);   // 16 scales

    const float d = half_at(block + 208);

    float* y = into;

    // Two halves of 128, each drawing four six-bit weights per byte of ql:
    // the low nibble and the high nibble, each topped up with two bits from
    // qh.  The 32 is the zero point -- six bits are stored unsigned and mean
    // [-32, 31].
    for(unsigned int h = 0; h < 2; h++) {
        for(unsigned int l = 0; l < 32; l++) {
            const int is = int(l / 16);

            const int q1 = int((ql[l]      & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
            const int q2 = int((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
            const int q3 = int((ql[l]      >> 4)  | (((qh[l] >> 4) & 3) << 4)) - 32;
            const int q4 = int((ql[l + 32] >> 4)  | (((qh[l] >> 6) & 3) << 4)) - 32;

            y[l]      = d * float(sc[is + 0]) * float(q1);
            y[l + 32] = d * float(sc[is + 2]) * float(q2);
            y[l + 64] = d * float(sc[is + 4]) * float(q3);
            y[l + 96] = d * float(sc[is + 6]) * float(q4);
        }

        y  += 128;
        ql += 64;
        qh += 32;
        sc += 8;
    }
}

/** One block of any format, to `quant_values(q)` floats. */
inline void dequantise_block(quant q, const char* block, float* into)
{
    switch(q) {
    case quant::q8_0: dequantise_q8_0(block, into); break;
    case quant::q4_K: dequantise_q4_k(block, into); break;
    case quant::q6_K: dequantise_q6_k(block, into); break;
    }
}

}
}

#endif // JLIB_AI_QUANT_HH
