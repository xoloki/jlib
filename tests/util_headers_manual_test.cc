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
