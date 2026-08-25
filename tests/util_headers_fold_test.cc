/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2011 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
