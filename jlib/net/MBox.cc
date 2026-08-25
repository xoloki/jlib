/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <paths.h>

#include <algorithm>

#include <jlib/net/MFolder.hh>
#include <jlib/net/MBox.hh>

#include <filesystem>

#include <jlib/sys/sys.hh>
#include <jlib/sys/Directory.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Date.hh>
#include <jlib/util/URL.hh>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

const std::string SYS_MAIL_DIR = std::string(_PATH_MAILDIR);

namespace jlib {
	namespace net {

        MBoxBuf::MBoxBuf(jlib::util::URL url) {
            if(getenv("JLIB_NET_MBOX_DEBUG"))
                std::cout << "jlib::net::MBoxBuf::MBoxBuf("+url()+")"<<std::endl;
            if(getenv("JLIB_NET_MBOX_DEBUG"))
                std::cout << "\turl.get_path() = '"<<url.get_path()<<"'"<<std::endl;
            if(url.get_path() == "") {
                if(getenv("JLIB_NET_MBOX_DEBUG"))
                    std::cout << "\turl.get_path() == \"\""<<std::endl;
                m_maildir = std::string(getenv("HOME"))+std::string("/mail");
                if(getenv("JLIB_NET_MBOX_DEBUG"))
                    std::cout << "\tm_maildir = '"<<m_maildir<<"'"<<std::endl;
            }
            else {
                if(getenv("JLIB_NET_MBOX_DEBUG"))
                    std::cout << "\turl.get_path() != \"\""<<std::endl;
                m_maildir = url.get_path();
                if(getenv("JLIB_NET_MBOX_DEBUG"))
                    std::cout << "\tm_maildir = '"<<m_maildir<<"'"<<std::endl;
            }
            if(url["canonical"] != "false") {
                m_inbox = SYS_MAIL_DIR+std::string("/")+std::string(getenv("USER"));
                m_canonical = true;
            }
            else {
                m_canonical = false;
            }
        }
        
        void MBoxBuf::list() {
            if(getenv("JLIB_NET_MBOX_DEBUG"))
                std::cout << "jlib::net::MBoxBuf::list()"<<std::endl;
            struct stat mystat;
            if(getenv("JLIB_NET_MBOX_DEBUG"))
                std::cout << "\tm_maildir = '"<<m_maildir<<"'"<<std::endl;
            if(stat(m_maildir.c_str(), &mystat) == -1) {
                if(mkdir(m_maildir.c_str(), 0700) == -1)
                    throw exception("error creating mail dir '"+m_maildir+"'");
            }
            
            if(m_canonical) {

                std::string sent_mail = m_maildir+"/sent-mail";
                
                if(stat(sent_mail.c_str(), &mystat) == -1) {
                    std::fstream fs(sent_mail.c_str(), std::ios_base::out|std::ios_base::app);
                    fs.close();
                }
                
                std::string sent_dir = m_maildir+"/sent";
                
                if(stat(sent_dir.c_str(), &mystat) == -1) {
                    if(mkdir(sent_dir.c_str(), 0700) == -1)
                        throw exception("error creating sent mail dir '"+sent_dir+"'");
                }
                
                // move sent-mail folder if it's time to do so
                jlib::util::Date today;
                jlib::net::MFolder mbox(sent_mail);
                mbox.scan();
                std::vector<jlib::net::Email> sent_mails;
                std::copy(mbox.begin(),mbox.end(),std::back_inserter(sent_mails));
                //cout << sent_mails.size() << " messages in " << sent_mail << endl;
                if(sent_mails.size() > 0) {
                    std::sort(sent_mails.begin(), sent_mails.end());
                    jlib::net::Email beg(sent_mails[0]);
                    jlib::util::Date beg_date;
                    beg_date.set(beg.headers()["DATE"]);
                    
                    if(today.mon() > beg_date.mon()) {
                        std::string cmd = "cp "+sent_mail+" "+sent_dir+"/sent-mail-"+beg_date.get("%b-%Y");
                        if(system(cmd.c_str())) {
                            throw exception("ERROR: moving old sent-mail dir, command '"+cmd+"' failed");
                        }
                        else {
                            std::ofstream fs(sent_mail.c_str(), std::ios_base::out|std::ios_base::trunc);
                            fs.close();
                        }
                    }
                }
            }

            std::list<std::string> path;
            tree(path,m_root);
        }

        void MBoxBuf::fill(std::list<std::string> path) {
            if(path.size() == 1 && path.front() == "INBOX") {
                iterator i = m_root.begin();
                if(!i->is_filled()) {
                    i->set_folder(new MFolder(m_inbox));
                }
            }
            else {
                std::string pathstr = MailNode::pathstr(path);
                iterator i = m_root.find(path);
                if(i != m_root.end() && !i->is_filled()) {
                    i->set_folder(new MFolder(m_maildir+pathstr));
                }
            }
        }

        void MBoxBuf::create_folder(std::list<std::string> path) {
            std::fstream box( (m_maildir+MailNode::pathstr(path)).c_str(), 
                              (std::ios_base::out|std::ios_base::app) );
            box.close();
        }

        void MBoxBuf::delete_folder(std::list<std::string> path) {
            if(is_inbox(path)) return;

            // This was sys::shell("rm "+m_maildir+pathstr(path)).  The folder
            // name comes from the filesystem or, for an IMAP account being
            // mirrored, from the server -- so it was a command line built out
            // of a string jlib did not choose, handed to /bin/sh, running rm.
            // A folder named "x; rm -rf $HOME" did that.  A folder named
            // "My Mail" merely failed, because rm saw two arguments.
            std::error_code ec;

            std::filesystem::remove_all(m_maildir + MailNode::pathstr(path), ec);

            if(ec) {
                throw exception("could not delete folder: " + ec.message());
            }
        }

        void MBoxBuf::rename_folder(std::list<std::string> path, std::list<std::string> npath) {
            if(is_inbox(path)) return;

            // Was sys::shell("mv "+...+" "+...), with the same hazard twice.
            std::error_code ec;

            std::filesystem::rename(m_maildir + MailNode::pathstr(path),
                                    m_maildir + MailNode::pathstr(npath), ec);

            if(ec) {
                throw exception("could not rename folder: " + ec.message());
            }
        }

        void MBoxBuf::tree(std::list<std::string> path, reference root) {
            if(m_canonical && path.size() == 0) {
                // add INBOX
                root.push_back(MailNode());
            }

            std::string pathstr = m_maildir+MailNode::pathstr(path);
            try {
                jlib::sys::Directory dir(pathstr);
                std::vector<std::string> ls = dir.list();
                bool folder,parent;
                
                if(ls.size() >= 1) {
                    for(u_int i=0;i<ls.size();i++) {
                        path.push_back(ls[i]);
                        folder = dir.is(ls[i],jlib::sys::REGULAR);
                        parent = dir.is(ls[i],jlib::sys::DIRECTORY);
                        root.push_back(MailNode(path,folder,parent));
                        tree(path,root.back());
                        path.pop_back();
                    }
                }
            }
            catch(std::exception& e) {
                
            }
        }

        bool MBoxBuf::is_inbox(std::list<std::string> path) {
            return (path.size() == 1 && path.front() == "INBOX");
        }        

    }
}

