/**
 * A request head, parsed -- with inputs drawn from the grammar (#354).
 *
 * The same invariants as `head_request`, from the same header. The only
 * difference is where an input comes from, and that difference is the point.
 *
 * **Byte mutation almost never produces a well-formed message.** libFuzzer
 * builds an input by editing bytes of one already in the corpus, and the
 * probability that random edits assemble a valid request line, a field
 * section and the blank line that ends it is effectively zero. It discovers
 * some of the shape from the dictionary entries it harvests out of string
 * comparisons -- the `DE:` annotations in its output -- but past the first
 * few fields the search is spending its budget on heads the parser rejects in
 * the first few bytes. Measured on the four #268 targets: coverage went
 * static and stayed there, `target_path` adding nothing across 4.2M inputs.
 *
 * `LLVMFuzzerCustomMutator` replaces that. Half the time it draws a whole
 * message from the RFC 9112 grammar -- valid by construction, because
 * `rule::generate` walks the same tree the parser does -- and half the time
 * it falls back to `LLVMFuzzerMutate`, which is what still finds the
 * malformed input a strict parser has to refuse.
 *
 * **Both halves are needed.** A grammar-only mutator tests the accepting path
 * and never the refusing one, and every bug #268 found was on the refusing
 * path. A byte-only mutator is what this file exists to improve on.
 */
#include "head_checks.hh"

#include <jlib/util/abnf.hh>
#include <jlib/util/http.hh>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

/**
 * start-line CRLF *( field-line CRLF ) CRLF
 *
 * Composed here rather than taken from the grammar, because
 * `HTTP-message` ends with an optional `message-body` of `*OCTET` and what
 * `parse_request_head` is handed stops at the blank line. Built once: the
 * shortest-string fixed point behind generate() is solved per call, and
 * paying for it on every draw would show up at 10,000 draws a second.
 */
const jlib::util::abnf::rule& request_head()
{
    using namespace jlib::util::abnf;

    static const rule r = [] {
        const grammar& g = jlib::util::http::grammar();

        const rule crlf = lit("\r\n");

        // At most six fields. An unbounded repetition here draws a head whose
        // size is dominated by how many times the count came up, and a corpus
        // of enormous valid heads crowds out everything else.
        return g.at("request-line") >> crlf >>
               rep(g.at("field-line") >> crlf, 0, 6) >> crlf;
    }();

    return r;
}

} // namespace

/** libFuzzer's own byte mutator, to fall back to. */
extern "C" std::size_t LLVMFuzzerMutate(std::uint8_t* data, std::size_t size,
                                        std::size_t max);

extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t* data,
                                               std::size_t size,
                                               std::size_t max,
                                               unsigned int seed)
{
    // Odd seeds keep the byte mutator, so the corpus still contains the
    // malformed inputs that are most of what a server actually meets.
    if((seed & 1) != 0) return LLVMFuzzerMutate(data, size, max);

    try {
        jlib::util::abnf::generate_options o;

        o.seed = seed;
        o.max_size = max < 4096 ? max : 4096;
        o.max_repeat = 3;

        const std::string s = request_head().generate(o);

        // A draw longer than libFuzzer's ceiling is dropped rather than
        // truncated: a truncated message is a *malformed* one, which the
        // other half of this mutator already produces in quantity, and
        // quietly turning valid draws into invalid ones would hide whether
        // the grammar half is doing anything.
        if(s.size() > max) return LLVMFuzzerMutate(data, size, max);

        std::memcpy(data, s.data(), s.size());

        return s.size();
    }
    catch(const std::exception&) {
        // A grammar that cannot be drawn from is not a reason to stop
        // fuzzing.
        return LLVMFuzzerMutate(data, size, max);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    jlib::fuzz::check_request_head(
        std::string(reinterpret_cast<const char*>(data), size));

    return 0;
}
