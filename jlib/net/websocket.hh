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

#ifndef JLIB_NET_WEBSOCKET_HH
#define JLIB_NET_WEBSOCKET_HH

#include <jlib/util/http.hh>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace jlib {
namespace net {

/**
 * RFC 6455 WebSocket: the handshake, and the frame codec.
 *
 * ## Two layers, and only the lower one is here
 *
 * This header is **framing without transport**: encode() turns a frame into
 * bytes and decode() turns bytes back into a frame, and neither touches a
 * socket.  That is the same split the RFC work already made between framing and
 * parsing, for the same reason -- a codec that needs a connection to be tested
 * is harder to trust than one that does not, and every assertion in
 * net_websocket_test runs against a std::string.
 *
 * The reader and writer over a stream, and the suspending twin on
 * sys::async_reader that net::http::server wants, come above this.
 *
 * ## The handshake is HTTP, so it is not reimplemented
 *
 * accept() computes the one value a server has to return and client_request()
 * and check_response() do the rest with util::http::fields, because the opening
 * handshake really is an HTTP request and response and jlib already reads both
 * against the RFC 9112 grammar.  There is no second header parser here.
 *
 * ## wss is not a special case
 *
 * sys::sslstream is already a stream.  Nothing in this file knows or asks
 * whether the bytes it is handed arrived over TLS, which is why there is no
 * wss:// anything below.
 *
 * ## Not in this pass
 *
 * No permessage-deflate (RFC 7692): it needs zlib, which is #233's open
 * question, and an unextended connection is a complete one -- a peer offering
 * the extension gets no agreement back and sends uncompressed frames.  No
 * subprotocol negotiation beyond echoing one choice back.  No autobahn run yet,
 * which is the thing that would actually establish conformance (#399).
 */
namespace websocket {

class error : public std::exception {
public:
    error(const std::string& msg = "") { m_msg = "websocket error: " + msg; }
    virtual ~error() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};

/**
 * RFC 6455 5.2's opcodes, by their own numbers.
 *
 * continuation is 0 rather than a flag because that is how it is on the wire: a
 * fragmented message is a first frame carrying the type, then continuations,
 * and only the last has fin set.
 */
enum class opcode : std::uint8_t {
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA
};

/** Is this one of the control opcodes?  RFC 6455 5.5: the high bit says so. */
inline bool control(opcode op)
{
    return (static_cast<std::uint8_t>(op) & 0x8) != 0;
}

/**
 * Which end produced a frame, which is the only thing that decides masking.
 *
 * RFC 6455 5.1: a client MUST mask every frame it sends and a server MUST NOT,
 * so "masked" and "came from the client" are the same fact written two ways.
 * One enum serves encode() and decode() because of that -- encode(f, client)
 * means "I am the client, so mask", decode(in, f, client) means "a client sent
 * this, so it must be masked", and in both the mapping is the same.
 *
 * **An enum rather than a bool, and rather than two functions.** The bool came
 * first and was wrong for the reason util/util.hh gives about base64: a flag at
 * a call site says nothing about which rule it selects.  Two named functions
 * would fix that and are still wrong, because **the role is a property of the
 * connection, not of the call** -- a server decodes every frame of a connection
 * as from-a-client, for that connection's whole life, and never varies it.  So
 * when the reader and writer layer arrives this becomes a constructor argument
 * and disappears from the call sites entirely; an enum survives that move and a
 * pair of functions would have to be deleted.
 */
enum class sender { client, server };

/** Does a frame from this end carry a masking key?  5.1: a client's does. */
inline bool masked(sender s)
{
    return s == sender::client;
}

/**
 * One frame, after the header has been read and the payload unmasked.
 *
 * Deliberately not "one message": reassembling fragments is the layer above,
 * and it needs to see fin and the continuation opcode to do it.  A decode()
 * that returned messages could not report a ping arriving between two
 * fragments, which RFC 6455 5.4 explicitly permits.
 */
struct frame {
    bool fin = true;
    opcode op = opcode::text;
    std::string payload;
};

/**
 * RFC 6455 7.4.1's close codes, the subset a server here can send.
 *
 * going_away and the 1007/1009 pair are the ones the framing layer itself
 * decides: a text payload that is not UTF-8 is 1007 by 8.1, and a frame past
 * the configured cap is 1009.
 */
enum class close_code : std::uint16_t {
    /**
     * 7.1.5's "no status received", which may never appear **on** the wire.
     *
     * An empty close payload is legal and means this.  close_reason() returns
     * it so that a caller has one thing to switch on rather than a code plus a
     * had-a-code flag; close_payload() refuses it, because writing 1005 into a
     * frame is exactly what 7.4.1 forbids.
     */
    no_status       = 1005,

    normal          = 1000,
    going_away      = 1001,
    protocol_error  = 1002,
    unacceptable    = 1003,
    invalid_payload = 1007,
    policy          = 1008,
    too_big         = 1009,
    internal_error  = 1011
};

/**
 * A fresh Sec-WebSocket-Key: 16 random bytes, base64.
 *
 * RFC 6455 4.1 asks for a nonce "selected randomly".  It is not a secret and
 * not a challenge in the cryptographic sense -- its whole job is to make a
 * cached or confused intermediary's response fail the check in check_response()
 * -- but it comes from the same CSPRNG as everything else here rather than
 * rand(), because there is no reason for a second source of randomness in one
 * library.
 */
std::string key();

/**
 * The Sec-WebSocket-Accept for a given key: base64(SHA-1(key + GUID)).
 *
 * The GUID is 258EAFA5-E914-47DA-95CA-C5AB0DC85B11, written out in the source
 * exactly as RFC 6455 4.2.2 gives it.  SHA-1 here is not a security claim and
 * the RFC says as much; it is a fixed function both ends compute so that a
 * server which merely echoes headers cannot accidentally complete a handshake.
 *
 * The input is the key *as it appeared in the field*, not the decoded bytes.
 */
std::string accept(std::string_view key);

/**
 * Fill in the four fields an opening handshake needs, given a key().
 *
 * Upgrade, Connection, Sec-WebSocket-Key and Sec-WebSocket-Version: 13.  The
 * caller owns the request line and the Host, because this does not know whether
 * it is talking through a proxy.
 */
void client_fields(util::http::fields& into, std::string_view key);

/**
 * Does this response complete the handshake for the key we sent?
 *
 * Checks the status is 101, that Upgrade is websocket and Connection contains
 * Upgrade (both case-insensitively, as RFC 6455 4.1 requires), and that
 * Sec-WebSocket-Accept is exactly accept(key).  Throws rather than returning
 * false, because every one of those failures is a different sentence and a
 * caller that gets a bare false has to guess which.
 */
void check_response(const util::http::Response& r, std::string_view key);

/**
 * Is this request an upgrade to WebSocket, and if so what should be returned?
 *
 * Returns false for an ordinary request rather than throwing, because a server
 * routing a mixture of both asks this question of everything.  A request that
 * *claims* to be an upgrade and is malformed -- no key, wrong version -- throws,
 * since answering 101 to it would be worse than answering 400.
 */
bool is_upgrade(const util::http::Request& q, std::string& accept_out);

/**
 * Encode one frame.
 *
 * as says which end is sending, and there is no default, so a caller has to
 * have decided: RFC 6455 5.1 makes masking a MUST in both directions, and a
 * server that masks is as wrong as a client that does not.
 *
 * The masking key comes from the same CSPRNG as key().  Masking exists to stop
 * a client from choosing the bytes an intermediary sees, so a predictable key
 * defeats the point of it entirely.
 *
 * The length is written in the shortest form that fits, which RFC 6455 5.2
 * requires rather than merely allows -- see decode(), which refuses the longer
 * spellings of a short length.
 */
std::string encode(const frame& f, sender as);

/** As encode(), with the masking key given, so a test can match the RFC's bytes. */
std::string encode(const frame& f, sender as, std::uint32_t masking_key);

/**
 * Decode one frame from the front of in.
 *
 * Returns the number of bytes consumed, or **0 if in does not yet hold a whole
 * frame** -- which is not an error and is the normal case on a stream.  A caller
 * appends what it read and asks again.
 *
 * 0 is never ambiguous, and the reason is stronger than "a zero-length frame
 * still consumes two bytes": every `return 0` is a prefix check, and the
 * **minimum complete frame is 2 bytes** unmasked or 6 masked, so no complete
 * frame of any shape can consume 0.  Nor is an error swallowed by it -- a
 * reserved bit in the first byte returns 0 while only one byte is present and
 * throws when the second arrives, so a diagnosis is postponed and not lost.
 *
 * What 0 does not carry is *how much more is needed*, so a caller feeding one
 * byte at a time re-parses the header on every call -- O(n) parses for an n-byte
 * frame.  That is the reader layer's problem to solve with a needed-bytes hint,
 * not this function's; noted here because the shape of the return is what
 * invites it.
 *
 * Throws on anything the RFC calls a failure: a reserved opcode, RSV bits set
 * with no extension negotiated, a control frame that is fragmented or longer
 * than 125 bytes (5.5), a length not written in its shortest form (5.2), a
 * 64-bit length with the top bit set (5.2), and a payload over max_payload.
 *
 * **from is required rather than inferred.** A server MUST reject an unmasked
 * frame from a client and a client MUST reject a masked one from a server, so a
 * decoder that simply unmasked whatever it was given would silently accept both
 * and remove the only check 5.1 asks for.
 *
 * max_payload bounds a single frame, not a message; the layer that reassembles
 * fragments has to bound the total itself, and a cap here would not see it.
 *
 * **The default is an estimate and not a measurement, which is the useful thing
 * to say about it.**  Its job is to be *finite* rather than to be right: the
 * length is a number the peer chose, so it is checked before anything is
 * reserved and before the bytes are waited for, and without that a peer claiming
 * 2^63 bytes makes a reader buffer until it dies.  1 MiB is sized for the first
 * expected consumer, a CDP accessibility tree for a real page.  When that is
 * actually running, measure one and put the measurement here with its
 * provenance -- which is a different kind of number from this one.
 */
std::size_t decode(std::string_view in, frame& into, sender from,
                   std::size_t max_payload = 1u << 20);

/**
 * The payload of a close frame: a 2-byte code then an optional UTF-8 reason.
 *
 * **reason must already be valid UTF-8, and this throws if it is not.**  The
 * failure happens either way and the only question is where: send invalid bytes
 * and the peer is *required* to fail the connection, so what you learn is a
 * close code from a remote machine during shutdown, about a mistake made locally
 * and synchronously.  Stripping to valid would be worse still -- a caller that
 * asked to send a reason would get a frame that silently says nothing.
 *
 * Validated **before** truncating, not after: truncating first would reject
 * input that was fine and got cut, and blame the caller for our cut.
 *
 * Then truncated to fit the 125 bytes a control frame may carry (5.5), on a
 * character boundary rather than mid-sequence, because the result still has to
 * be valid UTF-8 for the peer to accept it.
 */
std::string close_payload(close_code code, std::string_view reason = "");

/**
 * Read a close frame's payload back.
 *
 * An empty payload is legal and means "no status was given", which RFC 6455
 * 7.1.5 says to treat as 1005 -- a code that may never be sent on the wire, so
 * it is returned here and refused by close_payload().
 */
close_code close_reason(std::string_view payload, std::string& reason_out);

}
}
}

#endif // JLIB_NET_WEBSOCKET_HH
