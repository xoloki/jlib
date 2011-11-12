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

#include <jlib/net/ASMailBox.hh>

namespace jlib {
	namespace net {

        sys::sync<int> MailBoxRequest::master(0);

        MailBoxRequest::MailBoxRequest(request_type t)
            : type(t)
        {
            sys::auto_lock<Glib::Mutex> lock(master);
            id = master.ref()++;;
        }

        MailBoxRequest::MailBoxRequest(request_type t, std::string p)
            : type(t),
              password(p)
        {
            sys::auto_lock<Glib::Mutex> lock(master);
            id = master.ref()++;;
        }

        MailBoxRequest::MailBoxRequest(request_type t, 
                                       folder_info_type s, 
                                       folder_indx_type i)
            : type(t),
              src(s),
              indx(i)
        {
            sys::auto_lock<Glib::Mutex> lock(master);
            id = master.ref()++;;
        }

        MailBoxRequest::MailBoxRequest(request_type t, 
                                       folder_info_type s, 
                                       folder_info_type d, 
                                       folder_indx_type i)
            : type(t),
              src(s),
              dst(d),
              indx(i)
        {
            sys::auto_lock<Glib::Mutex> lock(master);
            id = master.ref()++;;
        }

        MailBoxRequest::MailBoxRequest(request_type t, 
                                       folder_info_type s, 
                                       Email e, 
                                       folder_indx_type i)
            : type(t),
              src(s),
              data(e),
              indx(i)
        {
            sys::auto_lock<Glib::Mutex> lock(master);
            id = master.ref()++;;
        }


        MailBoxResponse::MailBoxResponse(response_type t)
            : type(t)
        {
            priority = static_cast<int>(t);
        }

        MailBoxResponse::MailBoxResponse(response_type t, std::string p, float pct)
            : type(t),
              text(p),
              pct(pct)
        {
            priority = static_cast<int>(t);
        }

        MailBoxResponse::MailBoxResponse(response_type t, 
                                       folder_info_type s, 
                                       folder_indx_type i) 
            : type(t),
              src(s),
              indx(i)
        {
            priority = static_cast<int>(t);
        }

        MailBoxResponse::MailBoxResponse(response_type t, 
                                       folder_info_type s, 
                                       folder_info_type d, 
                                       folder_indx_type i) 
            : type(t),
              src(s),
              dst(d),
              indx(i)
        {
            priority = static_cast<int>(t);
        }

        MailBoxResponse::MailBoxResponse(response_type t, 
                                       folder_info_type s, 
                                       Email e, 
                                       folder_indx_type i) 
            : type(t),
              src(s),
              data(e),
              indx(i)
        {
            priority = static_cast<int>(t);
        }

        ASMailBox::ASMailBox()
        {
            run();
        }

        ASMailBox::~ASMailBox() {

        }

        void ASMailBox::handle(const MailBoxRequest& request) {
            try {
                MailBoxResponse response(MailBoxResponse::NONE);

                switch(request.type) {
                case MailBoxRequest::INITIALIZE:
                    this->on_init();
                    response.type = MailBoxResponse::INITIALIZED;
                    break;
                case MailBoxRequest::SET_PASSWORD:
                    this->on_set_password(request.password);
                    break;
                case MailBoxRequest::LIST_FOLDERS:
                    this->on_list_folders();
                    break;
                case MailBoxRequest::CREATE_FOLDER:
                    this->on_create_folder(request.src);
                    response.type = MailBoxResponse::FOLDER_CREATED;
                    response.src = request.src;
                    break;
                case MailBoxRequest::DELETE_FOLDER:
                    this->on_delete_folder(request.src);
                    response.type = MailBoxResponse::FOLDER_DELETED;
                    response.src = request.src;
                    break;
                case MailBoxRequest::RENAME_FOLDER:
                    this->on_rename_folder(request.src, request.dst);
                    response.type = MailBoxResponse::FOLDER_RENAMED;
                    response.src = request.src;
                    response.dst = request.dst;
                    break;
                case MailBoxRequest::EXPUNGE_FOLDER:
                    this->on_expunge_folder(request.src);
                    response.type = MailBoxResponse::FOLDER_EXPUNGED;
                    response.src = request.src;
                    break;
                case MailBoxRequest::CHECK_RECENT:
                    this->on_check_recent(request.src);
                    break;
                case MailBoxRequest::LIST_MESSAGES:
                    this->on_list_messages(request.src, request.indx);
                    break;
                case MailBoxRequest::LOAD_MESSAGES:
                    this->on_load_messages(request.src, request.indx);
                    break;
                case MailBoxRequest::COPY_MESSAGES:
                    this->on_copy_messages(request.src, request.dst, request.indx);
                    break;
                case MailBoxRequest::APPEND_MESSAGE:
                    this->on_append_message(request.src, request.data);
                    break;
                case MailBoxRequest::SET_MESSAGE_FLAGS:
                    this->on_set_message_flags(request.src, request.indx, request.data);
                    break;
                case MailBoxRequest::UNSET_MESSAGE_FLAGS:
                    this->on_unset_message_flags(request.src, request.indx, request.data);
                    break;
                default:
                    throw exception("Unknown request type");
                }
                if(response.type != MailBoxResponse::NONE) {
                    push(response);
                }
            } catch(std::exception& e) {
                push(MailBoxResponse(MailBoxResponse::ERROR, e.what()));
            }
        }

        void ASMailBox::handle(const MailBoxResponse& r) {
            switch(r.type) {
            case MailBoxResponse::ERROR:
                error.emit(r.text);
                break;
            case MailBoxResponse::LOGIN_ERROR:
                login_error.emit();
                break;
            case MailBoxResponse::STATUS:
                status.emit(r.text);
                break;
            case MailBoxResponse::INITIALIZED:
                initialized.emit();
                break;
            case MailBoxResponse::PROGRESS:
                progress.emit(r.text,r.pct);
                break;
            case MailBoxResponse::FOLDERS_LOADED:
                folders_loaded.emit(m_folders.get());
                break;
            case MailBoxResponse::FOLDER_LISTED:
                folder_listed.emit(r.src);
                break;
            case MailBoxResponse::FOLDER_CREATED:
                folder_created.emit(r.src);
                break;
            case MailBoxResponse::FOLDER_DELETED:
                folder_deleted.emit(r.src);
                break;
            case MailBoxResponse::FOLDER_RENAMED:
                folder_renamed.emit(r.src,r.dst);
                break;
            case MailBoxResponse::FOLDER_EXPUNGED:
                folder_expunged.emit(r.src);
                break;
            case MailBoxResponse::MESSAGE_LISTED:
                message_listed.emit(r.src,r.indx.front(),r.data);
                break;
            case MailBoxResponse::MESSAGE_LOADED:
                message_loaded.emit(r.src,r.indx.front(),r.data);
                break;
            case MailBoxResponse::MESSAGES_COPIED:
                messages_copied.emit(r.src,r.dst,r.indx);
                break;
            case MailBoxResponse::MESSAGE_FLAGS_SET:
                message_flags_set.emit(r.src,r.indx,r.data);
                break;
            case MailBoxResponse::MESSAGE_FLAGS_UNSET:
                message_flags_unset.emit(r.src,r.indx,r.data);
                break;
            }
        }

        void ASMailBox::init() {
            this->clear();
            push(MailBoxRequest(MailBoxRequest::INITIALIZE));
        }

        void ASMailBox::reinit() {
            this->clear();
            try {
                this->reset();
            } catch(std::exception& e) {
                std::cerr << "ASMailBox::reinit: caught std::exception: " << e.what() << std::endl;
            } catch(...) {
                std::cerr << "ASMailBox::reinit: caught exception" << std::endl;
            }
            push(MailBoxRequest(MailBoxRequest::INITIALIZE));
        }

        void ASMailBox::set_password(std::string password) {
            push(MailBoxRequest(MailBoxRequest::SET_PASSWORD, password));
        }

        void ASMailBox::list_folders() {
            push(MailBoxRequest(MailBoxRequest::LIST_FOLDERS));
        }

        void ASMailBox::create_folder(folder_info_type folder) {
            push(MailBoxRequest(MailBoxRequest::CREATE_FOLDER, folder));
        }

        void ASMailBox::delete_folder(folder_info_type folder) {
            push(MailBoxRequest(MailBoxRequest::DELETE_FOLDER, folder));
        }

        void ASMailBox::rename_folder(folder_info_type src, folder_info_type dst) {
            push(MailBoxRequest(MailBoxRequest::RENAME_FOLDER, src, dst));
        }

        void ASMailBox::expunge_folder(folder_info_type folder) {
            push(MailBoxRequest(MailBoxRequest::EXPUNGE_FOLDER, folder));
        }

        void ASMailBox::check_recent(folder_info_type folder) {
            push(MailBoxRequest(MailBoxRequest::CHECK_RECENT, folder));
        }

        void ASMailBox::list_messages(folder_info_type folder) {
            push(MailBoxRequest(MailBoxRequest::LIST_MESSAGES, folder));
        }
        
        void ASMailBox::load_messages(folder_info_type folder, folder_indx_type indx) {
            push(MailBoxRequest(MailBoxRequest::LOAD_MESSAGES, folder, indx));
        }

        void ASMailBox::copy_messages(folder_info_type src, 
                                      folder_info_type dst, 
                                      folder_indx_type indx) {
            push(MailBoxRequest(MailBoxRequest::COPY_MESSAGES, src, dst, indx));
        }

        
        void ASMailBox::append_message(folder_info_type folder, Email email) {
            push(MailBoxRequest(MailBoxRequest::APPEND_MESSAGE, folder, email));
        }

        void ASMailBox::set_message_flags(folder_info_type folder, 
                                          folder_indx_type indx, 
                                          Email email) {
            push(MailBoxRequest(MailBoxRequest::SET_MESSAGE_FLAGS, folder, email, indx));
        }
        
        void ASMailBox::unset_message_flags(folder_info_type folder, 
                                            folder_indx_type indx, 
                                            Email email) {
            push(MailBoxRequest(MailBoxRequest::UNSET_MESSAGE_FLAGS, folder, email, indx));
        }


        void ASMailBox::clear() {
            sys::auto_lock<Glib::Mutex> lock(m_requests);
            while(!m_requests.ref().empty()) {
                m_requests.ref().pop();
            }
        }

        void ASMailBox::clear(MailBoxRequest::request_type type) {
            sys::auto_lock<Glib::Mutex> lock(m_requests);
            std::list<MailBoxRequest> list;

            while(!m_requests.ref().empty()) {
                MailBoxRequest req(m_requests.ref().top());

                if(req.type != type) {
                    list.push_back(req);
                }

                m_requests.ref().pop();
            }

            std::list<MailBoxRequest>::iterator i;
            for(i = list.begin(); i != list.end(); i++) {
                m_requests.ref().push(*i);
            }
   
        }

        void ASMailBox::clear(MailBoxResponse::response_type type) {
            sys::auto_lock<Glib::Mutex> lock(m_responses);
            std::list<MailBoxResponse> list;

            while(!m_responses.ref().empty()) {
                if(m_responses.ref().front().type != type) {
                    list.push_back(m_responses.ref().front());
                }

                m_responses.ref().pop();
            }

            std::list<MailBoxResponse>::iterator i;
            for(i = list.begin(); i != list.end(); i++) {
                m_responses.ref().push(*i);
            }
   
        }

    }
}

