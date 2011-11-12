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
#ifndef JLIB_UTIL_XML_TOKENIZER_HH
#define JLIB_UTIL_XML_TOKENIZER_HH

// needed includes
#include <jlib/util/xml.hh>

#include <stack>
#include <iostream>

// namespace declaration
namespace jlib {
    namespace util {
        namespace xml {
            
            
            //! token
            /*! an token is a representation for a literal character or a 
              generic std::string (not recognized as a literal) */
            class token {
                friend class stream_iterator;
                
            public:
                //! ctor, constructs empty token object
                token(){ generic=true; literal=0; };
                
                //! returns, if the token is a literal
                bool is_literal(){ return !bgen; };
                
                //! compare operator for literals
                bool operator ==(char ch){ return bgen?false:ch==literal; };
                //! compare operator for literals
                bool operator !=(char ch){ return bgen?true:ch!=literal; };
                //! compare operator for a generic string
                bool operator ==(std::string str){ return bgen?str==generic:false; };
                //! compare operator for a generic string
                bool operator !=(std::string str){ return bgen?str!=generic:true; };
                //! compare operator for an xmltoken
                bool operator ==(token tok)
                {
                    return tok.bgen==bgen?(bgen?tok.generic==generic:tok.literal==literal):false;
                };
                //! compare operator for an token
                bool operator !=(token tok)
                {
                    return tok.bgen==bgen?(bgen?tok.generic!=generic:tok.literal!=literal):false;
                };
                
                //! set generic string
                token &operator =(std::string &str){ generic=str; bgen=true; return *this; };
                //! set literal char
                token &operator =(char ch){ literal=ch; bgen=false; return *this; };
                
                std::string &operator()(){ return generic; };
                
                //! get generic std::string   
                std::string &get_generic(){ return generic; };
                //! get literal char
                char get_literal(){ return literal; };
                
            protected:
                //! indicates, if we have a generic string
                bool bgen;
                //! the literal character
                char literal;
                //! the generic string
                std::string generic;
            };
            
            
            //! xml input stream iterator
            /*! an iterator through all token contained in the  input stream */
            class stream_iterator
            {
            public:
                //! ctor
                stream_iterator(std::istream &is);
                
                //! dereference operator
                token& operator*(){ return curtoken; };
                //! pointer access operator
                const token* operator->(){ return &curtoken; };
                //! advances in the xml stream
                stream_iterator &operator++(){ get_next(); return *this; };
                //! advances in the xml stream
                stream_iterator &operator++(int){ get_next(); return *this; };
                
                //! returns current token
                token& get(){ return curtoken; };
                
                //! puts the token back into the stream
                void put_back( token &token ){ tokenstack.push(token); };
                
            protected:
                
                //! internal: parses the next token
                void get_next();
                
                // internally used to recognize chars in the stream
                bool is_literal( char c );
                bool is_whitespace( char c );
                bool is_newline( char c );
                bool is_stringdelimiter( char c ); // start-/endchar of a string
                
            private:
                //! input stream
                std::istream &instr;
                
                //! current token
                token curtoken;
                
                //! cdata-mode doesn't care for whitespaces in generic strings
                bool cdata_mode;
                
                //! last occured std::string delimiter
                char last_delimiter;
                
                //! char which was put back internally
                char putback_char;
                
                //! stack for put_back()'ed tokens
                std::stack<token> tokenstack;
            };
            
            
            // namespace end
        }
    }
}

#endif //JLIB_UTIL_XML_TOKENIZER_HH
