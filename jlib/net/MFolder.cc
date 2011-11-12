/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <jlib/net/net.hh>
#include <jlib/net/MFolder.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Regex.hh>
#include <jlib/util/Date.hh>

#include <algorithm>
#include <sstream>
#include <iostream>
#include <fstream>

const std::string DIVIDE = "From ";
const bool DEBUG = false;
const std::string INTERNAL = "FOLDER INTERNAL DATA";

namespace jlib {
    namespace net {


        MFolderBuffer::MFolderBuffer(std::string path) {
            m_scan_begin = 0;
            m_path = path;
            std::ofstream ofs(m_path.c_str(),std::ios_base::out | std::ios_base::app);
            if(!ofs) {
                throw std::ios_base::failure("unable to open "+path);
            }
        }

        MFolderBuffer::~MFolderBuffer() {
            
        }
        
        bool MFolderBuffer::modified() {
            return (m_scan_begin <  jlib::util::file::mtime(m_path));
        }

        void MFolderBuffer::scan(bool check_modified) {
            if(check_modified) {
                if(!modified()) {
                    return;
                }
            }

            m_scan_begin = time(static_cast<time_t*>(0));

            m_divide.clear();
            m_rep.clear();

            scan_divide();
            scan_headers();
        }
        
        void MFolderBuffer::set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which) {
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep[*i].set_flags(flags);
            }
        }
        void MFolderBuffer::unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which) {
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep[*i].unset_flags(flags);
            }
        }
        void MFolderBuffer::sync() {
            std::list<u_int> del;
            for(u_int i=0;i<m_rep.size();i++) {
                std::set<Email::flag_type> flags = m_rep[i].get_flags();
                if(flags.find(Email::deleted_flag) != flags.end()) {
                    del.push_back(i);
                }
            }
            remove(del);
        }

        void MFolderBuffer::fill(std::list<unsigned int> which) {
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                unsigned int j = *i;

                if(!filled(j)) {
                    std::ifstream ifs(m_path.c_str(), std::ios_base::in);
                    ifs.seekg(m_divide[j], std::ios_base::beg);
                    std::string buf;

                    // get rid of "From " pseudoheader
                    //jlib::sys::getline(ifs,buf);

                    if(j+1 == m_divide.size()) {
                        jlib::sys::getstring( ifs, buf );
                    }
                    else {
                        jlib::sys::getstring( ifs, buf, (m_divide[j+1]-m_divide[j]) );
                    }
                    //std::istringstream is(buf);
                    m_rep[j].create(buf);
                    m_filled[j] = true;
                }
            }
            
        }
        
        void MFolderBuffer::add(std::vector<Email> mails) {
            for(unsigned int i=0;i<mails.size();i++) {
                std::ofstream ofs(m_path.c_str(), std::ios_base::out | std::ios_base::app);
                std::ifstream ifs(m_path.c_str());
                ifs.seekg(-1,std::ios_base::end);
                char c;
                ifs >> c;
                
                if(c != '\n') 
                    ofs << '\n';
                ofs << mails[i].raw();
            }
            
        }
        
        void MFolderBuffer::scan_divide() {
            std::ifstream is(m_path.c_str(), std::ios_base::in);
            parse_divide(is, m_divide, DIVIDE);
        }

        void MFolderBuffer::scan_headers() {
            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): entering"<<std::endl
                          <<"scanning file " << m_path << std::endl;
            }
            m_rep.clear();
            m_filled.clear();
            std::ifstream ifs(m_path.c_str(), std::ios_base::in);
            
            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): "<<m_divide.size()
                          << " emails from which to parse headers" <<std::endl;
            }
            for(unsigned int i=0;i<m_divide.size();i++) {
                std::string buf;
                ifs.seekg(m_divide[i]);
                if(i+1 == m_divide.size()) {
                    if(getenv("JLIB_NET_DEBUG")) {
                        std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): "
                                  << "reading email " <<i<<" of "
                                  <<m_divide.size()<<"; from byte " << m_divide[i]
                                  << std::endl;
                    }
                    // get rid of "From " pseudoheader
                    //jlib::sys::getline(ifs,buf);
                    jlib::sys::getstring( ifs, buf );
                }
                else {
                    if(getenv("JLIB_NET_DEBUG")) {
                        std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): "
                                  << "reading email " <<i<<" of "
                                  <<m_divide.size()<<"; " << (m_divide[i+1]-m_divide[i]) 
                                  << " bytes" << std::endl;
                    }
                    // get rid of "From " pseudoheader
                    //jlib::sys::getline(ifs,buf);
                    jlib::sys::getstring( ifs, buf, (m_divide[i+1]-m_divide[i]) );
                }
                if(getenv("JLIB_NET_DEBUG")) {
                    std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): "
                              <<"constructing istringstream from buf" << std::endl;
                }

                u_int size = buf.length();

                // don't try to parse the whole email, just grab the headers
                unsigned int p = buf.find("\n\n");
                if(p != buf.npos) {
                    //buf.erase(p+2);
                    buf.erase(p);
                }

                //std::istringstream is(buf);
                if(getenv("JLIB_NET_DEBUG")) {
                    std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): "
                              <<"constructing Email object and pushing onto m_rep" << std::endl;
                }
                
                m_rep.push_back(Email(buf));
                m_rep.back().set_data_size(size);
                m_filled.push_back(false);
            }

            ifs.close();
            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"jlib::net::MFolderBuffer::scan_headers(): leaving"<<std::endl;
            }
        }
        
        void MFolderBuffer::remove(std::list<unsigned int> which) {
            std::vector<unsigned int> phys;
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                phys.push_back(*i);
            }
            std::sort(phys.begin(), phys.end());

            if(getenv("JLIB_NET_MBOX_DEBUG")) {
                for(unsigned int i=0;i<phys.size();i++) {
                    std::cout << "phys[i] = " << phys[i] << std::endl;
                }
            }

            std::vector<long> pts;
            for(unsigned int i=0;i<phys.size();i++) {
                pts.push_back(m_divide[phys[i]]);
                if(phys[i]+1 != m_divide.size()) {
                    pts.push_back(m_divide[phys[i]+1]);
                }
            }

            if(getenv("JLIB_NET_MBOX_DEBUG")) {
                for(unsigned int i=0;i<pts.size();i++) {
                    std::cout << "pts[i] = " << pts[i] << std::endl;
                }
            }

            jlib::util::file::kill(m_path, pts);
        }
        /*        

        void MFolder::remove(unsigned int i) {
            std::vector<long> pts;
            unsigned int j = m_logical[i];
            pts.push_back(m_divide[j]);
            if(j+1 != m_divide.size()) {
                pts.push_back(m_divide[j+1]);
            }
            jlib::util::file::kill(m_path, pts);
        }
        */
    }
}
