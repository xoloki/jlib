/**
 * A request head, parsed, with nothing between the bytes and the parser.
 *
 * #268: the socket fuzzer cannot tell a mutant that reached a new branch from
 * one rejected in the first ten octets, and the great majority are rejected
 * in the first ten octets -- a random edit to a valid request usually breaks
 * the request line. Its corpus never grows, so anything behind two correct
 * decisions is reached only by luck.
 *
 * Coverage feedback is what turns that from sampling into a search. No
 * socket, no accept, no close: hundreds of microseconds of transport per
 * input become none.
 *
 * **The invariant is the same one the socket fuzzer asserts**, reduced to
 * what a pure function can promise: it returns, or it throws util::http::error.
 * Anything else -- a crash, a hang, an exception of another type -- is the
 * bug. Sanitizers supply the rest.
 */
#include <jlib/util/http.hh>

#include "check.hh"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    // A cap, so a mutant that is mostly length does not teach the corpus that
    // longer is better. max_head is 8192 and the parser is not asked to cope
    // with more than a reader would hand it.
    if(size > 16384) return 0;

    const std::string head(reinterpret_cast<const char*>(data), size);

    try {
        const jlib::util::http::Request q =
            jlib::util::http::parse_request_head(head);

        // Touch what a caller would touch: an accessor that reads past the
        // end of something the parser mis-sized is a defect the parse alone
        // would not show.
        (void) q.method();
        (void) q.target();
        (void) q.version();
        (void) q.body_framing();
        (void) q.fields().get("Host");

        // **The smuggling refusal, as an invariant** rather than as a list of
        // heads somebody thought of. RFC 9112 6.1 makes a message carrying
        // both Content-Length and Transfer-Encoding one that must not be
        // forwarded or served: the two disagree about where the body ends,
        // and a proxy and an origin reading different answers out of the same
        // bytes is precisely request smuggling. parse_request_head refuses
        // it, so a head that survives parsing cannot have both.
        const bool has_length = q.fields().count("Content-Length") > 0;
        const bool has_coding = q.fields().count("Transfer-Encoding") > 0;

        JLIB_FUZZ_CHECK(!(has_length && has_coding), head);

        // A length framing means a length was read; anything else means the
        // field was not what decided it.
        if(q.body_framing() == jlib::util::http::framing::length) {
            JLIB_FUZZ_CHECK(has_length, head);
        }
    }
    catch(const jlib::util::http::error&) {
        // A refusal is a correct outcome and the common one.
    }

    return 0;
}
