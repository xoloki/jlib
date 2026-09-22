/**
 * A response head, parsed the way the *client* parses one.
 *
 * The mirror of head_request, and not redundant with it: RFC 9112 6.3 gives a
 * response with neither Content-Length nor Transfer-Encoding a body that runs
 * until the connection closes, where the same request has no body at all. The
 * two functions therefore make different decisions from the same fields, and
 * the framing rules are where a parser hides its worst bugs.
 *
 * `head_request` -- the flag, not the file -- is the other asymmetry: a
 * response to HEAD carries the framing fields of a body it will never send,
 * so the same bytes must frame differently depending on what was asked. The
 * selector byte drives it, so both readings of every corpus unit get tried.
 */
#include <jlib/util/http.hh>

#include "check.hh"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    if(size < 1 || size > 16384) return 0;

    const bool to_head = (data[0] & 1) != 0;

    const std::string head(reinterpret_cast<const char*>(data + 1), size - 1);

    try {
        const jlib::util::http::Response r =
            jlib::util::http::parse_head(head, to_head);

        (void) r.version();
        (void) r.reason();

        // A status line that parsed names a status in range. Anything outside
        // it would be a number the rest of the client switches on.
        JLIB_FUZZ_CHECK(r.status() >= 100 && r.status() <= 599, head);

        // The smuggling refusal, in the direction a client reads. A response
        // carrying both framings is the half of the attack that poisons a
        // cache rather than the half that bypasses a front end.
        const bool has_length = r.fields().count("Content-Length") > 0;
        const bool has_coding = r.fields().count("Transfer-Encoding") > 0;

        JLIB_FUZZ_CHECK(!(has_length && has_coding), head);

        if(r.body_framing() == jlib::util::http::framing::length) {
            JLIB_FUZZ_CHECK(has_length, head);
        }
    }
    catch(const jlib::util::http::error&) {
        // A refusal is an answer. Anything else escaping is not.
    }

    return 0;
}
