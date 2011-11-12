#include <iostream>

#include <jlib/net/net.hh>

int main(int argc, char** argv) {
    std::string addr = "joe_yandle@division-by-zero.com",
        rare;
    if( (rare=jlib::net::extract_address("<"+addr)) != addr ||
        (rare=jlib::net::extract_address(addr+">")) != addr ) {

        std::cerr << "addr: "<<addr<<std::endl;
        std::cerr << "extracted: "<<rare<<std::endl;
        return 1;
    }

    std::string s = ",  joey@dbzero.com  , \"Yandle, Joseph\" <joey@dbzero.com>, joey@dbzero.com (Yandle, Joseph)";
    std::list<std::string> split = jlib::net::split_addresses(s);
    std::list<std::string>::iterator i = split.begin();
    if(split.size() != 3) {
        std::cerr << "split.size() == " << split.size() << std::endl;
        for(i = split.begin(); i != split.end(); i++) {
            std::cerr << "    " << *i << std::endl;
        }
        return 1;
    }
    if(*i != "joey@dbzero.com") {
        std::cerr << "joey@dbzero.com != " << *i << std::endl;
        for(i = split.begin(); i != split.end(); i++) {
            std::cerr << "    " << *i << std::endl;
        }
        return 1;
    }
    i++;
    if(*i != "\"Yandle, Joseph\" <joey@dbzero.com>") {
        std::cerr << "\"Yandle, Joseph\" <joey@dbzero.com> != " << *i << std::endl;
        for(i = split.begin(); i != split.end(); i++) {
            std::cerr << "    " << *i << std::endl;
        }
        return 1;
    }
    i++;
    if(*i != "joey@dbzero.com (Yandle, Joseph)") {
        std::cerr << "joey@dbzero.com (Yandle, Joseph) != " << *i << std::endl;
        for(i = split.begin(); i != split.end(); i++) {
            std::cerr << "    " << *i << std::endl;
        }
        return 1;
    }

    return 0;
}
