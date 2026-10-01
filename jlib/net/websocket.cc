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

#include <jlib/net/websocket.hh>

#include <jlib/util/util.hh>
#include <jlib/util/utf8.hh>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace jlib {
namespace net {
namespace websocket {

namespace {

/**
 * RFC 6455 4.2.2, written out as the document gives it.
 *
 * Concatenated with the client's key and hashed.  The RFC's own words are that
 * this value "is unlikely to be used by network endpoints that do not
 * understand the WebSocket Protocol" -- it is a shibboleth, not a secret.
 */
const char* const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/**
 * The byte at i, unsigned.
 *
 * **Deliberately not called at().** In the standard library that name is the
 * *bounds-checked* accessor, the one that throws, and this is `operator[]` plus a
 * cast -- unchecked. Every call site is guarded by an explicit size check a line
 * or two above, and a reader who took the name at its word would relax about
 * exactly that. The name should promise only what it delivers.
 *
 * It exists for the cast rather than the subscript. Sign extension is *the*
 * classic defect in code like this: 0x81 is -127 as a signed char on most
 * platforms, so `b0 & 0x70` on a raw char promotes with the sign. Centralising
 * the conversion means the next frame type cannot forget it.
 */
unsigned char byte_at(std::string_view s, std::size_t i)
{
    return static_cast<unsigned char>(s[i]);
}

/** ASCII case-insensitive compare.  Field *values* here are all ASCII tokens. */
bool iequals(std::string_view a, std::string_view b)
{
    if(a.size() != b.size()) return false;

    for(std::size_t i = 0; i < a.size(); i++) {
        unsigned char x = byte_at(a, i), y = byte_at(b, i);

        if(x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
        if(y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');

        if(x != y) return false;
    }

    return true;
}

/**
 * Is token one of the comma-separated items in list?
 *
 * Connection is a list field, and "Connection: keep-alive, Upgrade" is both
 * legal and what a proxy in the path is likely to have made of it -- so an
 * equality test against the whole value is wrong, and was the first thing to
 * get this wrong when it was written that way.
 */
bool listed(std::string_view list, std::string_view token)
{
    std::size_t i = 0;

    while(i <= list.size()) {
        std::size_t comma = list.find(',', i);
        if(comma == std::string_view::npos) comma = list.size();

        std::size_t b = i, e = comma;

        while(b < e && (list[b] == ' ' || list[b] == '\t')) b++;
        while(e > b && (list[e - 1] == ' ' || list[e - 1] == '\t')) e--;

        if(iequals(list.substr(b, e - b), token)) return true;

        i = comma + 1;
    }

    return false;
}

void random_bytes(unsigned char* into, std::size_t n)
{
    // RAND_bytes, not rand() or std::random_device.  A predictable masking key
    // defeats the only thing masking is for -- see encode() -- and there is no
    // reason for a second source of randomness in a library that already links
    // OpenSSL.
    if(RAND_bytes(into, static_cast<int>(n)) != 1)
        throw error("RAND_bytes failed");
}

}

std::string key()
{
    unsigned char raw[16];

    random_bytes(raw, sizeof raw);

    return util::base64::encode(
        std::string(reinterpret_cast<const char*>(raw), sizeof raw));
}

std::string accept(std::string_view k)
{
    const std::string input = std::string(k) + GUID;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    if(EVP_Digest(input.data(), input.size(), hash, &len, EVP_sha1(), 0) != 1)
        throw error("SHA-1 failed");

    return util::base64::encode(
        std::string(reinterpret_cast<const char*>(hash), len));
}

void client_fields(util::http::fields& into, std::string_view k)
{
    into.add("Upgrade", "websocket");
    into.add("Connection", "Upgrade");
    into.add("Sec-WebSocket-Key", std::string(k));
    into.add("Sec-WebSocket-Version", "13");
}

void check_response(const util::http::Response& r, std::string_view k)
{
    // 101 and nothing else.  A 200 here is a server that answered the request
    // as an ordinary GET, which is the common failure and reads nothing like a
    // handshake problem unless it is named.
    if(r.status() != 101) {
        throw error("the server answered " + std::to_string(r.status()) +
                    " rather than 101 Switching Protocols");
    }

    if(!iequals(r.fields().get("Upgrade"), "websocket"))
        throw error("the response does not say Upgrade: websocket");

    if(!listed(r.fields().get("Connection"), "Upgrade"))
        throw error("the response's Connection does not list Upgrade");

    const std::string want = accept(k);

    if(r.fields().get("Sec-WebSocket-Accept") != want) {
        throw error("Sec-WebSocket-Accept does not match the key sent, so "
                    "something in the path is answering rather than the server");
    }
}

bool is_upgrade(const util::http::Request& q, std::string& accept_out)
{
    // Nothing that does not claim to be an upgrade is one.  Asked of every
    // request a mixed server routes, so this has to be cheap and silent.
    if(!q.fields().has("Upgrade")) return false;
    if(!iequals(q.fields().get("Upgrade"), "websocket")) return false;

    // From here on it claims to be a handshake, so a malformed one is an error
    // rather than an ordinary request: answering 101 to it would be worse.
    if(!listed(q.fields().get("Connection"), "Upgrade"))
        throw error("Upgrade: websocket with no Upgrade in Connection");

    if(q.method() != "GET")
        throw error("a handshake must be a GET, not " + q.method());

    const std::string version = q.fields().get("Sec-WebSocket-Version");

    if(version != "13") {
        // 4.4 wants the versions we do support echoed back in the 426, which is
        // the caller's job; this only has to refuse.
        throw error("Sec-WebSocket-Version is \"" + version + "\", not 13");
    }

    const std::string k = q.fields().get("Sec-WebSocket-Key");

    if(k.empty())
        throw error("a handshake with no Sec-WebSocket-Key");

    // 4.1: the key decodes to exactly 16 bytes.  Checked because a key of the
    // wrong length still produces a perfectly good Accept, so without this the
    // handshake completes and the only sign is that nothing else noticed.
    bool clean = true;

    if(util::base64::decode(k, clean).size() != 16 || !clean)
        throw error("Sec-WebSocket-Key is not 16 base64-encoded bytes");

    accept_out = accept(k);

    return true;
}

std::string encode(const frame& f, sender as)
{
    std::uint32_t k = 0;

    if(masked(as)) {
        unsigned char raw[4];

        random_bytes(raw, sizeof raw);

        k = (static_cast<std::uint32_t>(raw[0]) << 24) |
            (static_cast<std::uint32_t>(raw[1]) << 16) |
            (static_cast<std::uint32_t>(raw[2]) << 8) |
            static_cast<std::uint32_t>(raw[3]);
    }

    return encode(f, as, k);
}

std::string encode(const frame& f, sender as, std::uint32_t masking_key)
{
    if(control(f.op)) {
        if(f.payload.size() > 125)
            throw error("a control frame may carry at most 125 bytes (5.5)");
        if(!f.fin)
            throw error("a control frame may not be fragmented (5.5)");
    }

    std::string out;

    out += static_cast<char>((f.fin ? 0x80 : 0x00) |
                             static_cast<std::uint8_t>(f.op));

    const std::size_t n = f.payload.size();
    const unsigned char maskbit = masked(as) ? 0x80 : 0x00;

    // The shortest form that fits.  5.2 requires it rather than permitting it,
    // which is why decode() refuses the longer spellings.
    if(n < 126) {
        out += static_cast<char>(maskbit | static_cast<unsigned char>(n));
    }
    else if(n <= 0xFFFF) {
        out += static_cast<char>(maskbit | 126);
        out += static_cast<char>((n >> 8) & 0xFF);
        out += static_cast<char>(n & 0xFF);
    }
    else {
        out += static_cast<char>(maskbit | 127);

        for(int shift = 56; shift >= 0; shift -= 8)
            out += static_cast<char>((static_cast<std::uint64_t>(n) >> shift) & 0xFF);
    }

    unsigned char kb[4] = { 0, 0, 0, 0 };

    if(masked(as)) {
        for(int i = 0; i < 4; i++)
            kb[i] = static_cast<unsigned char>((masking_key >> (24 - 8 * i)) & 0xFF);

        out.append(reinterpret_cast<const char*>(kb), 4);
    }

    if(!masked(as)) {
        out += f.payload;
    }
    else {
        // 5.3: octet i of the payload is XORed with octet i modulo 4 of the key.
        for(std::size_t i = 0; i < n; i++)
            out += static_cast<char>(byte_at(f.payload, i) ^ kb[i % 4]);
    }

    return out;
}

std::size_t decode(std::string_view in, frame& into, sender from,
                   std::size_t max_payload)
{
    if(in.size() < 2) return 0;

    const unsigned char b0 = byte_at(in, 0);
    const unsigned char b1 = byte_at(in, 1);

    // RSV1-3.  Set without a negotiated extension to give them meaning, which
    // nothing here negotiates, so they can only be a peer talking a protocol
    // this is not -- 5.2 says fail the connection.
    if(b0 & 0x70)
        throw error("a reserved bit is set and no extension was negotiated");

    const std::uint8_t raw_op = b0 & 0x0F;

    switch(raw_op) {
        case 0x0: case 0x1: case 0x2:
        case 0x8: case 0x9: case 0xA:
            break;
        default:
            throw error("reserved opcode " + std::to_string(raw_op));
    }

    const bool fin = (b0 & 0x80) != 0;
    const bool has_mask = (b1 & 0x80) != 0;

    // 5.1 in both directions.  A decoder that unmasked whatever it was handed
    // would accept a masked server frame and an unmasked client one, and this
    // is the only check the RFC asks for here.
    const bool want = websocket::masked(from);

    if(has_mask != want) {
        throw error(want ? "an unmasked frame from a client (5.1)"
                         : "a masked frame from a server (5.1)");
    }

    std::uint64_t len = b1 & 0x7F;
    std::size_t at_byte = 2;

    if(len == 126) {
        if(in.size() < at_byte + 2) return 0;

        len = (static_cast<std::uint64_t>(byte_at(in, 2)) << 8) | byte_at(in, 3);
        at_byte = 4;

        if(len < 126)
            throw error("a 16-bit length that fits in 7 bits (5.2)");
    }
    else if(len == 127) {
        if(in.size() < at_byte + 8) return 0;

        // 5.2: the most significant bit MUST be 0.
        if(byte_at(in, 2) & 0x80)
            throw error("a 64-bit length with the top bit set (5.2)");

        len = 0;

        for(std::size_t i = 2; i < 10; i++)
            len = (len << 8) | byte_at(in, i);

        at_byte = 10;

        if(len <= 0xFFFF)
            throw error("a 64-bit length that fits in 16 bits (5.2)");
    }

    if(control(static_cast<opcode>(raw_op))) {
        if(len > 125)
            throw error("a control frame carrying more than 125 bytes (5.5)");
        if(!fin)
            throw error("a fragmented control frame (5.5)");
    }

    // Before reserving anything: a 64-bit length is a peer's number, and the
    // point of the cap is to refuse it rather than to try to hold it.
    if(len > max_payload) {
        throw error("a frame of " + std::to_string(len) +
                    " bytes, over the " + std::to_string(max_payload) +
                    " byte cap");
    }

    unsigned char kb[4] = { 0, 0, 0, 0 };

    if(has_mask) {
        if(in.size() < at_byte + 4) return 0;

        for(int i = 0; i < 4; i++)
            kb[i] = byte_at(in, at_byte + static_cast<std::size_t>(i));

        at_byte += 4;
    }

    const std::size_t n = static_cast<std::size_t>(len);

    if(in.size() < at_byte + n) return 0;

    into.fin = fin;
    into.op = static_cast<opcode>(raw_op);
    into.payload.clear();
    into.payload.reserve(n);

    if(!has_mask) {
        into.payload.assign(in.substr(at_byte, n));
    }
    else {
        for(std::size_t i = 0; i < n; i++)
            into.payload += static_cast<char>(byte_at(in, at_byte + i) ^ kb[i % 4]);
    }

    return at_byte + n;
}

std::string close_payload(close_code code, std::string_view reason)
{
    if(code == close_code::no_status) {
        throw error("1005 means no status was received and may not be sent "
                    "(7.4.1); send an empty payload instead");
    }

    // Before truncating, not after: cutting first would reject input that was
    // fine and got cut, and the message would blame the caller for our cut.
    if(!util::utf8_valid(reason)) {
        throw error("a close reason that is not valid UTF-8 (5.5.1); the peer "
                    "is required to fail the connection over it");
    }

    const std::uint16_t c = static_cast<std::uint16_t>(code);

    std::string out;

    out += static_cast<char>((c >> 8) & 0xFF);
    out += static_cast<char>(c & 0xFF);

    // 125 for the whole control payload, two of which are the code.
    std::string text(reason.substr(0, reason.size() < 123 ? reason.size() : 123));

    // Truncating to a byte count can land inside a character, and the result has
    // to be valid UTF-8 or the peer fails the connection over our close frame.
    while(!text.empty() && util::utf8_ends_mid_character(text))
        text.pop_back();

    return out + text;
}

close_code close_reason(std::string_view payload, std::string& reason_out)
{
    reason_out.clear();

    // 7.1.5: an empty payload is legal and means no status was given.
    if(payload.empty()) return close_code::no_status;

    // One byte cannot be a code, and 5.5.1 allows no other reading of it.
    if(payload.size() == 1)
        throw error("a close frame with a one-byte payload (5.5.1)");

    const std::uint16_t c = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte_at(payload, 0)) << 8) | byte_at(payload, 1));

    reason_out.assign(payload.substr(2));

    // 5.5.1 says the reason is UTF-8 text, and 8.1 makes invalid UTF-8 a
    // failure.  Checked here rather than by the caller because a caller that
    // forgot would log a reason that is not text.
    if(!util::utf8_valid(reason_out))
        throw error("a close frame whose reason is not valid UTF-8 (5.5.1)");

    return static_cast<close_code>(c);
}

}
}
}
