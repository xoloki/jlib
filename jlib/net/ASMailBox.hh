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

#ifndef JLIB_NET_ASMAILBOX_HH
#define JLIB_NET_ASMAILBOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <list>

#include <sigc++/sigc++.h>

#include <jlib/sys/ASServent.hh>
#include <jlib/net/Email.hh>

namespace jlib {
	namespace net {

        struct folder_attributes {
            bool is_parent;
            bool is_select;
            long mtime;
        };

        struct message_attributes {
            bool is_seen;
            bool is_answered;
            bool is_deleted;
        };
        
        typedef std::list<std::string>                         folder_path_type;
        typedef std::map<int, Email>                           folder_data_type;
        typedef std::list<int>                                 folder_indx_type;
        typedef std::set<int>                                  folder_sort_indx_type;

        struct folder_info_type {
            folder_path_type path;
            folder_attributes attr;
        };

        struct folder_type {
            folder_info_type info;
            folder_data_type data;
        };
        
        inline bool operator!=(const folder_attributes& t1, const folder_attributes& t2) {
            return (t1.is_parent != t2.is_parent || t1.is_select != t2.is_select);
        }

        inline bool operator==(const folder_attributes& t1, const folder_attributes& t2) {
            return (t1.is_parent == t2.is_parent && t1.is_select == t2.is_select);
        }

        inline bool operator!=(const folder_info_type& t1, const folder_info_type& t2) {
            return (t1.path != t2.path || t1.attr != t2.attr);
        }

        inline bool operator==(const folder_info_type& t1, const folder_info_type& t2) {
            return (t1.path == t2.path && t1.attr == t2.attr);
        }

        class MailBoxRequest {
        public:
            enum request_type {
                SET_PASSWORD,
                INITIALIZE,
                LIST_FOLDERS,

                CREATE_FOLDER,
                DELETE_FOLDER,
                RENAME_FOLDER,
                EXPUNGE_FOLDER, 

                CHECK_RECENT,

                LIST_MESSAGES, 

                SET_MESSAGE_FLAGS,
                UNSET_MESSAGE_FLAGS,

                COPY_MESSAGES,
                APPEND_MESSAGE,

                LOAD_MESSAGES,
            };

            MailBoxRequest(request_type t);

            MailBoxRequest(request_type t, std::string p);

            MailBoxRequest(request_type t, folder_info_type src, 
                           folder_indx_type indx=folder_indx_type());

            MailBoxRequest(request_type t, folder_info_type src, folder_info_type dst, 
                           folder_indx_type indx=folder_indx_type());

            MailBoxRequest(request_type t, folder_info_type src, Email email, 
                           folder_indx_type indx=folder_indx_type());

            request_type type;

            folder_info_type src;
            folder_info_type dst;

            folder_indx_type indx;

            Email data;
            std::string password;

            int id;
            static sys::sync<int> master;

            bool operator<(const MailBoxRequest& r) const {
                if(this->type == r.type) {
                    if(this->type == LIST_MESSAGES) {
                        if(!this->indx.empty() && r.indx.empty()) {
                            return true;
                        } else if(this->indx.empty() && !r.indx.empty()) {
                            return false;
                        }
                    }
                    return r.id < this->id;
                } else {
                    return (static_cast<int>(this->type) < static_cast<int>(r.type));
                }
            }
        };

        class MailBoxResponse {
        public:
            enum response_type { 
                NONE,
                ERROR,
                LOGIN_ERROR,
                INITIALIZED, 
                STATUS, 
                PROGRESS, 
                
                FOLDERS_LOADED, 
                
                FOLDER_LISTED, 
                FOLDER_CREATED, 
                FOLDER_DELETED, 
                FOLDER_RENAMED, 
                FOLDER_EXPUNGED, 
                
                MESSAGE_LISTED, 
                MESSAGE_LOADED,
                MESSAGES_COPIED,
                MESSAGE_APPENDED,

                MESSAGE_FLAGS_SET,
                MESSAGE_FLAGS_UNSET,

            };

            MailBoxResponse(response_type t);

            MailBoxResponse(response_type t, std::string text, float p=0);

            MailBoxResponse(response_type t, folder_info_type src, 
                            folder_indx_type indx=folder_indx_type());

            MailBoxResponse(response_type t, folder_info_type src, folder_info_type dst, 
                           folder_indx_type indx=folder_indx_type());

            MailBoxResponse(response_type t, folder_info_type src, Email email, 
                           folder_indx_type indx=folder_indx_type());


            response_type type;

            folder_info_type src;
            folder_info_type dst;

            folder_indx_type indx;

            float pct;

            Email data;
            std::string text;
            int priority;
        };

        /**
         * class ASMailBox is an ASServent which takes requests for the various mailbox 
         * tasks, and asynchronously sends SigC::Signals when it finishes them
         * 
         */
        class ASMailBox : public sys::ASServent<MailBoxRequest,MailBoxResponse> {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::net::ASMailBox exception")+( (msg=="")?"":": ")+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef std::map<folder_path_type, folder_type> folder_map_type;

            ASMailBox();
            virtual ~ASMailBox();

            virtual void handle(const MailBoxRequest& r);
            virtual void handle(const MailBoxResponse& r);

            void init();
            void reinit();
            void set_password(std::string password);
            void list_folders();

            void create_folder(folder_info_type folder);
            void delete_folder(folder_info_type folder);
            void rename_folder(folder_info_type src, folder_info_type dst);
            void expunge_folder(folder_info_type folder);

            void check_recent(folder_info_type folder);

            void list_messages(folder_info_type folder);
            void load_messages(folder_info_type folder, folder_indx_type indx);
            void copy_messages(folder_info_type src, folder_info_type dst, 
                               folder_indx_type indx);
            
            void append_message(folder_info_type folder, Email email);
            
            void set_message_flags(folder_info_type folder, 
                                   folder_indx_type indx, Email email);
            void unset_message_flags(folder_info_type folder, 
                                     folder_indx_type indx, Email email);


            virtual void on_init() = 0;
            virtual void on_set_password(std::string password) = 0;

            virtual void on_list_folders() = 0;

            virtual void on_create_folder(folder_info_type folder) = 0;
            virtual void on_delete_folder(folder_info_type folder) = 0;
            virtual void on_rename_folder(folder_info_type src, folder_info_type dst) = 0;
            virtual void on_expunge_folder(folder_info_type folder) = 0;

            virtual void on_check_recent(folder_info_type folder) = 0;

            virtual void on_list_messages(folder_info_type folder, folder_indx_type indx) = 0;
            virtual void on_load_messages(folder_info_type folder, folder_indx_type indx) = 0;
            virtual void on_copy_messages(folder_info_type src, folder_info_type dst, 
                                          folder_indx_type indx) = 0;

            virtual void on_append_message(folder_info_type folder, Email email) = 0;

            virtual void on_set_message_flags(folder_info_type folder, 
                                              folder_indx_type indx, Email email) = 0;
            virtual void on_unset_message_flags(folder_info_type folder, 
                                                folder_indx_type indx, Email email) = 0;



            /**
             * signal an error
             * std::string: text description of error
             */
            sigc::signal1<void,std::string> 
            error;

            /**
             * signal an error during login
             */
            sigc::signal0<void> 
            login_error;
            
            /**
             * signal status
             * std::string: text description of error
             */
            sigc::signal1<void,std::string> 
            status;

            /**
             * signal that the mailbox has been initialized
             */
            sigc::signal0<void> 
            initialized;

            /**
             * signal the progress of current action
             * std::string: text description of action
             * float:       amount done (0.0 -> 1.0)
             */
            sigc::signal2<void,std::string,float> 
            progress;

            /**
             * signal when a message has been listed. args:
             * std::list<std::string>: path of folder
             * int:                    which message (count from 0)
             * std::string:            header data
             * message_attributes:     attributes of message
             */
            sigc::signal3<void,folder_info_type,int,Email> 
            message_listed;

            /**
             * signal when a message has been loaded. args:
             * std::list<std::string>: path of folder
             * int:                    which message
             * std::string:            text of message
             * message_attributes:     attributes of message
             */
            sigc::signal3<void,folder_info_type,int,Email> 
            message_loaded;

            /**
             * signal when message flags have been set.  args:
             * std::list<std::string>: path of folder
             * std::set<int>:          which messages
             * std::string:            text of message
             * message_attributes:     attributes of message
             */
            sigc::signal3<void,folder_info_type,folder_indx_type,Email> 
            message_flags_set;

            /**
             * signal when message flags have been set.  args:
             * std::list<std::string>: path of folder
             * std::set<int>:          which messages
             * std::string:            text of message
             * message_attributes:     attributes of message
             */
            sigc::signal3<void,folder_info_type,folder_indx_type,Email> 
            message_flags_unset;

            /**
             * signal when a message has been loaded. args:
             * std::list<std::string>: path of folder
             * int:                    which message
             * std::string:            text of message
             * message_attributes:     attributes of message
             */
            sigc::signal3<void,folder_info_type,folder_info_type,folder_indx_type> 
            messages_copied;

            /**
             * signal when folder info is listed
             * std::list<std::string>: path of folder
             * folder_attributes:      folder attributes
             */
            sigc::signal1<void,folder_info_type>  
            folder_listed;
            

            /**
             * signal when folder is created
             * std::list<std::string>: path of folder
             * folder_attributes:      folder attributes
             */
            sigc::signal1<void,folder_info_type> 
            folder_created;

            /**
             * signal when folder is deleted
             * std::list<std::string>: path of folder
             * folder_attributes:      folder attributes
             */
            sigc::signal1<void,folder_info_type> 
            folder_deleted;
            
            /**
             * signal when folder is renamed
             * std::list<std::string>: path of folder
             * std::list<std::string>: path of folder
             * folder_attributes:      folder attributes
             */
            sigc::signal2<void,folder_info_type,folder_info_type> 
            folder_renamed;

            /**
             * signal when folder is expunged
             * std::list<std::string>: path of folder
             * folder_attributes:      folder attributes
             */
            sigc::signal1<void,folder_info_type> 
            folder_expunged;

            /**
             * signal when all folder info is loaded
             * folder_map_type: folder info
             */
            sigc::signal1<void,folder_map_type> 
            folders_loaded;

        protected:
            void clear();
            void clear(MailBoxRequest::request_type type);
            void clear(MailBoxResponse::response_type type);

            sys::sync<folder_map_type> m_folders;
        };
        
    }
}

#endif //JLIB_NET_ASMAILBOX_HH
