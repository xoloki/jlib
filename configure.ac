# Process this file with autoconf to produce a configure script.-*- autoconf -*-
AC_INIT([jlib], [1.2.0], [xoloki@gmail.com])
AC_CONFIG_SRCDIR([config.h.in])

AM_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign])

JLIB_MAJOR_VERSION=1
JLIB_MINOR_VERSION=2
JLIB_MICRO_VERSION=0

AC_DEFINE_UNQUOTED(JLIB_MAJOR_VERSION, $JLIB_MAJOR_VERSION, jlib major version)
AC_DEFINE_UNQUOTED(JLIB_MINOR_VERSION, $JLIB_MINOR_VERSION, jlib minor version)
AC_DEFINE_UNQUOTED(JLIB_MICRO_VERSION, $JLIB_MICRO_VERSION, jlib micro version)

# +1 : ? : +1  == new interface that does not break old one
# +1 : ? : 0   == new interface that breaks old one
#  ? : ? : 0   == no new interfaces, but breaks apps
#  ? :+1 : ?   == just some internal changes, nothing breaks but might work 
#                 better
# CURRENT : REVISION : AGE
libj_so_version="6:6:1"

jlib_version="$JLIB_MAJOR_VERSION.$JLIB_MINOR_VERSION.$JLIB_MICRO_VERSION"
jlib_release="$JLIB_MAJOR_VERSION.$JLIB_MINOR_VERSION"


AC_SUBST(JLIB_VERSION, $jlib_version)
AC_SUBST(JLIB_RELEASE, $jlib_release)
AC_SUBST(LIBJ_SO_VERSION, $libj_so_version)

#AC_CANONICAL_TARGET
#AC_CANONICAL_SYSTEM

AC_PROG_LN_S
AM_PROG_LIBTOOL
AC_PROG_CXX

dnl Checks for programs.

dnl Checks for libraries.

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

dnl Checks for library functions.
#AC_CHECK_FUNCS(socket strstr)

#ac_cpp="$CXX -E"

dnl Checking for C++ features
AC_LANG_CPLUSPLUS

dnl Checks for header files.
AC_HEADER_STDC

AC_CHECK_HEADERS(string,,AC_MSG_ERROR(you need the c++ string header))

AC_CHECK_HEADERS(iomanip,,AC_MSG_ERROR(you need the c++ iomanip header))

AC_CHECK_HEADERS(iostream,,AC_MSG_ERROR(you need the c++iostream header))
AC_CHECK_HEADERS(fstream,,AC_MSG_ERROR(you need the c++ fstream header))
AC_CHECK_HEADERS(sstream,,AC_MSG_ERROR(you need the c++ sstream header))

AC_CHECK_HEADERS(vector,,AC_MSG_ERROR(you need the c++ vector header))
AC_CHECK_HEADERS(map,,AC_MSG_ERROR(you need the c++ map header))
AC_CHECK_HEADERS(set,,AC_MSG_ERROR(you need the c++ set header))
AC_CHECK_HEADERS(stack,,AC_MSG_ERROR(you need the c++ stack header))
AC_CHECK_HEADERS(list,,AC_MSG_ERROR(you need the c++ list header))

AC_CHECK_HEADERS(exception,,AC_MSG_ERROR(you need the c++ exception header))
AC_CHECK_HEADERS(algorithm,,AC_MSG_ERROR(you need the c++ algorithm header))
AC_CHECK_HEADERS(functional,,AC_MSG_ERROR(you need the c++ functional header))
AC_CHECK_HEADERS(memory,,AC_MSG_ERROR(you need the c++ memory header))

AC_CHECK_HEADERS(bits/char_traits.h,,AC_MSG_ERROR(you need the c++ bits/char_traits.h header))

#AC_CHECK_HEADERS(map,,AC_MSG_ERROR(you need the c++ map header))

AC_CHECK_HEADERS(cctype,,AC_MSG_ERROR(you need the c++ cctype header))
AC_CHECK_HEADERS(cstdio,,AC_MSG_ERROR(you need the c++ cstdio header))
AC_CHECK_HEADERS(cstdarg,,AC_MSG_ERROR(you need the c++ cstdarg header))
AC_CHECK_HEADERS(ctime,,AC_MSG_ERROR(you need the c++ ctime header))
AC_CHECK_HEADERS(cmath,,AC_MSG_ERROR(you need the c++ cmath header))

AC_CHECK_HEADERS(arpa/inet.h,,AC_MSG_ERROR(you need the arpa/inet.h header))
AC_CHECK_HEADERS(netinet/in.h,,AC_MSG_ERROR(you need the netinet/in.h header))
AC_CHECK_HEADERS(netdb.h,,AC_MSG_ERROR(you need the netdb.h header))

AC_CHECK_HEADERS(sys/types.h,,AC_MSG_ERROR(you need the sys/types.h header))
AC_CHECK_HEADERS(sys/time.h,,AC_MSG_ERROR(you need the sys/time.h header))
AC_CHECK_HEADERS(sys/stat.h,,AC_MSG_ERROR(you need the sys/stat.h header))
AC_CHECK_HEADERS(sys/socket.h,,AC_MSG_ERROR(you need the sys/socket.h header))
AC_CHECK_HEADERS(sys/ioctl.h,,AC_MSG_ERROR(you need the sys/ioctl.h header))

AC_CHECK_HEADERS(openssl/ssl.h,,AC_MSG_ERROR(you need the openssl/ssl.h header))

AC_CHECK_HEADERS(paths.h,,AC_MSG_ERROR(you need the paths.h header))
AC_CHECK_HEADERS(unistd.h,,AC_MSG_ERROR(you need the unistd.h header))
AC_CHECK_HEADERS(fcntl.h,,AC_MSG_ERROR(you need the fcntl.h header))
AC_CHECK_HEADERS(dirent.h,,AC_MSG_ERROR(you need the dirent.h header))
AC_CHECK_HEADERS(errno.h,,AC_MSG_ERROR(you need the errno.h header))
AC_CHECK_HEADERS(signal.h,,AC_MSG_ERROR(you need the signal.h header))
AC_CHECK_HEADERS(pthread.h,,AC_MSG_ERROR(you need the pthread.h header))
AC_CHECK_HEADERS(regex.h,,AC_MSG_ERROR(you need the regex.h header))

AC_CHECK_HEADERS(X11/Xlib.h,have_x=true,AC_WARN(unable to find Xlib header: not compiling jlib::x code))
AC_CHECK_HEADERS(X11/Xutil.h,,AC_WARN(unable to find Xlib util header: not compiling jlib::x code))

AC_CHECK_HEADERS(GL/gl.h,have_gl=true,AC_WARN(unable to find OpenGL header: not compiling gl code))
AC_CHECK_HEADERS(GL/glx.h,have_glx=true,AC_WARN(unable to find OpenGL X header: not compiling glx code))
AC_CHECK_HEADERS(GL/glu.h,have_glu=true,AC_WARN(unable to find OpenGL utility header: not compiling glu code))
AC_CHECK_HEADERS(GL/glut.h,have_glut=true,AC_WARN(unable to find OpenGL GL utility toolkit header: not compiling glut code))

AC_CHECK_HEADERS(openssl/ssl.h,have_ssl=true,AC_WARN(unable to find openssl, not compiling ssl code))

AC_CHECK_HEADERS(sys/soundcard.h,have_oss_audio=true,AC_WARN(unable to find oss audio, not compiling oss code))

AC_CHECK_HEADERS(sodium.h,,AC_MSG_ERROR(you need the sodium.h header))
AC_CHECK_HEADERS(sodium/crypto_core_ristretto255.h,,AC_MSG_ERROR(you need the sodium ristretto header))

#PKG_CHECK_MODULES(SIGC, sigc++-2.0 >= 2.0.16, CXXFLAGS="$CXXFLAGS $SIGC_CFLAGS";LIBS="$LIBS $SIGC_LIBS", AC_MSG_ERROR(cannot find sigc++-2.0))

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES(PKGS, [sigc++-2.0 >= 2.0.16 glibmm-2.4 >= 2.4.8 gthread-2.0 >= 2.0.0 ImageMagick++ >= 6.0.0 json-c >= 0.9], CXXFLAGS="$CXXFLAGS $PKGS_CFLAGS"; LIBS="$LIBS $PKGS_LIBS")

#AM_PATH_LIBGCRYPT([1.2.0], CXXFLAGS="$CXXFLAGS $LIBGCRYPT_CFLAGS";LIBS="$LIBS $LIBGCRYPT_LIBS", AC_MSG_ERROR(you need libgcrypt for crypto))
AM_PATH_GPGME([0.3.15], CXXFLAGS="$CXXFLAGS $GPGME_CFLAGS";LIBS="$LIBS $GPGME_LIBS", AC_MSG_ERROR(you need gpgme for crypto))

AC_ARG_WITH([cuda], AS_HELP_STRING([--with-cuda=/usr/local/cuda], [Use cuda located at the given path, or --without-cuda to disable]), cuda_path=$withval)

AS_IF([test "$cuda_path" = "yes" -o "$cuda_path" = ""], [cuda_path='/usr/local/cuda'])
AS_IF([test "$cuda_path" != "no"], AC_CHECK_HEADER([cuda_runtime.h], [have_cuda=true], [have_cuda=false]), [have_cuda=false])
AS_IF([test "$have_cuda" = 'true'], [LIBS="$LIBS -L"$cuda_path/lib64" -lcublas"])

AM_CONDITIONAL(HAVE_CUDA, test x$have_cuda = xtrue)
AM_CONDITIONAL(HAVE_X, test x$have_x = xtrue)
AM_CONDITIONAL(HAVE_GL, test x$have_gl = xtrue)
AM_CONDITIONAL(HAVE_GLX, test x$have_glx = xtrue)
AM_CONDITIONAL(HAVE_GLU, test x$have_glu = xtrue)
AM_CONDITIONAL(HAVE_GLUT, test x$have_glut = xtrue)
AM_CONDITIONAL(HAVE_SSL, test x$have_ssl = xtrue)
AM_CONDITIONAL(HAVE_OSS_AUDIO, test x$have_oss_audio = xtrue)

LIBS="$LIBS -lssl -lcrypto -lsodium"

if test "x$prefix" = "xNONE"; then
    myprefix="/usr/local"
else
    myprefix="$prefix"
fi

myexec_prefix=$myprefix
myincludedir="$myprefix/include";
mylibdir="$myexec_prefix/lib";

jlib_cflags="-I$myincludedir/jlib-1.2 $SIGC_CFLAGS $GLIBMM_CFLAGS"
jlib_libs="-ljcrypt -ljnet -ljsys -ljutil"

if test x$have_oss_audio = xtrue; then 
   jlib_libs="-ljmedia $jlib_libs"
fi

if test x$have_x = xtrue; then
    jlib_libs="-ljx $jlib_libs"
fi

if test x$have_gl = xtrue && test x$have_glu = xtrue && test x$have_glut = xtrue; then
    jlib_libs="-ljgl $jlib_libs"
fi

AS_IF([test "$have_cuda" = 'true'],[jlib_libs="-ljcuda $jlib_libs"])

jlib_libs="-L$mylibdir $jlib_libs $SIGC_LIBS $GLIBMM_LIBS"

AC_DEFINE_UNQUOTED(JLIB_CFLAGS,"$jlib_cflags",[jlib cflags])
AC_DEFINE_UNQUOTED(JLIB_LIBS,"$jlib_libs",[jlib libs])

AC_SUBST(JLIB_CFLAGS, $jlib_cflags)
AC_SUBST(JLIB_LIBS, $jlib_libs)

dnl Checking for _PATH_MAILDIR
AC_MSG_CHECKING(if <paths.h> defines _PATH_MAILDIR)
AC_TRY_COMPILE(
[
#include <paths.h>
],[
const char* SYS_MAIL_DIR = _PATH_MAILDIR;
],[
    ac_paths_path_maildir=yes
],[
    ac_paths_path_maildir=no
    AC_DEFINE_UNQUOTED(_PATH_MAILDIR,"/var/spool/mail",[fill in missing system maildir path])
])
AC_MSG_RESULT([$ac_paths_path_maildir])


dnl Checking for C++ features
AC_MSG_CHECKING(if C++ compiler supports bool (required))
AC_TRY_COMPILE(
[
],[
    bool b=true;
    bool b1=false;
],[
    ac_bool=yes
],[
    config_error=yes
    AC_WARN(bool type is not supported by your compiler)
])
AC_MSG_RESULT([$ac_bool])

AC_MSG_CHECKING(if C++ compiler supports std::string (required))
AC_TRY_COMPILE(
[
#include <string>
],[
    std::string foo = "foo";
],[
    ac_std_string=yes
],[
    config_error=yes
    AC_WARN(std::string type is not supported by your compiler)
])
AC_MSG_RESULT([$ac_std_string])

AC_MSG_CHECKING(if C++ compiler supports std::stringstream (required))
AC_TRY_RUN(
[
#include <sstream>
#include <iostream>

int main() {
    std::istringstream i("abcdef");
    std::ostringstream o;
    o << "abcdef";
    
    if(o.str() != "abcdef")
        return 1;

    o.seekp(0, std::ios_base::beg);
    o << "ghi";

    o.seekp(0, std::ios_base::end);
    o << "jkl";


    if(o.str() != "ghidefjkl")
        return 1;

    char c;
    i >> c;

    if(c != 'a')
        return 1;

    i.seekg(3,std::ios_base::beg);
    i >> c;

    if(c != 'd')
        return 1;

    return 0;
}

],[
    ac_std_stringstream=yes
],[
    config_error=yes
    AC_WARN(std::stringstream type is not supported by your compiler)
])
AC_MSG_RESULT([$ac_std_stringstream])

AC_MSG_CHECKING(if C++ compiler supports specialized member template functions (required))
AC_TRY_RUN(
[
#include <string>
#include <sstream>

class foo {
public:
  template <class T> T get(std::string s);
};
template <class T> 
inline
T foo::get(std::string s) {
    std::istringstream i(s);
    T ret;
    i >> ret;
    return ret;
}

template <> 
inline
std::string foo::get<std::string>(std::string s) {
    return "foo";
}

int main() {
    foo f;
    int i = f.get<int>("1");
    std::string s = f.get<std::string>("s");
    if(i == 1 && s == "foo") return 0;
    else return 1;
}

],[
    ac_member_templates=yes
],[
    ac_member_templates=no
    config_error=yes
])
AC_MSG_RESULT([$ac_member_templates])

dnl
dnl check if the previous C++ tests went OK
dnl
AC_MSG_CHECKING(if C++ environment provides all required features)
if test "x$config_error" = xyes ; then
  AC_MSG_RESULT([no])
  AC_WARN(********************************************)
  AC_WARN(*** You need gcc >= 3.0 from gcc.gnu.org ***)
  AC_WARN(********************************************)
  AC_MSG_ERROR(Config error - exiting ...)
fi
AC_MSG_RESULT([yes])

AC_CONFIG_FILES([Makefile \
    jlib/Makefile \
    jlib/crypt/Makefile \
    jlib/gl/Makefile \
    jlib/glu/Makefile \
    jlib/glut/Makefile \
    jlib/glx/Makefile \
    jlib/math/Makefile \
    jlib/media/Makefile \
    jlib/net/Makefile \
    jlib/sys/Makefile \
    jlib/util/Makefile \
    jlib/x/Makefile \
    jlib/ai/Makefile \
    jlib/cuda/Makefile \
    jlib/apps/Makefile \
    tests/Makefile \
    jlib/jlib-1.2.pc
])

AC_OUTPUT
