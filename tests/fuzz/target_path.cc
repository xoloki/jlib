/**
 * A request target, turned into a path or refused.
 *
 * Where `%2F`, `%5C` and control characters are refused, where the encoded
 * slash in a *query* was wrongly refused for a week (#320), and where the
 * first fuzzing round found a NUL truncation (#262). Every one of those is a
 * decision about a string, reachable in nanoseconds and previously reached
 * through a socket.
 *
 * **The invariant**: it returns true with a path, or false with a reason, and
 * never both and never neither. A path it accepts must not contain a NUL or a
 * newline -- the first because a C API downstream would see a shorter string
 * than this one checked, the second because a path reaches the access log.
 */
#include <jlib/net/http_server.hh>

#include "check.hh"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    if(size > 8192) return 0;

    const std::string target(reinterpret_cast<const char*>(data), size);

    std::string path;
    std::string why;

    const bool took = jlib::net::http::server::path_of(target, path, why);

    if(took) {
        // Accepted. What it handed back has to be usable by everything
        // downstream, and "usable" is not the same as "parsed".
        JLIB_FUZZ_CHECK(path.find('\0') == std::string::npos, target);
        JLIB_FUZZ_CHECK(path.find('\n') == std::string::npos, target);
        JLIB_FUZZ_CHECK(path.find('\r') == std::string::npos, target);

        // A path is what gets joined to a root. One that does not start at
        // the root is a containment question answered by string arithmetic
        // somewhere else, and this is where it would begin.
        JLIB_FUZZ_CHECK(path.empty() || path[0] == '/', target);
    }
    else {
        // Refused. The reason is what an operator reads and what the client
        // is told, so it must exist.
        JLIB_FUZZ_CHECK(!why.empty(), target);
    }

    return 0;
}
