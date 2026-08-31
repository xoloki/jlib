/* -*- mode: C++ c-basic-offset: 4  -*-
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

#ifndef JLIB_AI_GGUF_HH
#define JLIB_AI_GGUF_HH

#include <jlib/math/matrix.hh>

#include <cstdint>
#include <exception>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * A GGUF model file: its metadata, its tensor index, and its weights.
 *
 * GGUF is the container llama.cpp writes and everything else has followed. A
 * file is a header, a block of typed key/value metadata, an index of tensors,
 * and then the tensor data, aligned.
 *
 * Metadata and the index are read at construction; **tensor data is not**. The
 * smallest model worth testing against is over a gigabyte, so read() seeks and
 * dequantises one tensor at a time. That makes read() the only expensive call
 * and the file handle shared mutable state -- one gguf is not safe to read
 * from two threads.
 *
 * ### Layout, and the happy accident in it
 *
 * GGUF dimensions are given fastest-varying first: `dims[0]` is the contiguous
 * one. jlib's math::matrix is column-major, so element (r,c) lives at
 * `c*rows + r` -- which is the *same* arrangement with `rows = dims[0]`. A
 * GGUF tensor therefore loads into a matrix of `dims[0] x dims[1]` with no
 * transposition and no shuffling, and `token_embd.weight` arrives as one
 * column per token, which is the orientation the rest of this library already
 * wants.
 *
 * What that costs is stated here so nobody rediscovers it: ggml's convention
 * makes a weight `[n_in, n_out]`, so the product a layer wants is `W^T x`, not
 * `W x`. Use backend::multiply_tn on a matrix loaded this way. Transposing at
 * load time would be the other option and a worse one -- it would touch every
 * byte of a gigabyte to save a flag.
 *
 * ### Endianness
 *
 * Little-endian only. The spec permits big-endian files and this reads the
 * bytes natively, so it would misread one; it does not pretend to detect that,
 * because no such file has ever been seen in the wild and a check that has
 * never run is not worth more than a sentence saying it is missing.
 */
class gguf {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::ai::gguf::exception: " + msg) {}

        const char* what() const throw() { return m_msg.c_str(); }

    private:
        std::string m_msg;
    };

    /** The metadata value types, with the spec's numbering. */
    enum class value_type {
        uint8 = 0, int8 = 1, uint16 = 2, int16 = 3, uint32 = 4, int32 = 5,
        float32 = 6, boolean = 7, string = 8, array = 9, uint64 = 10,
        int64 = 11, float64 = 12
    };

    /** The ggml tensor types, of which only a few are read here. */
    enum class tensor_type {
        f32 = 0, f16 = 1, q4_0 = 2, q4_1 = 3, q5_0 = 6, q5_1 = 7, q8_0 = 8,
        q8_1 = 9, q2_k = 10, q3_k = 11, q4_k = 12, q5_k = 13, q6_k = 14,
        q8_k = 15
    };

    /**
     * One metadata value.
     *
     * A flat struct rather than a variant: every integer width lands in `i`
     * and every float width in `d`, because a caller asking for
     * llama.block_count does not want to know whether the file said uint32 or
     * uint64. `type` is kept so it can still be asked.
     */
    struct value {
        value_type type = value_type::uint32;

        std::int64_t i = 0;             ///< every integer width, and bool
        double d = 0;                   ///< float32 and float64
        std::string s;                  ///< string

        value_type element = value_type::uint32;   ///< an array's element type
        std::vector<std::string> strings;          ///< an array of strings
        std::vector<double> numbers;               ///< an array of anything else
    };

    struct tensor_info {
        std::string name;
        std::vector<std::uint64_t> shape;   ///< dims[0] is the contiguous one
        tensor_type type = tensor_type::f32;
        std::uint64_t offset = 0;           ///< from the start of the data section

        /** How many elements, which is the product of the shape. */
        std::uint64_t elements() const;
    };

    explicit gguf(const std::string& path);

    unsigned int version() const { return m_version; }

    /** Where the tensor data begins, after the index and its padding. */
    std::uint64_t data_offset() const { return m_data_offset; }

    std::uint64_t alignment() const { return m_alignment; }

    const std::map<std::string, value>& metadata() const { return m_meta; }

    bool has(const std::string& key) const;

    /** Throws unless the key is present and of a compatible kind. */
    const value& get(const std::string& key) const;

    std::string str(const std::string& key) const;
    std::int64_t integer(const std::string& key) const;
    double real(const std::string& key) const;

    const std::vector<tensor_info>& tensors() const { return m_tensors; }

    bool has_tensor(const std::string& name) const;
    const tensor_info& tensor(const std::string& name) const;

    /**
     * Read one tensor, dequantised, as (shape[0] rows x shape[1] cols).
     *
     * A one-dimensional tensor -- every norm weight is one -- comes back as a
     * single column. Always float: dequantisation lands there naturally, and a
     * caller wanting fp16 can narrow afterwards with the loss it chose.
     *
     * f32, f16 and q8_0 are understood. Anything else throws naming the type
     * rather than returning something plausible.
     */
    math::matrix<float> read(const tensor_info& t) const;
    math::matrix<float> read(const std::string& name) const;

    /**
     * The tensor's bytes exactly as the file holds them, undecoded.
     *
     * For a weight that is going to stay in the encoding it arrived in: a
     * q8_0 tensor dequantised at load costs twice the memory and twice the
     * bandwidth of one dequantised inside the kernel that reads it.
     *
     * The caller has to know what the bytes mean, which is what `type` is for.
     */
    std::vector<char> read_raw(const tensor_info& t) const;
    std::vector<char> read_raw(const std::string& name) const;

    /** How many bytes a tensor of this type and size occupies. */
    static std::uint64_t stored_bytes(tensor_type t, std::uint64_t elements);

    /** The name of a ggml type, for an error message worth reading. */
    static std::string type_name(tensor_type t);

private:
    std::string m_path;
    mutable std::ifstream m_file;

    unsigned int m_version = 0;
    std::uint64_t m_alignment = 32;
    std::uint64_t m_data_offset = 0;

    std::map<std::string, value> m_meta;
    std::vector<tensor_info> m_tensors;
    std::map<std::string, std::size_t> m_by_name;

    void read_metadata(std::uint64_t count);
    void read_index(std::uint64_t count);

    value read_value(value_type t);
    std::string read_string();

    template<typename U> U read_pod();
};

}
}

#endif // JLIB_AI_GGUF_HH
