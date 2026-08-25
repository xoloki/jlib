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

//#include <gnome-1.0/gnome.h>

namespace jlib {
    namespace util {
        
        
        std::string MimeType::get_type_from_file(const std::string& path) {
            //return gnome_mime_type_of_file(path.c_str());
            std::string out, err;
            sys::shell("file "+path, out, err);

            return parse_file_output(out);
        }

        std::string MimeType::get_type_from_data(const std::string& data) {
            std::string ret;
            std::string out, err;
            jlib::sys::tfstream in;
            in << data;
            in.close();

            sys::shell("file "+in.get_path(), out, err);
            return parse_file_output(out);
        }

        std::string MimeType::parse_file_output(const std::string& data) {
            // On a local: this overwrote its own parameter.
            const std::string body = (data.find(":") != std::string::npos)
                ? data.substr(data.find(":"))
                : data;
            std::string ret;

            if(icontains(data, "image")) {
                ret = "image/";
                if(icontains(data, "JPEG")) {
                    ret += "jpeg";
                }
                else if(icontains(data, "GIF")) {
                    ret += "gif";
                }
                else if(icontains(data, "PNG")) {
                    ret += "png";
                }
                else 
                    ret = "application/octet-stream";
            }
            else if(icontains(data, "audio")) {
                ret = "audio/";
                if(icontains(data, "wav")) {
                    ret += "x-wav";
                }
                else
                    ret = "application/octet-stream";
            }
            

            else if(icontains(data, "document")) {
                if(icontains(data, "postscript"))
                    ret = "application/postscript";
                else if(icontains(data, "pdf"))
                    ret = "application/x-pdf";
                else
                    ret = "application/octet-stream";
            }

            else if(icontains(data, "text")) {
                ret += "text/";
                if(icontains(data, "HTML")) {
                    ret += "html";
                }
                else {
                    ret += "plain";
                }
            }
            else {
                ret = "application/octet-stream";
            }
            
            return ret;
        }
        
    }
}
