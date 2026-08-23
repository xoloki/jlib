# Process this file with autoconf to produce a configure script.-*- autoconf -*-
AC_PREREQ([2.69])
AC_INIT([jlib], [1.2.0], [xoloki@gmail.com])

AC_CONFIG_SRCDIR([jlib/sys/sys.hh])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])

AC_CANONICAL_HOST

JLIB_MAJOR_VERSION=1
JLIB_MINOR_VERSION=2
JLIB_MICRO_VERSION=0

AC_DEFINE_UNQUOTED([JLIB_MAJOR_VERSION], [$JLIB_MAJOR_VERSION], [jlib major version])
AC_DEFINE_UNQUOTED([JLIB_MINOR_VERSION], [$JLIB_MINOR_VERSION], [jlib minor version])
AC_DEFINE_UNQUOTED([JLIB_MICRO_VERSION], [$JLIB_MICRO_VERSION], [jlib micro version])

# +1 : ? : +1  == new interface that does not break old one
# +1 : ? : 0   == new interface that breaks old one
#  ? : ? : 0   == no new interfaces, but breaks apps
#  ? :+1 : ?   == just some internal changes, nothing breaks but might work
#                 better
# CURRENT : REVISION : AGE
libj_so_version="6:6:1"

jlib_version="$JLIB_MAJOR_VERSION.$JLIB_MINOR_VERSION.$JLIB_MICRO_VERSION"
jlib_release="$JLIB_MAJOR_VERSION.$JLIB_MINOR_VERSION"

AC_SUBST([JLIB_VERSION], [$jlib_version])
AC_SUBST([JLIB_RELEASE], [$jlib_release])
AC_SUBST([LIBJ_SO_VERSION], [$libj_so_version])

dnl ------------------------------------------------------------------
dnl Toolchain
dnl ------------------------------------------------------------------

AC_PROG_CXX
AM_PROG_AR
LT_INIT
AC_LANG([C++])

dnl Select strict C++20.  This is done directly rather than through
dnl AX_CXX_COMPILE_STDCXX because that macro's ext/noext argument handling
dnl differs between autoconf-archive releases (serial 23 drives the MSVC
dnl -std:c++NN spelling from its "ext" branch), and because an explicit probe
dnl keeps bootstrapping independent of autoconf-archive being installed.
dnl Strict c++20 is deliberate: it rejects GNU extensions, which is what we
dnl want from a portability cleanup targeting both clang and gcc.
AC_MSG_CHECKING([whether $CXX accepts -std=c++20])
jlib_save_CXX="$CXX"
CXX="$CXX -std=c++20"
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <version>
                     #include <string>]],
                   [[static_assert(__cplusplus >= 202002L);
                     std::string s = "c++20";]])],
  [AC_MSG_RESULT([yes])
   jlib_cxx_std="-std=c++20"],
  [CXX="$jlib_save_CXX"
   AC_MSG_RESULT([no])
   AC_MSG_ERROR([a C++20 compiler is required (clang 15+ or gcc 11+)])])

dnl Falling off the end of a value-returning function is undefined behaviour,
dnl not merely a diagnostic: at -O2 the compiler treats the path as
dnl unreachable and emits no epilogue.  do_hash() in jlib/crypt/curve.hh did
dnl exactly this and crashed inside _Unwind_Resume with no exception in
dnl flight.  A missing return is never intentional, so make it fatal.
AX_CHECK_COMPILE_FLAG([-Werror=return-type],
                      [CXXFLAGS="$CXXFLAGS -Werror=return-type"], [], [-Werror])

AX_PTHREAD([], [AC_MSG_ERROR([a working pthread implementation is required])])
LIBS="$PTHREAD_LIBS $LIBS"
CXXFLAGS="$CXXFLAGS $PTHREAD_CFLAGS"

dnl ------------------------------------------------------------------
dnl Host platform
dnl ------------------------------------------------------------------

case $host_os in
  darwin*) jlib_darwin=yes ;;
  *)       jlib_darwin=no  ;;
esac
AM_CONDITIONAL([DARWIN], [test "x$jlib_darwin" = xyes])

dnl ------------------------------------------------------------------
dnl System headers
dnl
dnl The C++ standard library headers this used to probe for are all
dnl mandated by C++20 and no longer worth checking.  Only the POSIX and
dnl platform headers that actually gate behaviour remain.
dnl ------------------------------------------------------------------

AC_CHECK_HEADERS([arpa/inet.h netinet/in.h netdb.h sys/socket.h sys/ioctl.h \
                  sys/types.h sys/time.h sys/stat.h unistd.h fcntl.h \
                  dirent.h paths.h pthread.h regex.h], [],
                 [AC_MSG_ERROR([required POSIX header $ac_header not found])])

AC_MSG_CHECKING([whether <paths.h> defines _PATH_MAILDIR])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <paths.h>]],
                   [[const char* SYS_MAIL_DIR = _PATH_MAILDIR;]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   AC_DEFINE_UNQUOTED([_PATH_MAILDIR], ["/var/spool/mail"],
                      [fill in missing system maildir path])])

dnl ------------------------------------------------------------------
dnl Required dependencies
dnl ------------------------------------------------------------------

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([JSONC],   [json-c >= 0.9])
PKG_CHECK_MODULES([GPGME],   [gpgme >= 1.0])
PKG_CHECK_MODULES([OPENSSL], [openssl >= 1.1])

dnl jlib/crypt/crypt.hh calls gpg_strerror/gpg_strsource inline, so every
dnl consumer links libgpg-error directly.  gpgme.pc only lists it under
dnl Requires.private, which pkg-config expands for static linking alone.
PKG_CHECK_MODULES([GPGERROR], [gpg-error])

dnl ------------------------------------------------------------------
dnl Optional dependencies
dnl ------------------------------------------------------------------

dnl libsodium, and specifically a build with the ristretto255 primitives,
dnl gates the elliptic curve code in jlib/crypt.
have_sodium=no
have_ristretto=no
PKG_CHECK_MODULES([SODIUM], [libsodium >= 1.0.18], [have_sodium=yes], [have_sodium=no])
AS_IF([test "x$have_sodium" = xyes],
  [jlib_save_CPPFLAGS="$CPPFLAGS"
   CPPFLAGS="$CPPFLAGS $SODIUM_CFLAGS"
   AC_CHECK_HEADER([sodium/crypto_core_ristretto255.h],
                   [have_ristretto=yes], [have_ristretto=no])
   CPPFLAGS="$jlib_save_CPPFLAGS"])
AS_IF([test "x$have_sodium" = xyes && test "x$have_ristretto" = xno],
      [AC_MSG_WARN([libsodium lacks ristretto255: not building elliptic curve code])])
AM_CONDITIONAL([HAVE_SODIUM],
               [test "x$have_sodium" = xyes && test "x$have_ristretto" = xyes])

dnl PortAudio replaces the OSS backend in jlib/media.
PKG_CHECK_MODULES([PORTAUDIO], [portaudio-2.0 >= 19],
                  [have_portaudio=yes], [have_portaudio=no])
AM_CONDITIONAL([HAVE_PORTAUDIO], [test "x$have_portaudio" = xyes])

dnl GLFW replaces GLUT as the portable windowing backend.
PKG_CHECK_MODULES([GLFW], [glfw3 >= 3.3], [have_glfw=yes], [have_glfw=no])
AM_CONDITIONAL([HAVE_GLFW], [test "x$have_glfw" = xyes])

dnl ImageMagick++ is only used by the jneural image classifiers.  IM7
dnl renamed the module, so try the new spelling before the old one.
PKG_CHECK_MODULES([MAGICK], [Magick++-7.Q16HDRI], [have_magick=yes],
  [PKG_CHECK_MODULES([MAGICK], [Magick++-6.Q16], [have_magick=yes],
    [PKG_CHECK_MODULES([MAGICK], [ImageMagick++ >= 6.0.0],
                       [have_magick=yes], [have_magick=no])])])

dnl ImageMagick installs libtool archives naming -lltdl in dependency_libs, so
dnl libtool puts it on the link line even though pkg-config never mentions it.
dnl Homebrew keeps libltdl in a keg it does not link into the default search
dnl path, and the link then fails with "library 'ltdl' not found" -- on the
dnl jneural apps only, which is enough to stop make and therefore make check.
dnl
dnl So: ask the linker, and only go looking if it says no.
AS_IF([test "x$have_magick" = xyes], [
  AC_MSG_CHECKING([whether the linker can find ltdl])
  jlib_save_LIBS="$LIBS"
  LIBS="-lltdl $LIBS"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])],
    [AC_MSG_RESULT([yes])
     LIBS="$jlib_save_LIBS"],
    [LIBS="$jlib_save_LIBS"
     jlib_ltdl_dir=
     for jlib_d in `brew --prefix libtool 2>/dev/null`/lib \
                    /opt/homebrew/opt/libtool/lib \
                    /usr/local/opt/libtool/lib \
                    /opt/local/lib; do
       AS_IF([test -f "$jlib_d/libltdl.dylib" -o -f "$jlib_d/libltdl.so"],
             [jlib_ltdl_dir="$jlib_d"; break])
     done
     AS_IF([test -n "$jlib_ltdl_dir"],
       [AC_MSG_RESULT([no, using $jlib_ltdl_dir])
        MAGICK_LIBS="$MAGICK_LIBS -L$jlib_ltdl_dir"],
       [AC_MSG_RESULT([no])
        AC_MSG_WARN([libltdl was not found, and ImageMagick asks for it: the jneural apps may fail to link])])
    ])
])

AM_CONDITIONAL([HAVE_MAGICK], [test "x$have_magick" = xyes])

dnl ------------------------------------------------------------------
dnl Windowing and graphics
dnl
dnl X11 and GLX are Linux-only here; macOS gets OpenGL through a
dnl framework and has no GLX at all.
dnl ------------------------------------------------------------------

have_x=no
have_gl=no
have_glu=no
have_glx=no

AS_IF([test "x$jlib_darwin" = xyes],
  [dnl Apple ships GL and GLU inside OpenGL.framework.  Both are deprecated
   dnl but present and functional; jlib.s rendering is fixed-function
   dnl 1.1/1.2, which needs the legacy 2.1 context they give.
   have_gl=yes
   have_glu=yes
   GL_CFLAGS=""
   GL_LIBS="-framework OpenGL"
   GLU_CFLAGS=""
   GLU_LIBS="-framework OpenGL"
   AC_SUBST([GL_CFLAGS])
   AC_SUBST([GL_LIBS])
   AC_SUBST([GLU_CFLAGS])
   AC_SUBST([GLU_LIBS])
],
  [PKG_CHECK_MODULES([X11],  [x11 xext], [have_x=yes],   [have_x=no])
   PKG_CHECK_MODULES([GL],   [gl],       [have_gl=yes],  [have_gl=no])
   PKG_CHECK_MODULES([GLU],  [glu],      [have_glu=yes], [have_glu=no])
   dnl GLX lives inside libGL and needs an X connection.
   AS_IF([test "x$have_gl" = xyes && test "x$have_x" = xyes],
         [have_glx=yes], [have_glx=no])])

AM_CONDITIONAL([HAVE_X],    [test "x$have_x"    = xyes])
AM_CONDITIONAL([HAVE_GL],   [test "x$have_gl"   = xyes])
AM_CONDITIONAL([HAVE_GLU],  [test "x$have_glu"  = xyes])
AM_CONDITIONAL([HAVE_GLX],  [test "x$have_glx"  = xyes])

dnl ------------------------------------------------------------------
dnl OSS audio (legacy; superseded by PortAudio)
dnl ------------------------------------------------------------------

AC_CHECK_HEADERS([sys/soundcard.h], [have_oss_audio=yes], [have_oss_audio=no])
AM_CONDITIONAL([HAVE_OSS_AUDIO], [test "x$have_oss_audio" = xyes])

dnl ------------------------------------------------------------------
dnl Linux joystick API, used by jlib/sys/joystick.hh
dnl ------------------------------------------------------------------

AC_CHECK_HEADERS([linux/joystick.h], [have_joystick=yes], [have_joystick=no])
AM_CONDITIONAL([HAVE_JOYSTICK], [test "x$have_joystick" = xyes])

dnl ------------------------------------------------------------------
dnl CUDA
dnl ------------------------------------------------------------------

AC_ARG_WITH([cuda],
  [AS_HELP_STRING([--with-cuda=/usr/local/cuda],
                  [Use cuda located at the given path, or --without-cuda to disable])],
  [cuda_path=$withval], [cuda_path=yes])

AS_IF([test "x$cuda_path" = xyes], [cuda_path='/usr/local/cuda'])

have_cuda=no
AS_IF([test "x$cuda_path" != xno],
  [jlib_save_CPPFLAGS="$CPPFLAGS"
   CPPFLAGS="$CPPFLAGS -I$cuda_path/include"
   AC_CHECK_HEADER([cuda_runtime.h], [have_cuda=yes], [have_cuda=no])
   CPPFLAGS="$jlib_save_CPPFLAGS"])

AS_IF([test "x$have_cuda" = xyes],
  [CUDA_CFLAGS="-I$cuda_path/include"
   CUDA_LIBS="-L$cuda_path/lib64 -lcublas -lcudart"],
  [CUDA_CFLAGS=""
   CUDA_LIBS=""])
AC_SUBST([CUDA_CFLAGS])
AC_SUBST([CUDA_LIBS])
AM_CONDITIONAL([HAVE_CUDA], [test "x$have_cuda" = xyes])

dnl ------------------------------------------------------------------
dnl Installed pkg-config file
dnl ------------------------------------------------------------------

dnl This must list only libraries that are actually built.  jlib/glfw does not
dnl exist yet (phase 4), and jlib/media is still gated on OSS (phase 3).
jlib_libs="-ljai -ljnet -ljcrypt -ljutil -ljsys"
AS_IF([test "x$have_oss_audio" = xyes], [jlib_libs="-ljmedia $jlib_libs"])
AS_IF([test "x$have_x"    = xyes], [jlib_libs="-ljx $jlib_libs"])
AS_IF([test "x$have_glx"  = xyes], [jlib_libs="-ljglx $jlib_libs"])
AS_IF([test "x$have_glfw" = xyes], [jlib_libs="-ljglfw $jlib_libs"])
AS_IF([test "x$have_glu"  = xyes], [jlib_libs="-ljglu $jlib_libs"])
AS_IF([test "x$have_gl"   = xyes], [jlib_libs="-ljgl $jlib_libs"])
AS_IF([test "x$have_cuda" = xyes], [jlib_libs="-ljcuda $jlib_libs"])

AC_SUBST([JLIB_PC_CFLAGS], ["-I\${includedir}/jlib-$jlib_release"])
AC_SUBST([JLIB_PC_LIBS],   ["-L\${libdir} $jlib_libs"])

AC_CONFIG_FILES([Makefile
    jlib/Makefile
    jlib/crypt/Makefile
    jlib/gl/Makefile
    jlib/glu/Makefile
    jlib/glx/Makefile
    jlib/glfw/Makefile
    jlib/math/Makefile
    jlib/media/Makefile
    jlib/net/Makefile
    jlib/sys/Makefile
    jlib/util/Makefile
    jlib/x/Makefile
    jlib/ai/Makefile
    jlib/cuda/Makefile
    jlib/apps/Makefile
    tests/Makefile
    jlib/jlib-1.2.pc
])

AC_OUTPUT

AC_MSG_NOTICE([

  jlib $jlib_version

    host ................. $host
    C++ standard ......... C++20 ($CXX)

  required
    json-c ............... yes
    gpgme ................ yes
    openssl .............. yes

  optional
    libsodium/ristretto .. $have_sodium/$have_ristretto
    portaudio ............ $have_portaudio
    glfw ................. $have_glfw
    ImageMagick++ ........ $have_magick
    cuda ................. $have_cuda

  windowing
    x11 .................. $have_x
    opengl ............... $have_gl
    glu .................. $have_glu
    glx .................. $have_glx

  platform
    linux joystick ....... $have_joystick

  legacy
    oss audio ............ $have_oss_audio
])
