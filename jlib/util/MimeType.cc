/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 * 
 */

#include <jlib/sys/sys.hh>
#include <jlib/sys/tfstream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/MimeType.hh>

//#include <gnome-1.0/gnome.h>

namespace jlib {
    namespace util {
        
        
        std::string MimeType::get_type_from_file(std::string path) {
            //return gnome_mime_type_of_file(path.c_str());
            std::string out, err;
            sys::shell("file "+path, out, err);

            return parse_file_output(out);
        }

        std::string MimeType::get_type_from_data(std::string data) {
            std::string ret;
            std::string out, err;
            jlib::sys::tfstream in;
            in << data;
            in.close();

            sys::shell("file "+in.get_path(), out, err);
            return parse_file_output(out);
        }

        std::string MimeType::parse_file_output(std::string data) {
            if(data.find(":") != std::string::npos) {
                data = data.substr(data.find(":"));
            }
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
