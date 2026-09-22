/**
 * What a fuzz target does when an invariant does not hold.
 *
 * **`abort()`, never `__builtin_trap()`**, and the difference is the whole
 * value of a finding. libFuzzer installs handlers for SIGSEGV, SIGBUS,
 * SIGILL, SIGFPE and SIGABRT; SIGTRAP is not among them. A target that traps
 * therefore dies without libFuzzer ever running its crash path -- no stack
 * trace, no `SUMMARY`, and above all **no `crash-<sha1>` file**, because
 * writing the offending input is something the handler does.
 *
 * That is not a theoretical difference. The first run of `target_path` found
 * a violating mutant in under a minute, killed the process with SIGTRAP, and
 * left nothing behind: the corpus held no trace of it, every seed and every
 * saved unit passed when re-run, and the input that actually broke the
 * invariant was gone. A fuzzer that cannot hand back the input has not found
 * a bug, it has only ruined a run.
 *
 * The message goes to stderr before aborting, so the log says which invariant
 * went rather than only that something did.
 */
#ifndef JLIB_TESTS_FUZZ_CHECK_HH
#define JLIB_TESTS_FUZZ_CHECK_HH

#include <cstdio>
#include <cstdlib>
#include <string>

namespace jlib::fuzz {

/** Print the input as hex, so a crash log alone is enough to reproduce. */
inline void dump(const std::string& in)
{
    std::fprintf(stderr, "input (%zu bytes): ", in.size());
    for(const unsigned char c : in) std::fprintf(stderr, "%02x", c);
    std::fprintf(stderr, "\n  as text: ");
    for(const unsigned char c : in)
        std::fputc(c >= 0x20 && c < 0x7f ? c : '.', stderr);
    std::fputc('\n', stderr);
}

[[noreturn]] inline void violated(const char* what, const std::string& in)
{
    std::fprintf(stderr, "\nfuzz invariant violated: %s\n", what);
    dump(in);
    std::fflush(stderr);
    std::abort();   // SIGABRT: libFuzzer catches this and saves the unit
}

} // namespace jlib::fuzz

/** Assert an invariant, naming it, and hand libFuzzer a reproducible unit. */
#define JLIB_FUZZ_CHECK(cond, in) \
    do { if(!(cond)) ::jlib::fuzz::violated(#cond, (in)); } while(0)

#endif
