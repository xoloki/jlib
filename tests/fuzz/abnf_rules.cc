/**
 * Every rule in the HTTP grammar, against arbitrary bytes.
 *
 * `try_parse` is the non-throwing half of the grammar and the one the server
 * uses on anything a peer sent -- a request line, a field line, a chunk size,
 * an Authorization header. Its contract is that it *answers* rather than
 * throws, for any input at all, which is the kind of claim a fuzzer can
 * settle and a test suite can only sample.
 *
 * **The rule list is read from the grammar, not written here** (#268). The
 * first version of this file hardcoded ten names taken from `at(...)` call
 * sites across the library -- which turned out to span three different
 * grammars. `config` belongs to util::conf and `template` to util::jinja, so
 * asking the HTTP grammar for them threw, and the target reported a library
 * bug that was entirely mine. rules() cannot drift in that way, and it covers
 * every production rather than the handful somebody remembered.
 *
 * Prose rules are excluded: an RFC that describes a production in words
 * rather than ABNF leaves compile() a rule it cannot run, and parsing one
 * throws by design. That is a documented answer, not a defect.
 *
 * The first byte selects the rule and the rest is the input, so libFuzzer
 * mutates the selector too: a unit that found something in `field-line` gets
 * tried against `status-line` for free.
 */
#include <jlib/util/http.hh>
#include <jlib/util/abnf.hh>

#include "check.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

/**
 * Every rule that can actually be parsed, resolved once.
 *
 * **prose_rules() is not sufficient on its own**, and finding that out cost
 * this target its second false finding. It names the three productions the
 * RFCs write in words -- `language-range`, `Language-Tag`, `mailbox` -- but
 * not the rules that *reference* them. `Accept-Language`, `Content-Language`
 * and `From` are ordinary ABNF whose parse reaches an unimplemented prose-val
 * and throws grammar_error, exactly as abnf.hh says it will. Filtering the
 * prose productions alone left all three in the list, and the fuzzer duly
 * "found" one of them within 72 units.
 *
 * So the list is built by asking rather than by reasoning: a rule that throws
 * grammar_error on trivial input depends on prose and is dropped, with a
 * count reported so the exclusion is visible rather than silent.
 */
const std::vector<std::string>& rules()
{
    static const std::vector<std::string> v = [] {
        const jlib::util::abnf::grammar& g = jlib::util::http::grammar();

        const std::vector<std::string> prose = g.prose_rules();

        std::vector<std::string> out;

        std::size_t dropped = 0;

        for(const std::string& name : g.rules()) {
            if(std::find(prose.begin(), prose.end(), name) != prose.end()) {
                dropped++;

                continue;
            }

            try {
                (void) g.at(name).try_parse("");
            }
            catch(const jlib::util::abnf::grammar_error&) {
                dropped++;

                continue;
            }
            catch(const std::exception&) {
                // Something other than missing prose. Keep it -- that is
                // precisely what this target exists to find.
            }

            out.push_back(name);
        }

        std::fprintf(stderr,
                     "fuzz harness: %zu parseable rules, %zu dropped "
                     "(prose-dependent)\n",
                     out.size(), dropped);

        return out;
    }();

    return v;
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    // **A harness mistake must not look like a finding.** Resolving every
    // name once, here, means a bad list fails at startup with its own message
    // instead of surfacing as a crash report against the library -- which is
    // exactly what the first version of this file did.
    for(const std::string& name : rules()) {
        if(!jlib::util::http::grammar().has(name)) {
            std::fprintf(stderr, "fuzz harness: no such rule: %s\n",
                         name.c_str());
            std::abort();
        }
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    if(size < 1 || size > 8192) return 0;

    const std::string& rule = rules()[data[0] % rules().size()];

    const std::string in(reinterpret_cast<const char*>(data + 1), size - 1);

    bool answered = false;

    try {
        const jlib::util::abnf::parse_result r =
            jlib::util::http::grammar().at(rule).try_parse(in);

        answered = true;

        // Matched or not, the result has to be self-consistent: root() throws
        // unless it matched, why() is valid only when it did not, and neither
        // may claim to have consumed more than it was given.
        if(r) {
            JLIB_FUZZ_CHECK(r.consumed() <= in.size(), in);

            (void) r.root();
        }
        else {
            (void) r.why();
        }
    }
    catch(const jlib::util::abnf::grammar_error&) {
        // The grammar could not run the rule, rather than the rule rejecting
        // the input. A prose-val reached only down some alternatives survives
        // the startup screen above, so this has to be tolerated here too --
        // it is a statement about the RFC, not about the parser.
        return 0;
    }
    catch(const std::exception&) {
        answered = false;
    }

    JLIB_FUZZ_CHECK(answered, in);

    return 0;
}
