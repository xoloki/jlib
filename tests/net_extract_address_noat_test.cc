// An address with no '@' has no address in it, so extract_address should
// return the empty string.  It used to store find()'s result in a u_int,
// which truncates npos to 0xFFFFFFFF; the "not found" guard therefore never
// fired and the function returned a chunk of its input instead.
#include <iostream>

#include <jlib/net/net.hh>

int main(int argc, char** argv) {
    const char* no_address[] = {
        "no-at-sign-here",
        "",
        "Joe Yandle",
        "   ",
    };

    int failures = 0;
    for(const char* s : no_address) {
        const std::string got = jlib::net::extract_address(s);
        if(!got.empty()) {
            std::cerr << "extract_address(\"" << s << "\") returned \""
                      << got << "\", expected empty" << std::endl;
            ++failures;
        }
    }

    // and the ordinary case still works
    const std::string addr = "joe_yandle@division-by-zero.com";
    const std::string got = jlib::net::extract_address(addr);
    if(got != addr) {
        std::cerr << "extract_address(\"" << addr << "\") returned \""
                  << got << "\"" << std::endl;
        ++failures;
    }

    return failures ? 1 : 0;
}
