#include <iostream>

#include <jlib/net/net.hh>

int main(int argc, char** argv) {
    std::string addr = "joe_yandle@division-by-zero.com",
        rare;
    if( (rare=jlib::net::extract_address(addr)) == addr )
        return 0;
    else {
        std::cerr << "addr: "<<addr<<std::endl;
        std::cerr << "extracted: "<<rare<<std::endl;
        return 1;

    }
}
