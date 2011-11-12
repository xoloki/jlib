#include <iostream>

#include <jlib/util/Headers.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "From: foo@bar.com\n"
      "Subject: i hate you, so, very much\n";

    jlib::util::Headers headers;
    headers.set("Return-Path", "<jwy@divisionbyzero.com>");
    headers.add("From", "foo@bar.com");
    headers.add("Subject", "i hate you, ");
    headers.append("Subject", "so, very much");

    if(raw == std::string(headers))
        return 0;
    else {
        std::cerr << "raw:\n"<<raw<<std::endl;
        std::cerr << "headers:\n"<<std::string(headers)<<std::endl;
        return 1;

    }
}
