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
#ifndef JLIB_UTIL_XML_HH
#define JLIB_UTIL_XML_HH

// needed includes
#include <string>
#include <list>
#include <map>
#include <iostream>
#include <exception>

#include <jlib/sys/object.hh>
#include <glibmm/refptr.h>

namespace jlib {
    namespace util {
        namespace xml {
            
            /*! \namespace xml
              all xmlpp classes, typedefs and enums are set into the xmlpp
              namespace to avoid conflicts with other similar classes */
            
            
            // classes
            
            //! xml parsing error codes enumeration
            enum errorcode {
                //! unspecified or unknown error
                unknown=0,
                //! error in the infile stream
                instream_error,			
                
                //! expected an open tag literal '<'
                opentag_expected,
                //! expected a '<' or cdata
                opentag_cdata_expected,
                //! expected an '>' closing tag literal
                closetag_expected,
                //! expected an '?' after '<'
                qmark_expected,
                //! expected a tag name after '<' or '</'
                tagname_expected,
                //! expected a '/' after closing tag literal '<'
                closetag_slash_expected,
                //! tag name from start and end tag mismatch
                tagname_close_mismatch,
                
                //! expected '=' after attribute name
                attr_equal_expected,
                //! expected value after 'a' in attribute
                attr_value_expected,
                
                //! dummy error code
                dummy	
            };
            
            
            //! xml error class
            /*! contains an xmlerrorcode and is thrown while parsing xml input */
            class error : public std::exception {
            public:
                //! constructor
                error( errorcode code ){ m_code = code; };
                virtual ~error() throw() {}
                //! returns the error code
                errorcode get_error(){ return m_code; };
                //! returns the std::string representation of the error code
                std::string get_strerror();
                virtual const char* what() const throw();
            protected:
                errorcode m_code;
            };

            //!  node
            class node : public sys::Object {
            public:
                typedef Glib::RefPtr<node> ptr;
                typedef const Glib::RefPtr<node> const_ptr;
                typedef Glib::RefPtr<const node> ptr_const;
                typedef std::map<std::string, std::string> attributes;
                typedef std::list<ptr> list;

                typedef enum { 
                    //! internal node, can contain subnodes
                    internal,
                    //! a leaf node, which contains no further nodes, eg. <img/>
                    leaf,
                    //! document root node
                    root, 
                    //! cdata node, which only contains char data
                    cdata
                } type;

                //! ctor
            protected:
                node(std::string name="");
            public:
                static node::ptr create(std::string name="");
                //! dtor
                virtual ~node();
                
                //! returns type of node
                type get_type() const { return m_type; };
                void set_type(type n){ m_type=n; };

                //! returns the node name, if nodetype != nt_cdata
                std::string get_name() const { return m_name; }
                void set_name(std::string n) { m_name = n; }

                //! returns subnode list
                list& get_list(){ return m_list; };
                const list& get_list() const { return m_list; };

                //! returns attributes of the tag
                attributes &get_attributes(){ return m_attributes; };
                const attributes& get_attributes() const { return m_attributes; };
                
                std::string get_attribute(std::string key) const;
                void set_attribute(std::string key, std::string val);

                //! returns cdata string
                std::string& get_cdata(){ return m_cdata; };
                const std::string& get_cdata() const { return m_cdata; };
                void set_cdata(std::string n) { m_cdata = n; }
                
                //! loads  node from input stream
                bool load( std::istream &instream );
                //! saves node to  output stream
                bool save( std::ostream &outstream, int indent=0 ) const;
                
                void add(node::ptr n);
                //! recursively delete all child nodes, but not this one
                void clear();

                //! recursively delete all child nodes of n
                static void clear(node::ptr n); 

            protected:
                //! nodetype
                type m_type;
                
                //! attributes of the tag
                attributes m_attributes;
                
                //! char data
                std::string m_cdata;

                //! name of node
                std::string m_name;
                
                //! stl list with subnodes
                list m_list;
            };
            
            class cdata : public node {
            protected:
                cdata() { set_name("cdata"); set_type(node::cdata); }
                cdata(std::string data) { set_name("cdata"); set_type(node::cdata); set_cdata(data); }
            public:
                static node::ptr create() { return node::ptr(new cdata()); }
                static node::ptr create(std::string data) { return node::ptr(new cdata(data)); }

                virtual ~cdata() {}
            };
            
            
            //!  document
            class document: public node {
            public:
                //! ctor
                document();
                virtual ~document();
            };
            
        }
    }
}

#endif //JLIB_UTIL_XML_HH
