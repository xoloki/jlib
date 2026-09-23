/**
 * A request head, parsed.
 *
 * Byte mutation: libFuzzer builds every input by editing bytes of one already
 * in the corpus. That is what finds the malformed head a strict parser must
 * refuse, and it is how #268 found the rootless target and the out-of-range
 * status. `grammar_head` is the same questions with inputs drawn from the
 * grammar instead; the pair is a measurement of which reaches further.
 */
#include "head_checks.hh"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    jlib::fuzz::check_request_head(
        std::string(reinterpret_cast<const char*>(data), size));

    return 0;
}
