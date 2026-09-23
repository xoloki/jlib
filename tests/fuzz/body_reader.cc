/**
 * A message body, read off a stream.
 *
 * **The gap #268 could not reach.** Chunked decoding is where a framing bug
 * does the most damage -- a length and a payload that disagree is how a
 * request gets smuggled past a front end -- and it is the one part of the
 * message path that takes a stream rather than a string. A target over
 * `std::istringstream` is all that was missing.
 *
 * The first byte chooses the framing and the cap, so one corpus covers all
 * four: a declared length, chunked, read-until-close, and none. The next two
 * give the declared length, which for `framing::length` is the number the
 * reader trusts and the fuzzer's chance to make it disagree with what
 * follows.
 */
#include <jlib/util/http.hh>

#include "check.hh"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    if(size < 3 || size > 16384) return 0;

    namespace http = jlib::util::http;

    const http::framing how = [&] {
        switch(data[0] & 3) {
        case 0:  return http::framing::length;
        case 1:  return http::framing::chunked;
        case 2:  return http::framing::until_close;
        default: return http::framing::none;
        }
    }();

    // Small on purpose. A declared length in the megabytes would spend the
    // run allocating rather than deciding, and every framing bug worth having
    // shows up in the first few hundred bytes.
    const std::size_t declared =
        (static_cast<std::size_t>(data[1]) << 8 | data[2]) & 0x3FF;

    // Half the draws run with a cap, because the cap is a promise the reader
    // makes and a promise is worth checking.
    const bool capped = (data[0] & 4) != 0;
    const std::size_t cap = capped ? 4096 : http::no_cap;

    std::istringstream is(
        std::string(reinterpret_cast<const char*>(data + 3), size - 3));

    const std::string input = is.str();

    try {
        const std::string body = http::read_body(is, how, declared, cap);

        // **A cap is a bound on what was read, not a hint.** Refusing a
        // gigabyte before reading it is the whole value of a declared length,
        // and a reader that returned more than it promised to would be the
        // one place that bound could fail silently.
        if(capped) JLIB_FUZZ_CHECK(body.size() <= cap, input);

        // A declared length that returned is a length that was met. Short is
        // an error, long is somebody else's body.
        if(how == http::framing::length)
            JLIB_FUZZ_CHECK(body.size() == declared, input);

        // Nothing framed is nothing read.
        if(how == http::framing::none) JLIB_FUZZ_CHECK(body.empty(), input);

        // Whatever came out came from what went in -- a decoder that returned
        // more octets than the stream held would be inventing them.
        JLIB_FUZZ_CHECK(body.size() <= input.size(), input);
    }
    catch(const http::error&) {
        // A body that ends early, a chunk without its CRLF, a chunk size that
        // is not one: refusals, and the point of the exercise.
    }

    return 0;
}
