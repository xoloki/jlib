/* -*- mode: C++ c-basic-offset: 4 -*-
 * Directory.C
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/sys/Directory.hh>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

namespace jlib {
    namespace sys {
        Directory::Directory(std::string p_path) {
            m_path = p_path;
            if(m_path != "/" && m_path[m_path.length()-1] == '/') {
                m_path = m_path.substr(0,m_path.length()-1);
            }
            if(m_path == "/") {
                m_name = "/";
            }
            else {
		std::string::size_type i = m_path.find_last_of("/");
		if(i != std::string::npos)
		    m_name = m_path.substr(i);
		else
		    m_name = m_path;
            }
        }
        
        Directory::Directory(const Directory& p_copy) {
            copy(p_copy);
        }
        
        Directory& Directory::operator=(const Directory& p_copy) {
            copy(p_copy);
            return (*this);
        }
        
        void Directory::copy(const Directory& p_copy) {
            m_path = p_copy.m_path;
            m_name = p_copy.m_name;
        }
        
        
        std::string Directory::get_path() const {
            return m_path;
        }
        
        std::string Directory::get_name() const {
            return m_name;
        }
        
        
        std::vector<std::string> Directory::list_files(bool p_full_path) const {
            std::vector<std::string> ret = list(static_cast<jlib::sys::file_type>(REGULAR));
            return ret;
        }
        
        std::vector<std::string> Directory::list_dirs(bool p_full_path) const {
            std::vector<std::string> ret = list((jlib::sys::file_type)DIRECTORY);
            return ret;        
        }
        
        std::vector<Directory> Directory::list_subdirs() const {
            std::vector<std::string> d = list((jlib::sys::file_type)DIRECTORY, true);
            std::vector<Directory> ret;
            for(unsigned int i=0; i<d.size(); i++) {
                ret.push_back(Directory(d[i]));
            }
            return ret;
        }
        std::vector<std::string> Directory::list(file_type p_type, bool p_full_path, bool p_show_dots) const {
            std::vector<std::string> ret;
            
            DIR* dir = opendir(m_path.c_str());
            if(dir == NULL) {
                throw exception("Error opening directory "+m_path);
            }
            struct dirent* entry = NULL;
            struct stat mystat;
            
            while( (entry = readdir(dir)) != NULL ) {
                std::string file(entry->d_name);
                std::string file_path = m_path+"/"+file;
                bool do_insert = false;
                if(stat(file_path.c_str(), &mystat) == -1) {
                    throw exception("error running stat in Directory::list()");
                }
                
                switch(p_type) {
                case REGULAR:
                    do_insert = S_ISREG(mystat.st_mode);
                    break;
                case SYMLINK:
                    do_insert = S_ISLNK(mystat.st_mode);
                    break;
                case DIRECTORY:
                    do_insert = S_ISDIR(mystat.st_mode);
                    break;
                case CHAR_DEV:
                    do_insert = S_ISCHR(mystat.st_mode);
                    break;
                case BLOCK_DEV:
                    do_insert = S_ISBLK(mystat.st_mode);
                    break;
                case FIFO:
                    do_insert = S_ISFIFO(mystat.st_mode);
                    break;
                case SOCKET:
                    do_insert = S_ISSOCK(mystat.st_mode);
                    break;
                case ALL:
                    do_insert = true;
                    break;
                default:
                    do_insert = true;
                    break;
                }
                
                if(do_insert) {
                    if(p_full_path) {
                        ret.push_back(file_path);
                    }
                    else {
                        ret.push_back(file);
                    }
                    if( (file == "." || file == "..") && !p_show_dots ) {
                        ret.pop_back();
                    }
                }
                
            }
            closedir(dir);
            
            return ret;
        }
        
        bool Directory::is(std::string p_file, file_type p_type) const {
            std::string file_path = m_path+"/"+p_file;
            struct stat mystat;
            if(stat(file_path.c_str(), &mystat) == -1) {
                throw exception("error running stat in Directory::is("+p_file+")");
            }
            
            switch(p_type) {
            case REGULAR:
                return (S_ISREG(mystat.st_mode));
            case SYMLINK:
                return (S_ISLNK(mystat.st_mode));
            case DIRECTORY:
                return (S_ISDIR(mystat.st_mode));
            case CHAR_DEV:
                return (S_ISCHR(mystat.st_mode));
            case BLOCK_DEV:
                return (S_ISBLK(mystat.st_mode));
            case FIFO:
                return (S_ISFIFO(mystat.st_mode));
            case SOCKET:
                return (S_ISSOCK(mystat.st_mode));
            case ALL:
                return true;
            default:
                return false;
            }
        }
        
        Directory Directory::sub(std::string file) const {
            return Directory(m_path+"/"+file);
        }
        
    }
}

