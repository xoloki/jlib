#include <iostream>

#include <jlib/util/util.hh>

#include <cstdlib>

int main(int argc, char** argv) {
    using namespace jlib::util;

    std::string foo("\x00\x01\x02\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",16);
    int intarr[4];
    for(int i=0;i<4;i++) {
        intarr[i] = i;
    }

    if(get<char>(foo) != 0x00) {
        std::cerr << "error: in jlib::util::get<char>(std::string)"<< std::endl;
        exit(1);
    }
    if(get<u_short>(foo,2) != 0x0302) {
        std::cerr << "error: in jlib::util::get<u_short>(std::string,u_int): "
                  << (int)get<u_short>(foo,2) <<std::endl;
        exit(1);
    }
    if(get<u_long>(foo,0) != 0x03020100) {
        std::cerr << "error: in jlib::util::get<u_long>(std::string,u_int): "
                  << (int)get<u_long>(foo,0) <<std::endl;
        exit(1);
    }

    set<char>(foo,0x01);
    if(get<char>(foo) != 0x01) {
        std::cerr << "error: in jlib::util::set<char>(std::string,char): "
                  << (int)get<char>(foo) <<std::endl;
        exit(1);
    }

    set<u_short>(foo, 0xffff, 1);
    if(get<u_short>(foo,1) != 0xffff) {
        std::cerr << "error: in jlib::util::set<u_short>(std::string,char,u_int): "
                  << (int)get<u_short>(foo,1) <<std::endl;
        exit(1);
    }
    
    set<u_long>(foo, 0x06060606, 0);
    if(get<u_long>(foo,0) != 0x06060606) {
        std::cerr << "error: in jlib::util::set<u_long>(std::string,char,u_int): "
                  << (int)get<u_long>(foo,0) <<std::endl;
        exit(1);
    }
    
    copy<int>(foo,intarr,2,4);
    if(get<int>(foo, 8) != 1) {
        std::cerr << "error: in jlib::util::copy<int>(std::string,int*,u_int,u_int): "
                  << hex_value(foo) <<std::endl;
        exit(1);
    }
    
    byte_copy(foo, intarr+3, 1);
    if(get<char>(foo) != 3) {
        std::cerr << "error: in jlib::util::byte_copy<int>(std::string,int*,u_int): "
                  << hex_value(foo) <<std::endl;
        exit(1);
    }

    exit(0);
}
