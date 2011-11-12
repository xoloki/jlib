#include <iostream>

#include <jlib/util/Headers.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "Received: from localhost (localhost [127.0.0.1]) by devotchka.germtop.com (8.11.4/8.11.4) with SMTP id f9T8xj318352 for jwy@localhost; Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Date: Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Message-Id: <200110290859.f9T8xj318352@devotchka.germtop.com>\n"
      "Received: 10.0.0.1\n"
      "From: foo@bar.com\n"
      "Foo: bar\n"
      " baz\n"
      " bat\n"
      "Subject: i hate you, so, very much\n"
      "Content-Type: multipart/alternative;\n"
      "boundary=\"----=_NextPart_000_005E_01C17C0B.91F7B8A0\"\n"
      "\n"
      "\n";

    jlib::util::Headers headers(raw);
    if(headers["FOO"] == "bar baz bat")
        return 0;
    else {
        std::cerr << "headers[\"FOO\"] = "<<headers["FOO"]<<std::endl;
        return 1;

    }
}
