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

/**
 * jlib::net::websocket, against RFC 6455's own bytes.
 *
 * The reason this test can be strong is that the RFC publishes vectors: 1.3
 * gives a handshake key and the Accept it must produce, and 5.7 writes out seven
 * example frames byte by byte.  So the codec is checked against the document
 * rather than against itself -- encode() must produce exactly those bytes and
 * decode() must read exactly those bytes back, which is a different and better
 * assertion than a round trip, since a round trip passes for two bugs that
 * cancel.
 *
 * No socket anywhere in here.  That is the point of keeping framing separate
 * from transport.
 */

#include <jlib/net/websocket.hh>
#include <jlib/util/utf8.hh>

#include <initializer_list>
#include <iostream>
#include <string>

using namespace jlib;
using namespace jlib::net;

// 5.1 ties masking to the end that sent the frame, so an unmasked frame is a
// server's and a masked one is a client's.  Spelled short because these appear
// in nearly every assertion below.
static const websocket::sender CLIENT = websocket::sender::client;
static const websocket::sender SERVER = websocket::sender::server;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** A byte string from a list of octets, so a test reads like the RFC's table. */
static std::string bytes(std::initializer_list<int> octets) {
    std::string out;

    for(int b : octets) out += static_cast<char>(static_cast<unsigned char>(b));

    return out;
}

static std::string hex(const std::string& s) {
    static const char* const digits = "0123456789abcdef";

    std::string out;

    for(std::size_t i = 0; i < s.size() && i < 24; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        out += digits[c >> 4];
        out += digits[c & 0x0F];
        out += ' ';
    }

    if(s.size() > 24) out += "...";

    return out;
}

/** Did this throw websocket::error?  The protocol failures all must. */
template<typename F>
static bool throws(F f) {
    try { f(); }
    catch(const websocket::error&) { return true; }
    catch(...) { return false; }

    return false;
}

static void the_handshake() {
    std::cout << "\nthe handshake, RFC 6455 1.3:\n";

    // The document's own example: this key, that Accept.  A single pinned pair
    // is enough because the function is SHA-1 of a concatenation -- there is no
    // state for a second vector to reach.
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string want = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    ok("  1.3's key produces 1.3's Sec-WebSocket-Accept",
       websocket::accept(key) == want, websocket::accept(key));

    // The GUID is appended to the key *as sent*, not to its decoded bytes, so
    // decoding first would be a plausible and wrong implementation that this
    // catches.
    ok("  a different key produces a different accept",
       websocket::accept("dGhlIHNhbXBsZSBub25jZQ=") != want);

    const std::string k = websocket::key();

    ok("  key() is 24 characters, being 16 bytes base64", k.size() == 24, k);
    ok("  and two of them differ", websocket::key() != websocket::key());

    util::http::fields f;

    websocket::client_fields(f, k);

    ok("  client_fields sets Upgrade", f.get("Upgrade") == "websocket");
    ok("  and Connection", f.get("Connection") == "Upgrade");
    ok("  and the key it was given", f.get("Sec-WebSocket-Key") == k);
    ok("  and version 13", f.get("Sec-WebSocket-Version") == "13");
}

static void the_published_frames() {
    std::cout << "\nthe seven example frames, RFC 6455 5.7:\n";

    // 1. A single-frame unmasked text message.
    {
        const std::string want = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
        const websocket::frame f{true, websocket::opcode::text, "Hello"};

        ok("  unmasked text \"Hello\" encodes to 81 05 48 65 6c 6c 6f",
           websocket::encode(f, SERVER) == want, hex(websocket::encode(f, SERVER)));

        websocket::frame got;

        const std::size_t n = websocket::decode(want, got, SERVER);

        ok("  and decodes back", n == want.size() && got.fin &&
           got.op == websocket::opcode::text && got.payload == "Hello");
    }

    // 2. The same message masked, with 5.7's masking key 0x37fa213d.
    {
        const std::string want = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                        0x7f, 0x9f, 0x4d, 0x51, 0x58});
        const websocket::frame f{true, websocket::opcode::text, "Hello"};

        ok("  masked text with 5.7's key encodes to its exact bytes",
           websocket::encode(f, CLIENT, 0x37fa213d) == want,
           hex(websocket::encode(f, CLIENT, 0x37fa213d)));

        websocket::frame got;

        ok("  and unmasks back to Hello",
           websocket::decode(want, got, CLIENT) == want.size() &&
           got.payload == "Hello", got.payload);
    }

    // 3. A fragmented message: "Hel" then "lo".
    {
        const std::string first = bytes({0x01, 0x03, 0x48, 0x65, 0x6c});
        const std::string last  = bytes({0x80, 0x02, 0x6c, 0x6f});

        const websocket::frame a{false, websocket::opcode::text, "Hel"};
        const websocket::frame b{true, websocket::opcode::continuation, "lo"};

        ok("  the first fragment is text with fin clear",
           websocket::encode(a, SERVER) == first, hex(websocket::encode(a, SERVER)));
        ok("  the last is a continuation with fin set",
           websocket::encode(b, SERVER) == last, hex(websocket::encode(b, SERVER)));

        websocket::frame got;

        websocket::decode(first, got, SERVER);

        ok("  decoding the first reports fin clear and opcode text",
           !got.fin && got.op == websocket::opcode::text && got.payload == "Hel");

        websocket::decode(last, got, SERVER);

        ok("  and the last reports continuation",
           got.fin && got.op == websocket::opcode::continuation &&
           got.payload == "lo");
    }

    // 4 and 5. An unmasked ping and the masked pong answering it.
    {
        const std::string ping = bytes({0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
        const std::string pong = bytes({0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                        0x7f, 0x9f, 0x4d, 0x51, 0x58});

        const websocket::frame p{true, websocket::opcode::ping, "Hello"};
        const websocket::frame q{true, websocket::opcode::pong, "Hello"};

        ok("  an unmasked ping", websocket::encode(p, SERVER) == ping,
           hex(websocket::encode(p, SERVER)));
        ok("  a masked pong", websocket::encode(q, CLIENT, 0x37fa213d) == pong,
           hex(websocket::encode(q, CLIENT, 0x37fa213d)));
    }

    // 6. 256 bytes of binary in one unmasked frame: the 16-bit length.
    {
        const std::string payload(256, '\x01');
        const websocket::frame f{true, websocket::opcode::binary, payload};
        const std::string out = websocket::encode(f, SERVER);

        ok("  256 binary bytes use the 16-bit length, 82 7E 01 00",
           out.substr(0, 4) == bytes({0x82, 0x7E, 0x01, 0x00}) &&
           out.size() == 4 + 256, hex(out));

        websocket::frame got;

        ok("  and decode returns all 256",
           websocket::decode(out, got, SERVER) == out.size() &&
           got.payload == payload);
    }

    // 7. 65536 bytes: the 64-bit length.
    {
        const std::string payload(65536, '\x01');
        const websocket::frame f{true, websocket::opcode::binary, payload};
        const std::string out = websocket::encode(f, SERVER);

        ok("  65536 binary bytes use the 64-bit length, 82 7F 00..01 00 00",
           out.substr(0, 10) == bytes({0x82, 0x7F, 0, 0, 0, 0, 0, 0x01, 0, 0}) &&
           out.size() == 10 + 65536, hex(out));

        websocket::frame got;

        ok("  and decode returns all 65536",
           websocket::decode(out, got, SERVER) == out.size() &&
           got.payload == payload);
    }
}

static void a_partial_frame_is_not_an_error() {
    std::cout << "\nan incomplete frame returns 0 rather than throwing:\n";

    const std::string whole = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});

    // Every prefix. This is the normal case on a stream and the one a reader
    // depends on: 0 means "ask me again with more", and anything else -- an
    // exception, or a frame with a short payload -- would turn a slow network
    // into a protocol error.
    bool all_zero = true;

    for(std::size_t i = 0; i < whole.size(); i++) {
        websocket::frame got;

        if(websocket::decode(whole.substr(0, i), got, SERVER) != 0) all_zero = false;
    }

    ok("  all six proper prefixes of a 7-byte frame return 0", all_zero);

    websocket::frame got;

    ok("  and the whole thing returns 7",
       websocket::decode(whole, got, SERVER) == whole.size());

    // A frame followed by the start of the next: consume only the first.
    const std::string two = whole + bytes({0x81, 0x02, 0x68});

    ok("  a trailing partial frame does not extend the first",
       websocket::decode(two, got, SERVER) == whole.size() &&
       got.payload == "Hello");
}

static void the_protocol_failures() {
    std::cout << "\nwhat the RFC says to fail the connection over:\n";

    websocket::frame got;

    // 5.2: RSV1-3 with no extension negotiated.
    ok("  a reserved bit set", throws([&] {
        websocket::decode(bytes({0xC1, 0x00}), got, SERVER);
    }));

    // 5.2: opcodes 3-7 and B-F are reserved.
    ok("  a reserved opcode", throws([&] {
        websocket::decode(bytes({0x83, 0x00}), got, SERVER);
    }));

    // 5.5: a control frame may not be fragmented...
    ok("  a fragmented control frame", throws([&] {
        websocket::decode(bytes({0x09, 0x00}), got, SERVER);
    }));

    // ...nor carry more than 125 bytes.  0x7E is 126, which is already too many.
    ok("  a control frame with a 126-byte length", throws([&] {
        websocket::decode(bytes({0x89, 0x7E, 0x00, 0x7E}), got, SERVER);
    }));

    // 5.2: the length must be in its shortest form.  Both of these describe a
    // payload that a shorter field could have carried, and accepting them is how
    // two peers end up disagreeing about where a frame ends.
    ok("  a 16-bit length that fits in 7 bits", throws([&] {
        websocket::decode(bytes({0x81, 0x7E, 0x00, 0x05}), got, SERVER);
    }));

    ok("  a 64-bit length that fits in 16 bits", throws([&] {
        websocket::decode(bytes({0x81, 0x7F, 0, 0, 0, 0, 0, 0, 0x01, 0x00}),
                          got, SERVER);
    }));

    ok("  a 64-bit length with the top bit set", throws([&] {
        websocket::decode(bytes({0x81, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0}),
                          got, SERVER);
    }));

    // 5.1, in both directions.  THESE TWO ARE THE LOAD-BEARING ONES: a decoder
    // that just unmasked whatever arrived would pass every other assertion in
    // this file and silently drop the only masking rule the RFC states.
    ok("  an unmasked frame where a server expects a masked one", throws([&] {
        websocket::decode(bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f}),
                          got, CLIENT);
    }));

    ok("  a masked frame where a client expects an unmasked one", throws([&] {
        websocket::decode(bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                 0x7f, 0x9f, 0x4d, 0x51, 0x58}), got, SERVER);
    }));

    // The cap is checked against the *declared* length, before anything is
    // reserved, so a peer cannot make this allocate by lying.
    ok("  a declared length over the cap", throws([&] {
        websocket::decode(bytes({0x82, 0x7E, 0x10, 0x00}), got, SERVER, 1024);
    }));

    // And encode refuses what a caller should not be able to send.
    ok("  encoding an over-long control frame", throws([&] {
        websocket::encode({true, websocket::opcode::ping, std::string(126, 'x')}, SERVER);
    }));

    ok("  encoding a fragmented control frame", throws([&] {
        websocket::encode({false, websocket::opcode::pong, ""}, SERVER);
    }));
}

/** A Request built the only way there is: by parsing a real request head. */
static util::http::Request req(const std::string& head) {
    return util::http::parse_request_head(head);
}

static std::string handshake_head(const std::string& version,
                                  const std::string& key,
                                  const std::string& method = "GET",
                                  const std::string& connection = "keep-alive, Upgrade") {
    return method + " /chat HTTP/1.1\r\n"
           "Host: example.com\r\n"
           "Upgrade: websocket\r\n"
           "Connection: " + connection + "\r\n"
           "Sec-WebSocket-Key: " + key + "\r\n"
           "Sec-WebSocket-Version: " + version + "\r\n\r\n";
}

static void the_server_side_handshake() {
    std::cout << "\nis_upgrade, which every request on a mixed server meets:\n";

    const std::string KEY = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string ACC = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    std::string acc;

    // An ordinary request is not an upgrade, and must not throw -- a server
    // routing both kinds asks this question of everything it serves.
    {
        util::http::Request q = req("GET /index.html HTTP/1.1\r\n"
                                    "Host: example.com\r\n\r\n");

        ok("  a plain GET is not an upgrade and does not throw",
           !throws([&] { websocket::is_upgrade(q, acc); }) &&
           !websocket::is_upgrade(q, acc));
    }

    {
        util::http::Request q = req(handshake_head("13", KEY));

        ok("  a well-formed handshake is recognised",
           websocket::is_upgrade(q, acc));

        // Connection arrived as "keep-alive, Upgrade" -- a list, which is what a
        // proxy in the path is likely to leave behind. An equality test against
        // the whole field value fails this, which is the bug the list walk in
        // listed() exists for.
        ok("  with Upgrade found inside a Connection list", acc == ACC, acc);
    }

    ok("  Upgrade: websocket with no Upgrade in Connection throws", throws([&] {
        util::http::Request q = req(handshake_head("13", KEY, "GET", "keep-alive"));
        websocket::is_upgrade(q, acc);
    }));

    ok("  a version other than 13 throws", throws([&] {
        util::http::Request q = req(handshake_head("8", KEY));
        websocket::is_upgrade(q, acc);
    }));

    // A key of the wrong length still yields a perfectly good Accept, so without
    // the length check the handshake completes and nothing notices.
    ok("  a key that is not 16 bytes throws", throws([&] {
        util::http::Request q = req(handshake_head("13", "c2hvcnQ="));
        websocket::is_upgrade(q, acc);
    }));

    ok("  a handshake that is not a GET throws", throws([&] {
        util::http::Request q = req(handshake_head("13", KEY, "POST"));
        websocket::is_upgrade(q, acc);
    }));

    std::cout << "\ncheck_response, from the client's side:\n";

    auto response = [](const std::string& text) {
        return util::http::parse_head(text);
    };

    const std::string good = "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + ACC + "\r\n\r\n";

    ok("  a correct 101 is accepted",
       !throws([&] { websocket::check_response(response(good), KEY); }));

    // The common failure by a mile: the server treated it as an ordinary GET.
    ok("  a 200 throws, and says it was not 101", throws([&] {
        websocket::check_response(
            response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"), KEY);
    }));

    // THE LOAD-BEARING ONE on this side: an intermediary that echoes the
    // upgrade fields but cannot compute the Accept gets refused. That is the
    // entire purpose of the key/accept exchange, which is not a security
    // mechanism and is a "did you actually understand me" check.
    ok("  a wrong Sec-WebSocket-Accept throws", throws([&] {
        websocket::check_response(
            response("HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),
            KEY);
    }));

    ok("  a 101 with no Upgrade field throws", throws([&] {
        websocket::check_response(
            response("HTTP/1.1 101 Switching Protocols\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " + ACC + "\r\n\r\n"), KEY);
    }));
}

static void closing() {
    std::cout << "\nclose frames, 5.5.1 and 7.4.1:\n";

    std::string reason;

    ok("  a code and a reason round-trip",
       websocket::close_reason(
           websocket::close_payload(websocket::close_code::going_away, "bye"),
           reason) == websocket::close_code::going_away && reason == "bye",
       reason);

    ok("  an empty payload reads as 1005, no status received",
       websocket::close_reason("", reason) == websocket::close_code::no_status &&
       reason.empty());

    ok("  and 1005 may not be sent", throws([&] {
        websocket::close_payload(websocket::close_code::no_status);
    }));

    ok("  a one-byte payload throws", throws([&] {
        websocket::close_reason(std::string(1, '\x03'), reason);
    }));

    // The reason is truncated to fit a control frame, and on a character
    // boundary -- a close frame whose own reason is invalid UTF-8 would be
    // failed by the peer, which is a poor way to say goodbye.
    {
        const std::string pizza = "\xF0\x9F\x8D\x95";   // U+1F355, four bytes
        std::string long_reason;

        while(long_reason.size() < 200) long_reason += pizza;

        const std::string p =
            websocket::close_payload(websocket::close_code::normal, long_reason);

        ok("  an over-long reason is cut to 125 bytes of payload",
           p.size() <= 125, std::to_string(p.size()));
        ok("  and cut on a character boundary, so it is still valid UTF-8",
           util::utf8_valid(p.substr(2)));
    }

    ok("  a received reason that is not valid UTF-8 throws", throws([&] {
        websocket::close_reason(bytes({0x03, 0xE8, 0xFF, 0xFE}), reason);
    }));

    // The write side has to refuse the same bytes the read side does, or we send
    // a close frame the peer is required to fail the connection over -- and the
    // only report of a local, synchronous mistake is a remote close code during
    // shutdown.  Asymmetric until juliet pointed it out.
    ok("  and so does a reason being sent", throws([&] {
        websocket::close_payload(websocket::close_code::normal, "\xFF\xFE");
    }));

    // Validated before truncating: a 200-byte valid reason is cut, not refused.
    ok("  a long but valid reason is cut rather than refused",
       !throws([&] {
           websocket::close_payload(websocket::close_code::normal,
                                    std::string(200, 'x'));
       }));

    // And this is what pins the *order*.  The bad byte sits past the 123-byte
    // cut, so an implementation that truncated first and validated after would
    // throw it away and accept the call -- sending something other than what was
    // asked for, which is the failure mode "strip to valid" has in general.
    ok("  an invalid byte beyond the cut still throws", throws([&] {
        websocket::close_payload(websocket::close_code::normal,
                                 std::string(200, 'x') + "\xFF");
    }));
}

static void utf8_validation() {
    std::cout << "\nutil::utf8_valid, which 8.1 needs and utf8_stream is not:\n";

    ok("  ascii", util::utf8_valid("hello"));
    ok("  empty", util::utf8_valid(""));
    ok("  two, three and four byte sequences",
       util::utf8_valid("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8D\x95"));

    // The three strictness cases, each of which utf8_stream deliberately lets
    // through because it is answering a different question.
    ok("  an overlong two-byte form is refused",
       !util::utf8_valid("\xC0\xAF"));
    ok("  an overlong three-byte form is refused",
       !util::utf8_valid("\xE0\x80\xAF"));
    ok("  a surrogate, D800, is refused",
       !util::utf8_valid("\xED\xA0\x80"));
    ok("  a value past U+10FFFF is refused",
       !util::utf8_valid("\xF4\x90\x80\x80"));
    ok("  a lone continuation byte is refused", !util::utf8_valid("\x80"));
    ok("  a truncated sequence is refused", !util::utf8_valid("\xE2\x82"));
    ok("  F5 cannot start a sequence", !util::utf8_valid("\xF5\x80\x80\x80"));

    // And the boundary cases that must be accepted, since refusing them would
    // reject valid messages: the lowest and highest of each length.
    ok("  U+0080, the lowest two-byte", util::utf8_valid("\xC2\x80"));
    ok("  U+07FF, the highest two-byte", util::utf8_valid("\xDF\xBF"));
    ok("  U+0800, the lowest three-byte", util::utf8_valid("\xE0\xA0\x80"));
    ok("  U+FFFF, the highest three-byte", util::utf8_valid("\xEF\xBF\xBF"));
    ok("  U+10000, the lowest four-byte", util::utf8_valid("\xF0\x90\x80\x80"));
    ok("  U+10FFFF, the highest", util::utf8_valid("\xF4\x8F\xBF\xBF"));

    // A text frame may contain NUL, which is why this is not xml's validator.
    ok("  a NUL is valid UTF-8, whatever XML thinks",
       util::utf8_valid(std::string(1, '\0')));
}

int main() {
    the_handshake();
    the_published_frames();
    a_partial_frame_is_not_an_error();
    the_protocol_failures();
    the_server_side_handshake();
    closing();
    utf8_validation();

    // What a green run does not establish.
    //
    // **Conformance.** The Autobahn test suite is what settles that, and it has
    // not been run; it reaches cases no hand-written test thinks of, which is
    // the same argument the *_live_test programs make about real servers. Until
    // then this says the published vectors are right and the failures named in
    // 5.1, 5.2 and 5.5 are caught -- not that nothing else is wrong.
    //
    // **Nothing above touches a socket**, by design, so it says nothing about
    // reading a frame split across two reads in practice, about a peer that
    // stops mid-header, or about the close handshake as a sequence rather than
    // as two payloads. That is the reader and writer layer, which does not exist
    // yet (#399).
    //
    // **No message reassembly**, because decode() returns frames. A fragmented
    // message interleaved with a ping is legal (5.4) and the rules for it --
    // a continuation with no first frame, a first frame while one is open --
    // belong to the layer above and are untested because unwritten.
    //
    // **key() is not tested for randomness**, only that two calls differ and
    // the length is right. It comes from RAND_bytes; a test that could tell a
    // CSPRNG from a bad one does not fit in a file like this.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
