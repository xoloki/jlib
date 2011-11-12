#include <iostream>

#include <jlib/util/Headers.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "Received: from localhost (localhost [127.0.0.1]) by devotchka.germtop.com (8.11.4/8.11.4) with SMTP id f9T8xj318352 for jwy@localhost; Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Date: Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Message-Id: <200110290859.f9T8xj318352@devotchka.germtop.com>\n"
      "X-Authentication-Warning: devotchka.germtop.com: localhost [127.0.0.1] didn't use HELO protocol\n"
      "Received: 64.81.68.235\n"
      "Received: 192.168.0.1\n"
      "Received: 172.17.0.1\n"
      "Received: 10.0.0.1\n"
      "From: foo@bar.com\n"
      "Subject: i hate you, so, very much\n";

    jlib::util::Headers headers(raw);
    if(raw == std::string(headers))
        return 0;
    else {
        std::cerr << "raw:\n"<<raw<<std::endl;
        std::cerr << "headers:\n"<<std::string(headers)<<std::endl;
        return 1;

    }
}
