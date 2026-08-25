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

#ifndef JLIB_UTIL_MIMETYPE_HH
#define JLIB_UTIL_MIMETYPE_HH

#include <exception>
#include <string>

namespace jlib {
    namespace util {
    

        /**
         * Class MimeType allows you to determine the MIME type of a chunk of data or file
         *
         */
        class MimeType {
        public:
            
            class exception : public std::exception {
            public:
                exception(const std::string& p_msg = "") {
                    m_msg = "jlib::util::MimeType exception: "+p_msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            /**
             * The media type of the file at path.
             *
             * Asks file(1) with --mime-type, which prints the answer rather
             * than a sentence about it, so nothing has to be parsed out.
             * "application/octet-stream" when file cannot say -- an
             * unidentifiable attachment should still be sendable.
             *
             * The path is passed as an argument and never through a shell.
             * It used to be interpolated into "file "+path and handed to
             * /bin/sh, so a file named "x; rm -rf ~" ran as a command.
             */
            static std::string get_type_from_file(const std::string& path);

            /** The media type of a blob, via a temporary file. */
            static std::string get_type_from_data(const std::string& data);
        };
        
    }
}
#endif //JLIB_UTIL_MIMETYPE_HH
