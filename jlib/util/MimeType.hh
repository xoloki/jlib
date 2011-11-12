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
                exception(std::string p_msg = "") {
                    m_msg = "jlib::util::MimeType exception: "+p_msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            /**
             * Get the Mime-Type from the given filename
             *
             * @param path path to file
             *
             * @return std::string description of data's MIME type
             */
            static std::string get_type_from_file(std::string path);

            /**
             * Get the Mime-Type from the given filename
             *
             * @param path path to file
             *
             * @return std::string description of data's MIME type
             */
            static std::string get_type_from_data(std::string data);
          
            /**
             * parse the text output of the UNIX file command into a mime-type
             */
            static std::string parse_file_output(std::string data);
        };
        
    }
}
#endif //JLIB_UTIL_MIMETYPE_HH
