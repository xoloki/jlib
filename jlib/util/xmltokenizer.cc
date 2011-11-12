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

// needed includes
#include <bits/char_traits.h>

#include <jlib/util/xml.hh>
#include <jlib/util/xmltokenizer.hh>


//! token value for a generic string
const int token_generic_string = 256;


// namespace declaration
namespace jlib {
    namespace util {
        namespace xml {
            
            
            // stream_iterator methods
            
            stream_iterator::stream_iterator(std::istream &is)
                :instr(is)
            {
                putback_char = -1;
                cdata_mode = false;
                last_delimiter = -1;
            }
            
            void stream_iterator::get_next() {
                // first use the token stack if filled
                if (tokenstack.size() != 0) {
                    // get the token from the stack and return it
                    token tok;
                    curtoken = tokenstack.top();
                    tokenstack.pop();
                    
                    //      cdata_mode = false;
                    
                    return;
                }
                
                bool finished = false;
                bool stringmode = false;
                
                std::string generic;
                // loop
                do {
                    // get next char
                    int c;
                    
                    if (putback_char == -1 )
                        c = instr.get();
                    else {
                        c = putback_char;
                        putback_char = -1;
                    }
                    
                    // do we have an eof?
                    //! /todo: check for instr.eof()
                    if (c == std::char_traits<char>::eof()) {
                        if (generic.length()==0) {
                            curtoken = c;
                            return;
                        }
                        else
                            break;
                    }
                    
                    // a literal and not in std::string mode?
                    if (is_literal(c) && !stringmode ) {
                        cdata_mode = false;
                        if (generic.length()==0) {
                            curtoken = c;
                            
                                // quick fix for removing set_cdata_mode() functionality
                            if (c=='>')
                                cdata_mode = true;
                            
                            return;
                        }
                        
                        putback_char = c;
                        break;
                    }
                    
                    // a std::string delimiter and not in cdata mode?
                    if (is_stringdelimiter(c) && !cdata_mode) {
                        stringmode = !stringmode;
                        if (!stringmode)
                            finished = true;
                    }
                    
                    // a whitespace?
                    if (is_whitespace(c) && !stringmode) {
                        if (generic.length()==0)
                            continue;
                        else
                            if (!cdata_mode)
                                break;
                    }
                    
                    // a newline char?
                    if (is_newline(c) ) {
                        if (cdata_mode && generic.length()!=0)
                            c = ' ';
                        else
                            continue;
                    }
                    
                    // add to generic string
                    generic += c;
                }
                while (!finished);
                
                // set the generic string
                curtoken = generic;
            }
            
            // returns if we have a literal char
            bool stream_iterator::is_literal( char c ) {
                switch(c)
                    {
                    case '?':
                    case '=':
                    case '!':
                    case '/':
                        if (cdata_mode)
                            return false;
                    case '<':
                    case '>':
                        return true;
                    }
                return false;
            }
            
            // returns if we have a white space char
            bool stream_iterator::is_whitespace( char c ) {
                switch(c) {
                case ' ':
                case '\t':
                    return true;
                }
                return false;
            }
            
            // returns if we have a newline
            bool stream_iterator::is_newline( char c ) {
                switch(c) {
                case '\n':
                case '\r':
                    return true;
                }
                return false;
            }
            
            // returns if we have a std::string delimiter (separating " and ')
            bool stream_iterator::is_stringdelimiter( char c ) {
                // do we have the same char that started the string
                if (last_delimiter == c) {
                    last_delimiter = -1;
                    return true;
                }
                else // do we have a delimiter char?
                    if (last_delimiter == -1 && (c=='\"' || c=='\'')) {
                        last_delimiter = c;
                        return true;
                    }
                
                return false;
            }
            
        }
    }
}
