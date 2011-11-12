/* -*- mode:C++ c-basic-offset:4 -*-
 * xmlpp - an xml file parser and validator
 * Copyright (C) 2000 Michael Fink
 * Copyright (C) 2000 Joe Yandle
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

// prevent multiple includes
#ifndef JLIB_UTIL_XML_PARSER_HH
#define JLIB_UTIL_XML_PARSER_HH

// needed includes
#include <jlib/util/xml.hh>
#include <jlib/util/xmltokenizer.hh>

// namespace declaration
namespace jlib {
    namespace util {
        namespace xml {
            
            
            //!  parser implementation	class
            class parser
            {
            public:
                //! ctor
                parser( std::istream &inputstream );
                
                //! parses the node as the document root
                void parse_document( document &doc );
                
                //! parses a node, without processing instructions
                void parse_node( node& n );
                
            protected:
                //! parses xml header, such as processing instructions, doctype and so on
                void parse_header( document &doc );
                
                //! parses a xml tag attribute list
                void parse_attributes( node::attributes &attr );
                
                /**
                 * get rid of enclosing quotes, and translate 
                 * &amp; etc
                 */
                static std::string clean(std::string s);

            protected:
                std::istream &instream;
                stream_iterator tokenizer;
            };
            
            // namespace end
        }
    }
}

#endif //JLIB_UTIL_XML_PARSER_HH
