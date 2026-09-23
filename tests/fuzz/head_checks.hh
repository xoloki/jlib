/**
 * What a parsed request head has to be true of, shared by both head targets.
 *
 * Two targets ask the same questions of parse_request_head and differ only in
 * how their inputs are made: `head_request` mutates bytes, `grammar_head`
 * draws well-formed messages from the ABNF and mutates those (#354). Keeping
 * the invariants in one place is what makes the pair a comparison -- two
 * copies that drifted would be measuring two different things and reporting
 * it as a difference between mutators.
 */
#ifndef JLIB_TESTS_FUZZ_HEAD_CHECKS_HH
#define JLIB_TESTS_FUZZ_HEAD_CHECKS_HH

#include <jlib/util/http.hh>

#include "check.hh"

#include <string>

namespace jlib::fuzz {

inline void check_request_head(const std::string& head)
{

    // A cap, so a mutant that is mostly length does not teach the corpus that
    // longer is better. max_head is 8192 and the parser is not asked to cope
    // with more than a reader would hand it.
    if(head.size() > 16384) return;


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
}

} // namespace jlib::fuzz

#endif
