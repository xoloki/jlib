# Public jlib headers include <gpgme.h>, <sodium.h>, <openssl/*.h> and
# <json.h> directly, so anything compiling against them needs those -I flags.
AM_CPPFLAGS = -I$(top_srcdir) \
	$(GPGME_CFLAGS) $(GPGERROR_CFLAGS) $(SODIUM_CFLAGS) $(OPENSSL_CFLAGS) $(JSONC_CFLAGS) $(PORTAUDIO_CFLAGS)

EXTRA_DIST = audio_test.hh certificate.hh mailserver.hh

JSYS_LA   = $(top_builddir)/jlib/sys/libjsys.la
JUTIL_LA  = $(top_builddir)/jlib/util/libjutil.la
JCRYPT_LA = $(top_builddir)/jlib/crypt/libjcrypt.la
JNET_LA   = $(top_builddir)/jlib/net/libjnet.la
JMEDIA_LA = $(top_builddir)/jlib/media/libjmedia.la
JX_LA     = $(top_builddir)/jlib/x/libjx.la

# These play real audio, so they skip themselves when the host has no output
# device rather than failing on a headless machine.
if HAVE_PORTAUDIO
MEDIA_TESTS = media_datastream_test \
              media_notestream_test \
              media_player_test \
              media_wavfile_test \
              media_wavstream_test \
              sys_ringbuffer_test \
              math_lookat_test \
              sys_sigpipe_test \
              sys_tls_sigpipe_test \
              media_voice_test \
              media_source_test \
              media_wavetable_test \
              media_mixer_test \
              media_sampler_test \
              media_playlist_test \
              media_live_test
endif

if HAVE_SODIUM
CURVE_TESTS = crypt_curve_test crypt_schnorr_test crypt_groth_test
endif

# x_window_test needs a live display, so it is gated rather than being an
# unconditional member of TESTS as it used to be.
if HAVE_X
X_TESTS = x_window_test
endif

TESTS = \
	math_test \
	math_object_test \
	tensor_test \
\
	net_address_test \
	net_email_test \
	net_email_received_test \
	net_email_multipart_test \
	net_mbox_crlf_test \
	net_protocol_test \
	net_imap_test \
	net_imap_live_test \
	sys_proxy_live_test \
	net_pop3_live_test \
	net_mime_test \
	net_same_address_test \
 \
	sys_sync_test \
	sys_run_test \
	sys_socket_test \
 \
	util_test \
	util_codec_test \
	util_url_test \
	util_date_test \
	util_rfc2047_test \
	util_headers_test \
	util_headers_manual_test \
	util_headers_fold_test \
	util_headers_angie_test \
	util_xml_test \
	util_abnf_test \
	util_abnf_compile_test \
 \
	$(X_TESTS) \
	$(MEDIA_TESTS) \
	$(CURVE_TESTS)

check_PROGRAMS = $(TESTS)

media_datastream_test_SOURCES = media_datastream_test.cc
media_datastream_test_LDADD   = $(JMEDIA_LA) $(JSYS_LA)
media_notestream_test_SOURCES = media_notestream_test.cc
media_notestream_test_LDADD   = $(JMEDIA_LA) $(JSYS_LA)
media_player_test_SOURCES     = media_player_test.cc
media_player_test_LDADD       = $(JMEDIA_LA) $(JSYS_LA)
media_wavfile_test_SOURCES    = media_wavfile_test.cc
media_wavfile_test_LDADD      = $(JMEDIA_LA) $(JSYS_LA)

media_wavstream_test_SOURCES  = media_wavstream_test.cc
media_wavstream_test_LDADD    = $(JMEDIA_LA) $(JSYS_LA)

sys_ringbuffer_test_SOURCES   = sys_ringbuffer_test.cc
sys_ringbuffer_test_LDADD     = $(JSYS_LA)

math_lookat_test_SOURCES      = math_lookat_test.cc
math_lookat_test_LDADD        = $(JSYS_LA)

sys_sigpipe_test_SOURCES      = sys_sigpipe_test.cc
sys_sigpipe_test_LDADD        = $(JSYS_LA)

sys_tls_sigpipe_test_SOURCES  = sys_tls_sigpipe_test.cc
sys_tls_sigpipe_test_LDADD    = $(JSYS_LA) $(OPENSSL_LIBS)

media_voice_test_SOURCES      = media_voice_test.cc
media_voice_test_LDADD        = $(JMEDIA_LA) $(JSYS_LA)

media_source_test_SOURCES     = media_source_test.cc
media_source_test_LDADD       = $(JMEDIA_LA) $(JSYS_LA)

media_wavetable_test_SOURCES  = media_wavetable_test.cc
media_wavetable_test_LDADD    = $(JMEDIA_LA) $(JSYS_LA)

media_mixer_test_SOURCES      = media_mixer_test.cc
media_mixer_test_LDADD        = $(JMEDIA_LA) $(JSYS_LA)

media_sampler_test_SOURCES    = media_sampler_test.cc
media_sampler_test_LDADD      = $(JMEDIA_LA) $(JUTIL_LA) $(JSYS_LA)

media_playlist_test_SOURCES   = media_playlist_test.cc
media_playlist_test_LDADD     = $(JMEDIA_LA) $(JUTIL_LA) $(JSYS_LA)

media_live_test_SOURCES       = media_live_test.cc
media_live_test_LDADD         = $(JMEDIA_LA) $(JUTIL_LA) $(JSYS_LA)

net_address_test_SOURCES        = net_address_test.cc
net_address_test_LDADD          = $(JNET_LA) $(JUTIL_LA)

net_email_test_SOURCES          = net_email_test.cc
net_email_test_LDADD            = $(JNET_LA)
net_email_received_test_SOURCES = net_email_received_test.cc
net_email_received_test_LDADD   = $(JNET_LA)
net_mime_test_SOURCES           = net_mime_test.cc
net_mime_test_LDADD             = $(JNET_LA) $(JUTIL_LA)

net_pop3_live_test_SOURCES      = net_pop3_live_test.cc
net_pop3_live_test_LDADD        = $(JNET_LA) $(JUTIL_LA) $(JSYS_LA) $(OPENSSL_LIBS)

sys_proxy_live_test_SOURCES     = sys_proxy_live_test.cc
sys_proxy_live_test_LDADD       = $(JSYS_LA) $(JUTIL_LA) $(OPENSSL_LIBS)

net_imap_live_test_SOURCES      = net_imap_live_test.cc
net_imap_live_test_LDADD        = $(JNET_LA) $(JUTIL_LA) $(JSYS_LA) $(OPENSSL_LIBS)

net_imap_test_SOURCES           = net_imap_test.cc
net_imap_test_LDADD             = $(JNET_LA) $(JUTIL_LA)

net_protocol_test_SOURCES       = net_protocol_test.cc
net_protocol_test_LDADD         = $(JNET_LA) $(JUTIL_LA)

net_mbox_crlf_test_SOURCES      = net_mbox_crlf_test.cc
net_mbox_crlf_test_LDADD        = $(JNET_LA)

net_same_address_test_SOURCES   = net_same_address_test.cc
net_same_address_test_LDADD     = $(JNET_LA)

net_email_multipart_test_SOURCES = net_email_multipart_test.cc
net_email_multipart_test_LDADD   = $(JNET_LA)

sys_run_test_SOURCES            = sys_run_test.cc
sys_run_test_LDADD              = $(JSYS_LA) $(JUTIL_LA)

sys_sync_test_SOURCES = sys_sync_test.cc
sys_sync_test_LDADD   = $(JSYS_LA)

sys_socket_test_SOURCES = sys_socket_test.cc
sys_socket_test_LDADD   = $(JSYS_LA)

util_test_SOURCES = util_test.cc
util_test_LDADD   = $(JUTIL_LA)
util_rfc2047_test_SOURCES       = util_rfc2047_test.cc
util_rfc2047_test_LDADD         = $(JUTIL_LA)

util_date_test_SOURCES          = util_date_test.cc
util_date_test_LDADD            = $(JUTIL_LA)

util_url_test_SOURCES           = util_url_test.cc
util_url_test_LDADD             = $(JUTIL_LA)

util_codec_test_SOURCES         = util_codec_test.cc
util_codec_test_LDADD           = $(JUTIL_LA)

util_headers_test_SOURCES = util_headers_test.cc
util_headers_test_LDADD   = $(JUTIL_LA)
util_headers_manual_test_SOURCES = util_headers_manual_test.cc
util_headers_manual_test_LDADD   = $(JUTIL_LA)
util_headers_fold_test_SOURCES = util_headers_fold_test.cc
util_headers_fold_test_LDADD   = $(JUTIL_LA)
util_headers_angie_test_SOURCES = util_headers_angie_test.cc
util_headers_angie_test_LDADD   = $(JUTIL_LA)
util_abnf_test_SOURCES = util_abnf_test.cc
util_abnf_test_LDADD   = $(JUTIL_LA)

util_abnf_compile_test_SOURCES = util_abnf_compile_test.cc
util_abnf_compile_test_LDADD   = $(JUTIL_LA)

util_xml_test_SOURCES = util_xml_test.cc
util_xml_test_LDADD   = $(JUTIL_LA)

x_window_test_SOURCES = x_window_test.cc
x_window_test_LDADD   = $(JX_LA)

math_test_SOURCES = math_test.cc
math_test_LDADD   = $(JUTIL_LA)

math_object_test_SOURCES = math_object_test.cc
math_object_test_LDADD   = $(JUTIL_LA)

tensor_test_SOURCES = tensor_test.cc
tensor_test_LDADD   = $(JUTIL_LA)

crypt_curve_test_SOURCES = crypt_curve_test.cc
crypt_curve_test_LDADD   = $(JUTIL_LA) $(JCRYPT_LA) $(SODIUM_LIBS)

crypt_schnorr_test_SOURCES = crypt_schnorr_test.cc
crypt_schnorr_test_LDADD   = $(JUTIL_LA) $(JCRYPT_LA) $(SODIUM_LIBS)

crypt_groth_test_SOURCES = crypt_groth_test.cc
crypt_groth_test_LDADD   = $(JUTIL_LA) $(JCRYPT_LA) $(SODIUM_LIBS)
