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

#include <jlib/util/util.hh>

#include <jlib/util/xml.hh>
#include <jlib/util/xmlparser.hh>

#include <cstdio>
#include <cstdarg>

// namespace declaration
namespace jlib {
    namespace util {
        namespace xml {
            
            
            // error methods
            
            const char* error::what() const throw () {
                switch(m_code) {
                case unknown:
                    return "unknown";
                case instream_error:
                    return "instream_error";
                case opentag_expected:
                    return "opentag_expected";
                case opentag_cdata_expected:
                    return "opentag_cdata_expected";
                case closetag_expected:
                    return "closetag_expected";
                case qmark_expected:
                    return "qmark_expected";
                case tagname_expected:
                    return "tagname_expected";
                case closetag_slash_expected:
                    return "closetag_slash_expected";
                case tagname_close_mismatch:
                    return "tagname_close_mismatch";
                case attr_equal_expected:
                    return "attr_equal_expected";
                case attr_value_expected:
                    return "attr_value_expected";
                case dummy:
                    return "dummy";
                default:
                    return "unknown error";
                }
            }
            
            std::string error::get_strerror() {
                return what();
            }
            
            
            // node methods
            
            node::node(std::string name) 
            { 
                set_type(node::internal);
                set_name(name);
            }

            node::ptr node::create(std::string name) {
                return node::ptr(new node(name));
            }

            node::~node() {
                clear();
            }


            bool node::load(std::istream &instream) {
                parser parser(instream);
                
                try {
                    node *pnode = this;
                    xml::document *pdoc = dynamic_cast<xml::document*>(pnode);
                    
                    if ( get_type() == node::root  && pdoc!=NULL ) {
                        pdoc->clear();
                        parser.parse_document( *pdoc );
                    }
                    else
                        parser.parse_node( *pnode );
                }
                catch(error e) {
                    e.get_error();
                    return false;
                }
                return true;
            }
            
            bool node::save( std::ostream &outstream, int indent ) const {
                // output indendation spaces
                for(int i=0;i<indent;i++)
                    outstream << ' ';
                
                // output cdata
                if (m_type == node::cdata) {
                    outstream << trim(m_cdata) << std::endl;
                    return true;   
                }
                
                // output document process information
                if (m_type == node::root) {
                    //outstream << cdata.c_str() << std::endl;
                    //return true;   
                }
                
                // output tag name
                
                if (m_type == node::root) {
                    outstream << "<?";
                }
                else {
                    outstream << '<';
                }

                outstream << get_name();
                
                attributes::const_iterator i, stop;
                i = m_attributes.begin();
                
                for(;i!=m_attributes.end();i++) {
                    outstream << ' ' << i->first << '='
                              << '\"' << jlib::util::xml::encode(i->second) << '\"';
                }
                
                switch(m_type) {
                case node::root:
                    //      break;
                case node::internal:
                    {
                        if(m_type == root) {
                            outstream << '?';
                        }
                        outstream << '>' << std::endl;
                        
                        node::list::const_iterator i = m_list.begin();
                        
                        for(;i!=m_list.end();i++)
                            if(m_type == node::root)
                                (*i)->save(outstream,indent);
                            else
                                (*i)->save(outstream,indent+1);
                        
                        // output indendation spaces
                        for(int i=0;i<indent;i++)
                            outstream << ' ';

                        if(m_type != node::root) {
                            outstream << '<' << '/'
                                      << get_name()
                                      << '>' << std::endl;
                        }
                    }
                    break;
                case node::leaf:
                    outstream << '/' << '>' << std::endl;
                    return true;
                default:
                    return false;
                }
                
                return true;
            }
            
            std::string node::get_attribute(std::string key) const {
                attributes::const_iterator i=m_attributes.find(key);
                if(i != m_attributes.end())
                    return i->second;
                else
                    return std::string();
            }

            void node::set_attribute(std::string key, std::string val) {
                attributes::iterator i=m_attributes.find(key);
                if(i != m_attributes.end())
                    i->second = val;
                else
                    m_attributes.insert(std::make_pair(key,val));
            }

            void node::add(node::ptr n) {
                m_list.push_back(n);
            }

            void node::clear() {
                for(list::iterator i=m_list.begin();i!=m_list.end();i++) {
                    node::ptr nodeptr = *i;
                    clear(nodeptr);
                }
                m_list.clear();
            }
            
            void node::clear(node::ptr n) {
                for(list::iterator i=n->get_list().begin();i!=n->get_list().end();i++) {
                    node::ptr nodeptr = *i;
                    clear(nodeptr);
                }
            }


            document::document() 
            { 
                set_type(node::root); 
                set_name("xml");
                set_attribute("version","1.0");
            }

            document::~document() {
                clear();
            }

            
        }
    }
}
// namespace end


