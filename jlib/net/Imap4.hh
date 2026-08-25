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

#ifndef JLIB_NET_IMAP4_HH
#define JLIB_NET_IMAP4_HH

#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>
#include <jlib/net/Email.hh>
#include <jlib/net/imap_response.hh>

#include <cstddef>
#include <vector>
#include <mutex>
#include <map>

namespace jlib {
    namespace net {
        
        class ListItem {
        public:
            ListItem();
            ListItem(const std::string& line);
            
            std::vector<std::string> get_attributes();
            std::string get_delim();
            std::string get_name();

            bool is_folder();
            bool is_parent();

        protected:
            std::vector<std::string> m_attr;
            std::string m_delim;
            std::string m_name;

            // Initialized here because ListItem() is an empty body: a
            // default-constructed item reported whatever was on the stack for
            // both of these, and one of them decides whether a mailbox is
            // shown at all.
            bool m_is_folder = false;
            bool m_is_parent = false;
        };

        /**
         * Class Imap4 is an implementation of the IMAP4 protocol, 
         * RFC 1730.
         */
        class Imap4 {//: public SigC::Object {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "imap4 exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


            /**
             * session state
             */
            typedef enum { UnConnected, NonAuthenticated, Authenticated, Selected } State;
            
            /**
             * Create Imap4 from the passed URL
             *
             */
            Imap4(jlib::util::URL url);
            
            /**
             * Destructor.
             */
            ~Imap4();
            
            /** Is the scheme one that means TLS from the first byte? */
            bool is_secure();

            /**
             * Was STARTTLS asked for?
             *
             *     imap://mail.example.com/INBOX?tls=starttls
             *
             * RFC 2595 3: connect in the clear on the ordinary port, then
             * upgrade in place.  A query parameter rather than a third scheme
             * because that is how this class already takes a connection
             * option -- see m_url["proxy"] -- and because there is no
             * registered scheme for it.
             *
             * If it is asked for and the server does not offer it, connect()
             * throws.  Carrying on in the clear is the bug this branch exists
             * to fix, wearing a different hat.
             */
            bool use_starttls();

            /** The server's capabilities, as of the last CAPABILITY. */
            const std::vector<std::string>& capabilities() const { return m_capabilities; }

            /**
             * CAPABILITY, and remember the answer.
             *
             * It used to be tokenize(handshake(...)[0]) with the first two
             * tokens erased, which indexes an empty vector when the server
             * says nothing and takes the capabilities out of whatever the
             * first response happened to be.
             */
            const std::vector<std::string>& capability(jlib::sys::socketstream& sock);

            /** Does the capability list hold this, compared whole? */
            bool has_capability(const std::string& name) const;

            /**
             * Negotiate STARTTLS on an already-connected plaintext stream.
             *
             * Called by connect() when ?tls=starttls was asked for.  Throws if
             * the server does not offer it, rather than carrying on in the
             * clear.
             */
            void upgrade(jlib::sys::socketstream& sock);

            jlib::sys::socketstream* connect();
            void disconnect(jlib::sys::socketstream& sock);

            // 6.1.    Client Commands - Any State
            /**
             * The CAPABILITY command requests a listing of capabilities that the
             * server supports.  Declared with the rest of the capability
             * handling above; this comment is where the RFC ordering put it.
             */

            /**
             * Since any command can return a status update as untagged data, the
             * NOOP command can be used as a periodic poll for new messages or
             * message status updates during a period of inactivity.
             *
             * @return status update information
             */
            std::vector<std::string> noop(jlib::sys::socketstream& sock);

            /**
             * The IDLE command is sent from the client to the server when the
             * client is ready to accept unsolicited mailbox update messages.  The
             * server requests a response to the IDLE command using the continuation
             * ("+") response.  The IDLE command remains active until the client
             * responds to the continuation, and as long as an IDLE command is
             * active, the server is now free to send untagged EXISTS, EXPUNGE, and
             * other messages at any time.
             *
             * @return status update information
             */
            std::vector<std::string> idle(jlib::sys::socketstream& sock);
            std::vector<std::string> idle_send(jlib::sys::socketstream& sock);
            std::vector<std::string> idle_done(jlib::sys::socketstream& sock);

            /**
             * The LOGOUT command informs the server that the client is done with
             * the session. 
             *
             * @throw imap4_exception if an exception occurs while doing i/o
             */
            void logout(jlib::sys::socketstream& sock);


            // 6.2.    Client Commands - Non-Authenticated State
            /**
             * The AUTHENTICATE command indicates an authentication mechanism,
             * such as described in [IMAP-AUTH], to the server.
             *
             * @param name authentication mechanism name
             */ 
            void authenticate(jlib::sys::socketstream& sock, const std::string& name);

            /**
             * The LOGIN command identifies the user to the server and carries
             * the plaintext password authenticating this user.
             *
             * @param user username
             * @param pass password
             *
             * @throw imap4_exception if an exception occurs while doing i/o
             */
            void login(jlib::sys::socketstream& sock, const std::string& user="", const std::string& pass="");
            

            // 6.3.    Client Commands - Authenticated State
            /**
             * The SELECT command selects a  mailbox  so  that  messages  in  the
             * mailbox  can  be  accessed.  Before returning an OK to the client,
             * the server MUST send the following untagged data to the client:
             * 
             * FLAGS       Defined flags in the mailbox
             * 
             * <n> EXISTS  The number of messages in the mailbox
             *
             * <n> RECENT  The number of messages added to the  mailbox  since
             *             the previous time this mailbox was read
             * 
             * OK [UIDVALIDITY <n>]
             *            The unique  identifier  validity  value.   See  the
             *            description of the UID command for more detail.
             *
             * This method will set the member variables m_exists, m_recent, and
             * m_unseen.
             *
             * @param path mailbox name
             *
             * @return vector containing server response
             */
            std::vector<std::string> select(jlib::sys::socketstream& sock, const std::string& path);

            /**
             * The EXAMINE command is identical to SELECT and returns the same
             * output; however, the selected mailbox is identified as read-only.
             * No changes to the permanent state of the mailbox, including
             * per-user state, are permitted.
             *
             * @param path mailbox name
             *
             * @return vector containing server response
             */
            std::vector<std::string> examine(jlib::sys::socketstream& sock, const std::string& path);

            /**
             * The CREATE command creates a mailbox with the given name.  An OK
             * response is returned only if a new mailbox with that name has been
             * created.  It is an error to attempt to create INBOX or a mailbox
             * with a name that refers to an extant mailbox.  Any error in
             * creation will return a tagged NO response.
             * 
             * @param path mailbox name
             */
            void create(jlib::sys::socketstream& sock, const std::string& path);

            /**
             * The DELETE command permanently removes the mailbox with the given
             * name.  A tagged OK response is returned only if the mailbox has
             * been deleted.  It is an error to attempt to delete INBOX or a
             * mailbox name that does not exist.  Any error in deletion will
             * return a tagged NO response.
             *
             * @param path mailbox name
             */
            void remove(jlib::sys::socketstream& sock, const std::string& path);
            
            /**
             * The RENAME command changes the name of a mailbox.  A tagged OK
             * response is returned only if the mailbox has been renamed.  It is
             * an error to attempt to rename from a mailbox name that does not
             * exist or to a mailbox name that already exists.  Any error in
             * renaming will return a tagged NO response.
             *
             * @param old_name old mailbox name
             * @param new_name new mailbox name
             */
            void rename(jlib::sys::socketstream& sock, const std::string& old_name, const std::string& new_name);
            
            /**
             * The SUBSCRIBE command adds the specified mailbox name to the
             * server's set of "active" or "subscribed" mailboxes as returned by
             * the LSUB command.  This command returns a tagged OK response only
             * if the subscription is successful.
             *
             * @param path mailbox
             */
            void subscribe(jlib::sys::socketstream& sock, const std::string& path);

            /**
             * The UNSUBSCRIBE command removes the specified mailbox name from
             * the server's set of "active" or "subscribed" mailboxes as returned
             * by the LSUB command.  This command returns a tagged OK response
             * only if the unsubscription is successful.
             * 
             * @param path mailbox
             */
            void unsubscribe(jlib::sys::socketstream& sock, const std::string& path);

            /**
             * The LIST command returns a subset of names from the complete set
             * of all names available to the user.  Zero or more untagged LIST
             * replies are returned, containing the name attributes, hierarchy
             * delimiter, and name; see the description of the LIST reply for
             * more detail.
             * 
             * An empty ("" string) reference name argument indicates that the
             * mailbox name is interpreted as by SELECT. The returned mailbox
             * names MUST match the supplied mailbox name pattern.  A non-empty
             * reference name argument is the name of a mailbox or a level of
             * mailbox hierarchy, and indicates a context in which the mailbox
             * name is interpreted in an implementation-defined manner.
             * 
             * The reference and mailbox name arguments are interpreted, in an
             * implementation-dependent fashion, into a canonical form that
             * represents an unambiguous left-to-right hierarchy.  The returned
             * mailbox names will be in the interpreted form.
             * 
             * Any part of the reference argument that is included in the
             * interpreted form SHOULD prefix the interpreted form.  It should
             * also be in the same form as the reference name argument.  This
             * rule permits the client to determine if the returned mailbox name
             * is in the context of the reference argument, or if something about
             * the mailbox argument overrode the reference argument.  Without
             * this rule, the client would have to have knowledge of the servers
             * naming semantics including what characters are "breakouts" that
             * override a naming context.
             * 
             * @param ref reference name
             * @param path mailbox name
             */
            std::vector<ListItem> list(jlib::sys::socketstream& sock, const std::string& ref, const std::string& path);

            /**
             * The LSUB command returns a subset of names from the set of names
             * that the user has declared as being "active" or "subscribed".
             * Zero or more untagged LSUB replies are returned.  The arguments to
             * LSUB are in the same form as those for LIST.
             * 
             * @param ref reference name
             * @param path mailbox name
             */
            std::vector<ListItem> lsub(jlib::sys::socketstream& sock, const std::string& ref, const std::string& path);

            /**
             * The APPEND command appends the literal argument as a new message
             * in the specified destination mailbox.  This argument is in the
             * format of an [RFC-822] message.  8-bit characters are permitted in
             * the message.  A server implementation that is unable to preserve
             * 8-bit data properly MUST be able to reversibly convert 8-bit
             * APPEND data to 7-bit using [MIME-1] encoding.
             *
             * @param path mailbox name
             * @param data message literal
             * @param flag optional flag parenthesized list
             * @param date optional date/time string
             */
            void append(jlib::sys::socketstream& sock, const std::string& path, const std::string& data, const std::string& flag="", const std::string& date="");


            //6.4.    Client Commands - Selected State
            /**
             * The CHECK command requests a checkpoint of the currently selected
             * mailbox.  A checkpoint refers to any implementation-dependent
             * housekeeping associated with the mailbox (e.g. resolving the
             * server's in-memory state of the mailbox with the state on its
             * disk) that is not normally executed as part of each command.  A
             * checkpoint may take a non-instantaneous amount of real time to
             * complete.  If a server implementation has no such housekeeping
             * considerations, CHECK is equivalent to NOOP.
             *
             */
            void check(jlib::sys::socketstream& sock);

            /**
             * The CLOSE command permanently removes from the currently selected
             * mailbox all messages that have the \Deleted flag set, and returns
             * to authenticated state from selected state.  No untagged EXPUNGE
             * responses are sent.
             * 
             */
            void close(jlib::sys::socketstream& sock);

            /**
             * The EXPUNGE command permanently removes from the currently
             * selected mailbox all messages that have the \Deleted flag set.
             * Before returning an OK to the client, an untagged EXPUNGE response
             * is sent for each message that is removed.
             * 
             */
            void expunge(jlib::sys::socketstream& sock);

            
            /**
             * The FETCH command retrieves data associated with a message in the
             * mailbox.  The data items to be fetched may be either a single atom
             * or a parenthesized list.  The currently defined data items that
             * can be fetched are:
             *
             * @param set message set
             * @param n vector of message data item names
             * 
             * @return vector containing sever response
             */
            std::vector<std::string> fetch(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::vector<std::string> n);
            
            
            /**
             * The STORE command alters data associated with a message in the
             * mailbox.  Normally, STORE will return the updated value of the
             * data with an untagged FETCH response.  A suffix of ".SILENT" in
             * the data item name prevents the untagged FETCH, and the server
             * should assume that the client has determined the updated value
             * itself or does not care about the updated value.
             * 
             * @param set message set
             * @param key message data item name
             * @param val value for message data item
             * 
             * @return vector containing sever response
             */
            std::vector<std::string> store(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, const std::string& key, std::vector<std::string> val);
            
            /**
             * The COPY command copies the specified message(s) to the specified
             * destination mailbox.  The flags and internal date of the
             * message(s) SHOULD be preserved in the copy.
             * 
             * @param set message set
             * @param box mailbox
             */
            void copy(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, const std::string& box);


            /**
             * Any command prefixed with an X is an experimental command.
             * Commands which are not part of this specification, or a standard
             * or standards-track revision of this specification, MUST use the X
             * prefix.
             */
            //void x(const std::string& cmd);
            
            /**
             * Get the the specified email with flags
             *
             * @param which which email we're retrieving (0-(n-1))
             * @throw imap4_exception if an exception occurs while doing i/o
             */
            Email get(jlib::sys::socketstream& sock, int which, bool only_headers=false);

            /**
             * Retrieve the text of specified email
             *
             * @param which which email we're retrieving
             * @throw imap4_exception if an exception occurs while doing i/o
             */
            std::string retrieve(int which, const std::string& mailbox="INBOX");
            std::string retrieve(jlib::sys::socketstream& sock, int which, const std::string& mailbox="INBOX");
            
            /**
             * Retrieve the text of specified email
             *
             * @param which which email we're retrieving
             * @throw std::exception if an exception occurs while doing i/o
             */
            std::string retrieve_headers(unsigned int which, const std::string& mailbox,unsigned int& size);
            std::string retrieve_headers(jlib::sys::socketstream& sock, unsigned int which, const std::string& mailbox,unsigned int& size);

            /**
             * Send a command and return the untagged responses it produced.
             *
             * What handshake() would be if it had existed after the grammar:
             * whole responses, parsed, with the tagged completion checked and
             * dropped.  Throws Imap4::exception on NO or BAD.
             */
            std::vector<imap::response> command(jlib::sys::socketstream& sock,
                                                const std::string& data);

            /**
             * SEARCH, RFC 3501 6.4.4.  The message numbers that matched.
             *
             * criteria is the search key -- "UNSEEN", "FROM joe", "SINCE
             * 1-Jan-2026" -- and charset names the encoding its strings are
             * in, if they are not ASCII.
             *
             * This used to send a bare tag with no command at all and return
             * whatever the server said about that, which is BAD.
             */
            std::vector<unsigned long> search(jlib::sys::socketstream& sock,
                                              const std::string& criteria,
                                              const std::string& charset = "");

            /**
             * UID, RFC 3501 6.4.8: the same commands keyed by unique id.
             *
             *     uid(sock, "SEARCH", "UNSEEN")
             *     uid(sock, "FETCH", "4827313 (RFC822.SIZE)")
             *
             * The one gtkmail most needs: a sequence number shifts under an
             * EXPUNGE from another client, so any state a client keeps across
             * a connection has to be keyed by UID.
             *
             * Also a stub before this: a bare tag and no command.
             */
            std::vector<imap::response> uid(jlib::sys::socketstream& sock,
                                            const std::string& command,
                                            const std::string& args);

            /**
             * A byte range of one part, RFC 3501 6.4.5.
             *
             * The replacement for PARTIAL, which was withdrawn in RFC 3501 in
             * favour of a FETCH with an origin and a length -- so the method
             * that used to be called partial() is gone and this is not it.
             *
             *     fetch_partial(sock, 0, "", 0, 1024)      the first KB
             *     fetch_partial(sock, 0, "HEADER", 0, 512)
             *
             * A server may return fewer octets than were asked for; it may
             * not return more.
             */
            std::string fetch_partial(jlib::sys::socketstream& sock,
                                      unsigned int which,
                                      const std::string& section,
                                      std::size_t origin,
                                      std::size_t length);

            /**
             * FETCH one message and return one of its attributes.
             *
             * items is what to ask for -- "FLAGS RFC822.SIZE RFC822.HEADER" --
             * and want is which of them to return.  RFC822.SIZE, if it was
             * asked for and the server sent it, goes into size.
             *
             * Reads whole responses, so an attribute that arrives as a literal
             * arrives whole; retrieve() and retrieve_headers() each used to
             * take the response apart with util::tokenize and a linear search
             * for the attribute name.
             */
            std::string fetch_attribute(jlib::sys::socketstream& sock,
                                        unsigned int which,
                                        const std::string& items,
                                        const std::string& want,
                                        unsigned int& size);
            
            /**
             * Remove this email from it's server
             *
             * @param which which email we're removing
             * @throw std::exception if an exception occurs while doing i/o
             */
            void remove(int which, const std::string& mailbox="INBOX");
            
            std::vector<std::string> handshake(jlib::sys::socketstream& sock, const std::string& data);
            
            bool unseen(jlib::sys::socketstream& sock,int i);

            /**
             * parse the handshake output for exists, recent
             *
             */
            void parse(std::vector<std::string> hand);

            void exists(unsigned int e) {
                m_exists_mutex.lock();
                m_exists = e;
                m_exists_mutex.unlock();
            }

            void recent(unsigned int e) {
                m_recent_mutex.lock();
                m_recent = e;
                m_recent_mutex.unlock();
            }

            void unseen(unsigned int e) {
                m_unseen_mutex.lock();
                m_unseen = e;
                m_unseen_mutex.unlock();
            }

            void num(unsigned int e) {
                m_num_mutex.lock();
                m_num = e;
                m_num_mutex.unlock();
                //m_num(e);
            }

            unsigned int exists() {
                m_exists_mutex.lock();
                unsigned int tmp = m_exists;
                m_exists_mutex.unlock();
                return tmp;
            }

            unsigned int recent() {
                m_recent_mutex.lock();
                unsigned int tmp = m_recent;
                m_recent_mutex.unlock();
                return tmp;
            }

            unsigned int unseen() {
                m_unseen_mutex.lock();
                unsigned int tmp = m_unseen;
                m_unseen_mutex.unlock();
                return tmp;
            }

            unsigned int num() {
                m_num_mutex.lock();
                unsigned int tmp = m_num;
                m_num_mutex.unlock();
                return tmp;
                //return m_num();
            }
            
        protected:
            std::string tag(int i=0);

            std::string m_user, m_pass, m_host, m_delim;
            unsigned int m_port;
            unsigned int m_exists, m_recent, m_unseen;
            std::mutex m_exists_mutex, m_recent_mutex, m_unseen_mutex;
            int m_num;
            std::vector<std::string> m_capabilities;
            std::mutex m_num_mutex;
            int m_width;
            State m_state;
            jlib::util::URL m_url;
            bool m_idle = false;
        };
        
    }
}
#endif //JLIB_NET_IMAP4_HH
