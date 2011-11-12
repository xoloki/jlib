#include <iostream>

#include <jlib/net/Email.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "Received: 64.81.68.235\n"
      "Received: 192.168.0.1\n"
      "Received: 172.17.0.1\n"
      "Received: 10.0.0.1\n"
      "Date: Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Message-Id: <200110290859.f9T8xj318352@devotchka.germtop.com>\n"
      "From: foo@bar.com\n"
      "Received: 64.81.68.242\n"
      "Subject: i hate you, so, very much\n"
      "Mime-Version: 1.0\n"
      "Content-Type: text/plain\n"
      "\n"
      "i hate you guys\n"
      "\n"
      "\n";

    jlib::net::Email email(raw);
    std::string received = "64.81.68.235";

    if(email.get_received_ip() == received)
       return 0;
    else {
        std::cerr << "ip: ["<<email.get_received_ip()
                  << "] not: ["<<received<<"]"<<std::endl;
        return 1;

    }
}
