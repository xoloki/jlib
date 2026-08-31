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

#include <jlib/ai/gguf.hh>

#include <cstring>
#include <sstream>

namespace jlib {
namespace ai {

namespace {

    /** Q8_0: one fp16 scale then 32 signed bytes, 34 bytes for 32 values. */
    const std::uint64_t Q8_0_BLOCK = 32;
    const std::uint64_t Q8_0_BYTES = 34;

    /**
     * A bound on how long a string in the metadata may be.
     *
     * Not a format limit -- there is none -- but a length is read from the
     * file before it is trusted, and a corrupt one would otherwise ask for an
     * allocation of whatever the bytes happened to say.  The longest string in
     * a real file is a chat template of a few kilobytes.
     */
    const std::uint64_t MAX_STRING = 64u * 1024u * 1024u;

    /** And the same for counts, which are read before anything validates them. */
    const std::uint64_t MAX_COUNT = 64u * 1024u * 1024u;

}

std::uint64_t gguf::tensor_info::elements() const {
    std::uint64_t n = 1;

    for(std::uint64_t d : shape) n *= d;

    return n;
}

template<typename U>
U gguf::read_pod() {
    U v;

    m_file.read(reinterpret_cast<char*>(&v), sizeof(U));

    if(!m_file)
        throw exception("the file ended in the middle of a value");

    return v;
}

std::string gguf::read_string() {
    const std::uint64_t len = read_pod<std::uint64_t>();

    if(len > MAX_STRING) {
        std::ostringstream o;

        o << "a string of " << len << " bytes, which is longer than anything "
          << "this will believe";

        throw exception(o.str());
    }

    std::string s(static_cast<std::size_t>(len), '\0');

    if(len) {
        m_file.read(&s[0], static_cast<std::streamsize>(len));

        if(!m_file)
            throw exception("the file ended in the middle of a string");
    }

    return s;
}

gguf::value gguf::read_value(value_type t) {
    value v;

    v.type = t;

    switch(t) {
    case value_type::uint8:   v.i = read_pod<std::uint8_t>();  break;
    case value_type::int8:    v.i = read_pod<std::int8_t>();   break;
    case value_type::uint16:  v.i = read_pod<std::uint16_t>(); break;
    case value_type::int16:   v.i = read_pod<std::int16_t>();  break;
    case value_type::uint32:  v.i = read_pod<std::uint32_t>(); break;
    case value_type::int32:   v.i = read_pod<std::int32_t>();  break;
    case value_type::uint64:  v.i = static_cast<std::int64_t>(read_pod<std::uint64_t>()); break;
    case value_type::int64:   v.i = read_pod<std::int64_t>();  break;
    case value_type::boolean: v.i = read_pod<std::uint8_t>() ? 1 : 0; break;
    case value_type::float32: v.d = read_pod<float>();         break;
    case value_type::float64: v.d = read_pod<double>();        break;
    case value_type::string:  v.s = read_string();             break;

    case value_type::array: {
        const value_type et = static_cast<value_type>(read_pod<std::uint32_t>());
        const std::uint64_t n = read_pod<std::uint64_t>();

        if(n > MAX_COUNT) {
            std::ostringstream o;

            o << "an array of " << n << " elements, which is more than this "
              << "will believe";

            throw exception(o.str());
        }

        v.element = et;

        if(et == value_type::array)
            throw exception("an array of arrays, which the format does not "
                            "nest and this does not read");

        // Every element, one at a time, even when only a few are wanted:
        // strings are variable length, so there is no seeking past the tail of
        // one of these.  tokenizer.ggml.merges is sixty thousand of them.
        for(std::uint64_t k = 0; k < n; k++) {
            const value e = read_value(et);

            if(et == value_type::string) v.strings.push_back(e.s);
            else if(et == value_type::float32 || et == value_type::float64)
                v.numbers.push_back(e.d);
            else v.numbers.push_back(double(e.i));
        }

        break;
    }

    default: {
        std::ostringstream o;

        o << "unknown metadata value type " << static_cast<int>(t);

        throw exception(o.str());
    }
    }

    return v;
}

void gguf::read_metadata(std::uint64_t count) {
    for(std::uint64_t k = 0; k < count; k++) {
        const std::string key = read_string();
        const value_type t = static_cast<value_type>(read_pod<std::uint32_t>());

        m_meta[key] = read_value(t);
    }

    // Read after the loop rather than assumed, because the alignment is itself
    // one of the keys and the padding below depends on it.
    if(has("general.alignment")) {
        const std::int64_t a = integer("general.alignment");

        if(a <= 0 || (a & (a - 1)))
            throw exception("general.alignment is not a positive power of two");

        m_alignment = static_cast<std::uint64_t>(a);
    }
}

void gguf::read_index(std::uint64_t count) {
    for(std::uint64_t k = 0; k < count; k++) {
        tensor_info t;

        t.name = read_string();

        const std::uint32_t nd = read_pod<std::uint32_t>();

        if(nd > 4)
            throw exception("a tensor of more than four dimensions");

        for(std::uint32_t d = 0; d < nd; d++)
            t.shape.push_back(read_pod<std::uint64_t>());

        t.type = static_cast<tensor_type>(read_pod<std::uint32_t>());
        t.offset = read_pod<std::uint64_t>();

        m_by_name[t.name] = m_tensors.size();
        m_tensors.push_back(t);
    }
}

gguf::gguf(const std::string& path)
    : m_path(path),
      m_file(path, std::ios::binary)
{
    if(!m_file)
        throw exception("could not open " + path);

    char magic[4];

    m_file.read(magic, 4);

    if(!m_file || std::memcmp(magic, "GGUF", 4))
        throw exception(path + " does not begin with GGUF");

    m_version = read_pod<std::uint32_t>();

    if(m_version < 2 || m_version > 3) {
        std::ostringstream o;

        o << "GGUF version " << m_version << ", where this reads 2 and 3";

        throw exception(o.str());
    }

    const std::uint64_t tensor_count = read_pod<std::uint64_t>();
    const std::uint64_t kv_count = read_pod<std::uint64_t>();

    if(tensor_count > MAX_COUNT || kv_count > MAX_COUNT)
        throw exception("a header claiming more tensors or keys than this will "
                        "believe");

    read_metadata(kv_count);
    read_index(tensor_count);

    // The data begins at the next multiple of the alignment after the index.
    const std::uint64_t here = static_cast<std::uint64_t>(m_file.tellg());

    m_data_offset = here + ((m_alignment - (here % m_alignment)) % m_alignment);
}

bool gguf::has(const std::string& key) const {
    return m_meta.find(key) != m_meta.end();
}

const gguf::value& gguf::get(const std::string& key) const {
    std::map<std::string, value>::const_iterator i = m_meta.find(key);

    if(i == m_meta.end())
        throw exception("no metadata key '" + key + "'");

    return i->second;
}

std::string gguf::str(const std::string& key) const {
    const value& v = get(key);

    if(v.type != value_type::string)
        throw exception("metadata key '" + key + "' is not a string");

    return v.s;
}

std::int64_t gguf::integer(const std::string& key) const {
    const value& v = get(key);

    switch(v.type) {
    case value_type::uint8: case value_type::int8:
    case value_type::uint16: case value_type::int16:
    case value_type::uint32: case value_type::int32:
    case value_type::uint64: case value_type::int64:
    case value_type::boolean:
        return v.i;
    default:
        throw exception("metadata key '" + key + "' is not an integer");
    }
}

double gguf::real(const std::string& key) const {
    const value& v = get(key);

    if(v.type == value_type::float32 || v.type == value_type::float64)
        return v.d;

    // An integer where a real was asked for is a widening and not a surprise;
    // the other direction would be a loss and is refused above.
    return double(integer(key));
}

bool gguf::has_tensor(const std::string& name) const {
    return m_by_name.find(name) != m_by_name.end();
}

const gguf::tensor_info& gguf::tensor(const std::string& name) const {
    std::map<std::string, std::size_t>::const_iterator i = m_by_name.find(name);

    if(i == m_by_name.end())
        throw exception("no tensor named '" + name + "'");

    return m_tensors[i->second];
}

std::string gguf::type_name(tensor_type t) {
    switch(t) {
    case tensor_type::f32:  return "f32";
    case tensor_type::f16:  return "f16";
    case tensor_type::q4_0: return "q4_0";
    case tensor_type::q4_1: return "q4_1";
    case tensor_type::q5_0: return "q5_0";
    case tensor_type::q5_1: return "q5_1";
    case tensor_type::q8_0: return "q8_0";
    case tensor_type::q8_1: return "q8_1";
    case tensor_type::q2_k: return "q2_K";
    case tensor_type::q3_k: return "q3_K";
    case tensor_type::q4_k: return "q4_K";
    case tensor_type::q5_k: return "q5_K";
    case tensor_type::q6_k: return "q6_K";
    case tensor_type::q8_k: return "q8_K";
    }

    std::ostringstream o;

    o << "ggml type " << static_cast<int>(t);

    return o.str();
}

math::matrix<float> gguf::read(const tensor_info& t) const {
    const std::uint64_t n = t.elements();

    const unsigned int rows = t.shape.empty()
        ? 1u : static_cast<unsigned int>(t.shape[0]);

    const unsigned int cols = (n && rows)
        ? static_cast<unsigned int>(n / rows) : 0u;

    math::matrix<float> out(rows, cols);

    // Column-major and dims[0]-contiguous are the same arrangement, so the
    // elements arrive in exactly the order the matrix wants them.  See the
    // header.
    float* into = static_cast<math::buffer<float> >(out).data();

    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_data_offset + t.offset));

    if(!m_file)
        throw exception("could not seek to tensor '" + t.name + "'");

    switch(t.type) {
    case tensor_type::f32: {
        m_file.read(reinterpret_cast<char*>(into),
                    static_cast<std::streamsize>(n * sizeof(float)));
        break;
    }

    case tensor_type::f16: {
        std::vector<_Float16> raw(static_cast<std::size_t>(n));

        m_file.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(n * sizeof(_Float16)));

        for(std::uint64_t i = 0; i < n; i++) into[i] = float(raw[i]);

        break;
    }

    case tensor_type::q8_0: {
        if(n % Q8_0_BLOCK)
            throw exception("tensor '" + t.name + "' is q8_0 but its element "
                            "count is not a multiple of the block size");

        const std::uint64_t blocks = n / Q8_0_BLOCK;

        std::vector<char> raw(static_cast<std::size_t>(blocks * Q8_0_BYTES));

        m_file.read(raw.data(), static_cast<std::streamsize>(raw.size()));

        for(std::uint64_t b = 0; b < blocks; b++) {
            const char* p = raw.data() + b * Q8_0_BYTES;

            // The scale, then thirty-two signed bytes scaled by it.  memcpy
            // rather than a cast: the block is two bytes then thirty-two, so
            // nothing after the first is aligned for a _Float16 load.
            _Float16 d;

            std::memcpy(&d, p, sizeof(d));

            const signed char* q = reinterpret_cast<const signed char*>(p + 2);

            for(std::uint64_t i = 0; i < Q8_0_BLOCK; i++)
                into[b * Q8_0_BLOCK + i] = float(d) * float(q[i]);
        }

        break;
    }

    default:
        throw exception("tensor '" + t.name + "' is " + type_name(t.type) +
                        ", which this does not dequantise -- f32, f16 and q8_0 "
                        "are all it reads");
    }

    if(!m_file)
        throw exception("the file ended while reading tensor '" + t.name + "'");

    return out;
}

math::matrix<float> gguf::read(const std::string& name) const {
    return read(tensor(name));
}

std::uint64_t gguf::stored_bytes(tensor_type t, std::uint64_t n) {
    switch(t) {
    case tensor_type::f32:  return n * 4;
    case tensor_type::f16:  return n * 2;
    case tensor_type::q8_0: return (n / Q8_0_BLOCK) * Q8_0_BYTES;
    default: break;
    }

    throw exception("stored_bytes: " + type_name(t) + " has no size here");
}

std::vector<char> gguf::read_raw(const tensor_info& t) const {
    const std::uint64_t n = t.elements();

    if(t.type == tensor_type::q8_0 && (n % Q8_0_BLOCK))
        throw exception("tensor '" + t.name + "' is q8_0 but its element count "
                        "is not a multiple of the block size");

    std::vector<char> raw(static_cast<std::size_t>(stored_bytes(t.type, n)));

    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_data_offset + t.offset));

    if(!m_file)
        throw exception("could not seek to tensor '" + t.name + "'");

    m_file.read(raw.data(), static_cast<std::streamsize>(raw.size()));

    if(!m_file)
        throw exception("the file ended while reading tensor '" + t.name + "'");

    return raw;
}

std::vector<char> gguf::read_raw(const std::string& name) const {
    return read_raw(tensor(name));
}

}
}
