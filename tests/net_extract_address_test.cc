#include <iostream>

#include <jlib/net/net.hh>

int main(int argc, char** argv) {
    std::string addr = "joe_yandle@division-by-zero.com",
        raw = "Joe Yandle <"+addr+">", rare;
    if( (rare=jlib::net::extract_address(raw)) == addr )
        return 0;
    else {
        std::cerr << "addr: "<<addr<<std::endl;
        std::cerr << "extracted: "<<rare<<std::endl;
        return 1;

    }
}
