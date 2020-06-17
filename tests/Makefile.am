INCLUDES = -I$(top_srcdir)

noinst_PROGRAMS = $(TESTS) $(MEDIA_TESTS)

if HAVE_OSS_AUDIO
MEDIA_TESTS = 	media_datastream_test \
		media_notestream_test \
		media_player_test \
		media_wavfile_test
else
MEDIA_TESTS = 
endif

TESTS = \
	math_test \
	tensor_test \
    \
	x_window_test \
 \
	net_extract_address_simple_test  \
	net_extract_address_mangled_test  \
	net_extract_address_test  \
	net_email_test  \
	net_email_received_test  \
	net_email_multipart_test  \
 \
	sys_sync_test  \
 \
	util_test  \
	util_headers_test  \
	util_headers_manual_test \
	util_headers_fold_test \
	util_headers_angie_test \
	util_xml_test \
 \
	crypt_groth_test

#gl_glutmain_test_SOURCES = gl_glutmain_test.cc
#gl_glutmain_single_test_SOURCES = gl_glutmain_single_test.cc

media_datastream_test_SOURCES = media_datastream_test.cc
media_datastream_test_LDADD =  $(top_builddir)/jlib/media/libjmedia.la
media_notestream_test_SOURCES = media_notestream_test.cc
media_notestream_test_LDADD =  $(top_builddir)/jlib/media/libjmedia.la
media_player_test_SOURCES = media_player_test.cc
media_player_test_LDADD =  $(top_builddir)/jlib/media/libjmedia.la
media_wavfile_test_SOURCES = media_wavfile_test.cc
media_wavfile_test_LDADD =  $(top_builddir)/jlib/media/libjmedia.la

net_extract_address_test_SOURCES = net_extract_address_test.cc
net_extract_address_test_LDADD = $(top_builddir)/jlib/net/libjnet.la
net_extract_address_simple_test_SOURCES = net_extract_address_simple_test.cc
net_extract_address_simple_test_LDADD = $(top_builddir)/jlib/net/libjnet.la
net_extract_address_mangled_test_SOURCES = net_extract_address_mangled_test.cc
net_extract_address_mangled_test_LDADD = $(top_builddir)/jlib/net/libjnet.la
net_email_test_SOURCES = net_email_test.cc
net_email_test_LDADD = $(top_builddir)/jlib/net/libjnet.la
net_email_received_test_SOURCES = net_email_received_test.cc
net_email_received_test_LDADD = $(top_builddir)/jlib/net/libjnet.la
net_email_multipart_test_SOURCES = net_email_multipart_test.cc
net_email_multipart_test_LDADD = $(top_builddir)/jlib/net/libjnet.la

sys_sync_test_SOURCES = sys_sync_test.cc

util_test_SOURCES = util_test.cc
util_test_LDADD = $(top_builddir)/jlib/util/libjutil.la
util_headers_test_SOURCES = util_headers_test.cc
util_headers_test_LDADD = $(top_builddir)/jlib/util/libjutil.la
util_headers_manual_test_SOURCES = util_headers_manual_test.cc
util_headers_manual_test_LDADD = $(top_builddir)/jlib/util/libjutil.la
util_headers_fold_test_SOURCES = util_headers_fold_test.cc
util_headers_fold_test_LDADD = $(top_builddir)/jlib/util/libjutil.la
util_headers_angie_test_SOURCES = util_headers_angie_test.cc
util_headers_angie_test_LDADD = $(top_builddir)/jlib/util/libjutil.la
util_xml_test_SOURCES = util_xml_test.cc
util_xml_test_LDADD = $(top_builddir)/jlib/util/libjutil.la

x_window_test_SOURCES = x_window_test.cc
x_window_test_LDADD = $(top_builddir)/jlib/x/libjx.la

math_test_SOURCES = math_test.cc
math_test_LDADD = $(top_builddir)/jlib/util/libjutil.la

tensor_test_SOURCES = tensor_test.cc
tensor_test_LDADD = $(top_builddir)/jlib/util/libjutil.la

crypt_groth_test_SOURCES = crypt_groth_test.cc
crypt_groth_test_LDADD = $(top_builddir)/jlib/util/libjutil.la $(top_builddir)/jlib/crypt/libjcrypt.la
