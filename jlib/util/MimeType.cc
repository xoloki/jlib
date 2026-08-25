/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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
 *
 */

#include <jlib/sys/sys.hh>
#include <jlib/sys/tfstream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/MimeType.hh>
#include <jlib/util/content_type.hh>

//#include <gnome-1.0/gnome.h>

namespace jlib {
    namespace util {
        
        
        namespace {

            /**
             * file(1), asked for the answer rather than for a sentence.
             *
             * --mime-type -b prints "image/jpeg" and nothing else, so there is
             * no output to parse.  What was here instead ran icontains() over
             * file's English prose looking for the words "image" and "JPEG" --
             * and over the *whole* of its output, which begins with the
             * filename, so a file called JPEG-notes.txt was an image/jpeg.
             * The variable that was supposed to cut the name off was computed
             * and never read.
             */
            std::string sniff(const std::string& path) {
                std::string out, err;

                // sys::run, not sys::shell: the path is a filename, and
                // "file "+path handed it to /bin/sh.  A file named
                // "x; rm -rf ~" ran as a command.
                if(sys::run({ "file", "--mime-type", "-b", path }, out, err) != 0) {
                    return "application/octet-stream";
                }

                const std::string type = util::trim(out);

                // A type is "type/subtype" and nothing else.  file prints
                // "cannot open ..." on stdout for a file it cannot read, and
                // that is not a media type.
                if(!content_type::valid(type)) {
                    return "application/octet-stream";
                }

                return type;
            }

        }

        std::string MimeType::get_type_from_file(const std::string& path) {
            return sniff(path);
        }

        std::string MimeType::get_type_from_data(const std::string& data) {
            jlib::sys::tfstream in;

            in << data;
            in.close();

            return sniff(in.get_path());
        }

    }
}
