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

#ifndef JLIB_NET_IMAP4_HH
#define JLIB_NET_IMAP4_HH

#include <jlib/sys/socketstream.hh>
#include <glibmm/thread.h>

#include <jlib/util/URL.hh>
#include <jlib/net/Email.hh>

#include <vector>
#include <mutex>
#include <map>

namespace jlib {
    namespace net {
        
        class ListItem {
        public:
            ListItem();
            ListItem(std::string line);
            
            std::vector<std::string> get_attributes();
            std::string get_delim();
            std::string get_name();

            bool is_folder();
            bool is_parent();

        protected:
            std::vector<std::string> m_attr;
            std::string m_delim;
            std::string m_name;
            bool m_is_folder;
            bool m_is_parent;
        };

        /**
         * Class Imap4 is an implementation of the IMAP4 protocol, 
         * RFC 1730.
         */
        class Imap4 {//: public SigC::Object {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "imap4 exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
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
            
            bool is_secure();

            jlib::sys::socketstream* connect() throw(std::exception);
            void disconnect(jlib::sys::socketstream& sock) throw(std::exception);

            // 6.1.    Client Commands - Any State
            /**
             * The CAPABILITY command requests a listing of capabilities that the
             * server supports.
             *
             * @return vector containing the capabilities offered
             */
            std::vector<std::string> capability(jlib::sys::socketstream& sock);

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
            void logout(jlib::sys::socketstream& sock) throw(std::exception);


            // 6.2.    Client Commands - Non-Authenticated State
            /**
             * The AUTHENTICATE command indicates an authentication mechanism,
             * such as described in [IMAP-AUTH], to the server.
             *
             * @param name authentication mechanism name
             */ 
            void authenticate(jlib::sys::socketstream& sock, std::string name);

            /**
             * The LOGIN command identifies the user to the server and carries
             * the plaintext password authenticating this user.
             *
             * @param user username
             * @param pass password
             *
             * @throw imap4_exception if an exception occurs while doing i/o
             */
            void login(jlib::sys::socketstream& sock, std::string user="", std::string pass="") throw(std::exception);
            

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
            std::vector<std::string> select(jlib::sys::socketstream& sock, std::string path);

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
            std::vector<std::string> examine(jlib::sys::socketstream& sock, std::string path);

            /**
             * The CREATE command creates a mailbox with the given name.  An OK
             * response is returned only if a new mailbox with that name has been
             * created.  It is an error to attempt to create INBOX or a mailbox
             * with a name that refers to an extant mailbox.  Any error in
             * creation will return a tagged NO response.
             * 
             * @param path mailbox name
             */
            void create(jlib::sys::socketstream& sock, std::string path);

            /**
             * The DELETE command permanently removes the mailbox with the given
             * name.  A tagged OK response is returned only if the mailbox has
             * been deleted.  It is an error to attempt to delete INBOX or a
             * mailbox name that does not exist.  Any error in deletion will
             * return a tagged NO response.
             *
             * @param path mailbox name
             */
            void remove(jlib::sys::socketstream& sock, std::string path);
            
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
            void rename(jlib::sys::socketstream& sock, std::string old_name, std::string new_name);
            
            /**
             * The SUBSCRIBE command adds the specified mailbox name to the
             * server's set of "active" or "subscribed" mailboxes as returned by
             * the LSUB command.  This command returns a tagged OK response only
             * if the subscription is successful.
             *
             * @param path mailbox
             */
            void subscribe(jlib::sys::socketstream& sock, std::string path);

            /**
             * The UNSUBSCRIBE command removes the specified mailbox name from
             * the server's set of "active" or "subscribed" mailboxes as returned
             * by the LSUB command.  This command returns a tagged OK response
             * only if the unsubscription is successful.
             * 
             * @param path mailbox
             */
            void unsubscribe(jlib::sys::socketstream& sock, std::string path);

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
            std::vector<ListItem> list(jlib::sys::socketstream& sock, std::string ref, std::string path);

            /**
             * The LSUB command returns a subset of names from the set of names
             * that the user has declared as being "active" or "subscribed".
             * Zero or more untagged LSUB replies are returned.  The arguments to
             * LSUB are in the same form as those for LIST.
             * 
             * @param ref reference name
             * @param path mailbox name
             */
            std::vector<ListItem> lsub(jlib::sys::socketstream& sock, std::string ref, std::string path);

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
            void append(jlib::sys::socketstream& sock, std::string path, std::string data, std::string flag="", std::string date="");


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
             * The SEARCH command searches the mailbox for messages that match
             * the given searching criteria.  Searching criteria consist of one
             * or more search keys.  The untagged SEARCH response from the server
             * contains a listing of message sequence numbers corresponding to
             * those messages that match the searching criteria.
             * 
             * @param criteria searching criteria
             * @param spec optional character set specification
             *
             * @return vector with server response
             */
            std::vector<std::string> search(jlib::sys::socketstream& sock, std::string criteria, std::string spec="");
            
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
             * The PARTIAL command is equivalent to the associated FETCH command,
             * with the added functionality that only the specified number of
             * octets, beginning at the specified starting octet, are returned.
             * Only a single message can be fetched at a time.  The first octet
             * of a message, and hence the minimum for the starting octet, is
             * octet 1.
             * 
             * @param set message set
             * @param n vector of message data item names
             * @param p position of first octet
             * @param o number of octets
             * 
             * @return vector containing sever response
             */
            std::vector<std::string> partial(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::vector<std::string> n, 
                                   unsigned int p, unsigned int o);
            
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
            std::vector<std::string> store(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::string key, std::vector<std::string> val);
            
            /**
             * The COPY command copies the specified message(s) to the specified
             * destination mailbox.  The flags and internal date of the
             * message(s) SHOULD be preserved in the copy.
             * 
             * @param set message set
             * @param box mailbox
             */
            void copy(jlib::sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::string box);

            /**
             * The UID command has two forms.  In the first form, it takes as its
             * arguments a COPY, FETCH, or STORE command with arguments
             * appropriate for the associated command.  However, the numbers in
             * the message set argument are unique identifiers instead of message
             * sequence numbers.
             * 
             * In the second form, the UID command takes a SEARCH command with
             * SEARCH command arguments.  The interpretation of the arguments is
             * the same as with SEARCH; however, the numbers returned in a SEARCH
             * response for a UID SEARCH command are unique identifiers instead
             * of message sequence numbers.  For example, the command UID SEARCH
             * 1:100 UID 443:557 returns the unique identifiers corresponding to
             * the intersection of the message sequence number set 1:100 and the
             * UID set 443:557.
             * 
             * A unique identifier of a message is a number, and is guaranteed
             * not to refer to any other message in the mailbox.  Unique
             * identifiers are assigned in a strictly ascending fashion for each
             * message added to the mailbox.  Unlike message sequence numbers,
             * unique identifiers persist across sessions.  This permits a client
             * to resynchronize its state from a previous session with the server
             * (e.g.  disconnected or offline access clients); this is discussed
             * further in [IMAP-DISC].
             * 
             * Associated with every mailbox is a unique identifier validity
             * value, which is sent in an UIDVALIDITY response code in an OK
             * untagged response at message selection time.  If unique
             * identifiers from an earlier session fail to persist to this
             * session, the unique identifier validity value MUST be greater than
             * in the earlier session.
             * 
             * Note: An example of a good value to use for the unique
             * identifier validity value would be a 32-bit
             * representation of the creation date/time of the mailbox.
             * It is alright to use a constant such as 1, but only if
             * it guaranteed that unique identifers will never be
             * reused, even in the case of a mailbox being deleted and
             * a new mailbox by the same name created at some future
             * time.
             * 
             * 
             * Message set ranges are permitted; however, there is no guarantee
             * that unique identifiers be contiguous.  A non-existent unique
             * identifier within a message set range is ignored without any error
             * message generated.
             * 
             * The number after the "*" in an untagged FETCH response is always a
             * message sequence number, not a unique identifier, even for a UID
             * command response.  However, server implementations MUST implicitly
             * include the UID message data item as part of any FETCH response
             * caused by a UID command, regardless of whether UID was specified
             * as a message data item to the FETCH.
             * 
             * @param cmd command
             * @param arg args to cmd
             * 
             * @return result of cmd
             */
            std::vector<std::string> uid(jlib::sys::socketstream& sock, std::string cmd, std::vector<std::string> arg);

            /**
             * Any command prefixed with an X is an experimental command.
             * Commands which are not part of this specification, or a standard
             * or standards-track revision of this specification, MUST use the X
             * prefix.
             */
            //void x(std::string cmd);
            
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
            std::string retrieve(int which, std::string mailbox="INBOX") throw(std::exception);
            std::string retrieve(jlib::sys::socketstream& sock, int which, std::string mailbox="INBOX") throw(std::exception);
            
            /**
             * Retrieve the text of specified email
             *
             * @param which which email we're retrieving
             * @throw std::exception if an exception occurs while doing i/o
             */
            std::string retrieve_headers(unsigned int which, std::string mailbox,unsigned int& size) throw(std::exception);
            std::string retrieve_headers(jlib::sys::socketstream& sock, unsigned int which, std::string mailbox,unsigned int& size) throw(std::exception);
            
            /**
             * Remove this email from it's server
             *
             * @param which which email we're removing
             * @throw std::exception if an exception occurs while doing i/o
             */
            void remove(int which, std::string mailbox="INBOX") throw(std::exception);
            
            std::vector<std::string> handshake(jlib::sys::socketstream& sock, std::string data) throw(std::exception);
            
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
            std::mutex m_num_mutex;
            int m_width;
            State m_state;
            jlib::util::URL m_url;
            bool m_idle = false;
        };
        
    }
}
#endif //JLIB_NET_IMAP4_HH
