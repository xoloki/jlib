AM_CPPFLAGS = -I$(top_srcdir) $(OPENSSL_CFLAGS)

lib_LTLIBRARIES = libjnet.la
libjnet_la_SOURCES = Email.cc MailBox.cc Imap4Box.cc Pop3.cc MBox.cc \
                     Imap4.cc Imap4Fetch.cc net.cc MFolder.cc Imap4Folder.cc \
                     MailFolder.cc ASMailBox.cc ASImapBox.cc ASMBox.cc \
                     address.cc imap_response.cc http.cc
libjnet_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjnet_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la \
                    $(top_builddir)/jlib/util/libjutil.la \
                    $(top_builddir)/jlib/crypt/libjcrypt.la \
                    $(OPENSSL_LIBS) $(PTHREAD_LIBS)

libjnetincludedir = $(includedir)/jlib-1.2/jlib/net
libjnetinclude_HEADERS = Imap4Box.hh Pop3.hh MBox.hh Email.hh \
                         Imap4.hh Imap4Fetch.hh net.hh MFolder.hh \
                         Imap4Folder.hh MailBox.hh MailFetch.hh MailFolder.hh \
                         ASMailBox.hh ASImapBox.hh ASMBox.hh address.hh rfc5322.hh \
                         imap_response.hh rfc3501.hh http.hh
