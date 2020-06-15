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
#include <jlib/util/util.hh>
#include <jlib/util/xmlparser.hh>


// namespace declaration
namespace jlib {
    namespace util {
        namespace xml {
            
            
            // xmlparser methods
            
            parser::parser( std::istream &inputstream )
                :instream( inputstream ),tokenizer( inputstream )
            {
            }
            
            void parser::parse_document( document &doc ) {
                //doc.set_name("xml");
                //doc.nodenamehandle = contextptr->insert_tagname( rootstr );
                
                //! \todo: check if istream::fail() returns false when closed file
                if (instream.fail())
                    throw error(instream_error);
                
                parse_header( doc );
                parse_node( doc );
            }
            
            // parses the header, ie processing instructions and doctype tag
            void parser::parse_header( document &doc ) {
                tokenizer++;
                token token1 = *tokenizer;
                if (token1 != '<')
                    throw error( opentag_expected );
                
                tokenizer++;
                token token2 = *tokenizer;
                if (token2 != '?')
                    throw error( qmark_expected );
                
                tokenizer++;
                token token3 = *tokenizer;
                
                if (token3.is_literal()) {
                    // no "<?XML" was found
                    tokenizer.put_back( token3 );
                    tokenizer.put_back( token2 );
                    tokenizer.put_back( token1 );
                }
                else {
                    // token3 contains 'xml' string
                    
                    // /todo parse process instructions
                    
                    parse_attributes( doc.get_attributes() );
                }
                
                tokenizer++;
                if (*tokenizer != '?')
                    throw error( qmark_expected );
                
                tokenizer++;
                if (*tokenizer != '>')
                    throw error( closetag_expected );
            }
            
            // parses the contents of the current node
            void parser::parse_node( node& n ) {
                while (1==1) {
                    tokenizer++;
                    token token1 = *tokenizer;
                    
                    //! /todo replace the EOF with an tokenizer.end() iterator
                    if (token1 == char(EOF))
                        break;
                    
                    if (!token1.is_literal()) {
                        node::ptr cdatanode = node::create("cdata");
                        
                        // parse cdata section(s) and return
                        cdatanode->set_type( node::cdata );
                        //cdatanode->cdata.clear();
                        
                        while(!token1.is_literal()) {
                            cdatanode->get_cdata().append(token1.get_generic());
                            tokenizer++;
                            token1 = *tokenizer;
                        }
                        tokenizer.put_back( token1 );
                        //std::cout << "jlib::util::xml::parser::parse_node("
                        //<<node.get_nodename()<<"): cdatanode->cdata = " 
                        //<< cdatanode.cdata <<std::endl;
                                // put node into nodelist
                        n.get_list().push_back( cdatanode );
                        
                        continue;
                    }
                    
                    // continue parsing node content
                    
                    if (token1 != '<')
                        throw error(opentag_cdata_expected);
                    
                    // get node name
                    tokenizer++;
                    token token2 = *tokenizer;
                    if (token2.is_literal()) {
                        // check if closing '</...>' follows
                        if (token2 == '/') {
                            // return, we have a closing node with no more content
                            tokenizer.put_back( token2 );
                            tokenizer.put_back( token1 );
                            return;
                        }			
                        else
                            throw error(tagname_expected);
                    }
                    
                    // create subnode
                    node::ptr subnode = node::create(token2.get_generic());
                    
                    // parse attributes
                    parse_attributes(subnode->get_attributes());
                    
                    // check for single tag
                    tokenizer++;
                    token token3 = *tokenizer;
                    if (token3 == '/' ) {
                        // node has finished
                        tokenizer++;
                        token token4 = *tokenizer;
                        if (token4 != '>' )
                            throw error(closetag_expected);
                        
                        subnode->set_type(node::leaf);
                        
                        // put node into nodelist
                        n.get_list().push_back( subnode );
                        
                        continue;
                    }
                    
                    // parse possible sub nodes
                    parse_node(	*subnode.operator->() );
                    
                    // put node into nodelist
                    n.get_list().push_back( subnode );
                    
                    // parse end tag
                    tokenizer++;
                    if (*tokenizer != '<' )
                        throw error(opentag_expected);
                    
                    tokenizer++;
                    if (*tokenizer != '/' )
                        throw error(closetag_slash_expected);
                    
                    tokenizer++;
                    token1 = *tokenizer;
                    if (token1.is_literal())
                        throw error(tagname_expected);
                    
                    // check if open and close tag names are identical
                    if (token1 != token2 )
                        throw error(tagname_close_mismatch);
                    
                    //! /todo: can closing tags contain attributes, too?
                    tokenizer++;
                    if (*tokenizer != '>')
                        throw error(opentag_expected);
                }
            }
            
            // parses tag attributes
            void parser::parse_attributes( node::attributes &attr ) {
                while(1==1) {
                    tokenizer++;
                    token token1 = *tokenizer;
                    
                    if (token1.is_literal()) {
                        tokenizer.put_back( token1 );
                        break;
                    }
                    
                    tokenizer++;
                    if (*tokenizer != '=')
                        throw error(attr_equal_expected);
                    
                    tokenizer++;
                    token token2 = *tokenizer;
                    if (token2.is_literal())
                        throw error(attr_value_expected);

                    std::string key = jlib::util::xml::decode(token1.get_generic());
                    std::string val = jlib::util::xml::decode(token2.get_generic());
                    
                    if(val.length() > 1) {
                        if( (val[0] == '"' && val[val.length()-1] == '"') ||
                            (val[0] == '\'' && val[val.length()-1] == '\'') ) {
                            val = val.substr(1,val.length()-2);
                        }
                    }


                    // insert attribute into the map
                    node::attributes::value_type attrpair(key,val);
                    attr.insert(attrpair);
                }
            }


            std::string parser::clean(std::string s) {
                std::string ret = s;
                if(ret.length() > 1) {
                    if( (ret[0] == '"' && ret[ret.length()-1] == '"') ||
                        (ret[0] == '\'' && ret[ret.length()-1] == '\'') ) {
                        ret = ret.substr(1,ret.length()-2);
                    }
                }

                std::map<std::string,std::string> escape;
                escape["&amp;"] = "&";
                escape["&lt;"] = "<";
                escape["&gt;"] = ">";

                std::map<std::string,std::string>::iterator j = escape.begin();
                std::string::size_type i;
                while(j != escape.end()) {
                    while( (i=ret.find(j->first)) != std::string::npos ) {
                        ret.replace(i,j->first.length(),j->second);
                    }
                    j++;
                }

                return ret;
            }
            
            // namespace end
        }
    }
}
