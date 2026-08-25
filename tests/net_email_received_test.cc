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
