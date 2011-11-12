#include <iostream>

#include <jlib/net/Email.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "Received: from localhost (localhost [127.0.0.1]) by devotchka.germtop.com (8.11.4/8.11.4) with SMTP id f9T8xj318352 for jwy@localhost; Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Date: Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Message-Id: <200110290859.f9T8xj318352@devotchka.germtop.com>\n"
      "Received: 64.81.68.235\n"
      "Received: 192.168.0.1\n"
      "Received: 172.17.0.1\n"
      "Received: 10.0.0.1\n"
      "From: foo@bar.com\n"
      "Subject: i hate you, so, very much\n"
      "Mime-Version: 1.0\n"
      "Content-Type: multipart/mixed; boundary=\"0123456gtkmail0123456\"\n"
      "\n"
      "This is a multi-part message in MIME format.\n"
      "\n"
      "--0123456gtkmail0123456\n"
      "Content-Type: text/plain\n"
      "\n"
      "i hate you guys\n"
      "\n"
      "--0123456gtkmail0123456\n"
      "Content-Type: text/plain\n"
      "Content-Transfer-Encoding: base64\n"
      "\n"
      "c28gdmVyeSBtdWNoCg==\n"
      "\n"
      "--0123456gtkmail0123456--\n"
      "\n";

    jlib::net::Email email(raw);
    email.build();
    if(raw == email.raw())
        return 0;
    else {
        std::cerr << "raw:\n"<<raw<<std::endl;
        std::cerr << "email:\n"<<email.raw()<<std::endl;
        return 1;

    }
}
