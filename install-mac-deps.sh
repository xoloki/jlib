#!/bin/sh
#
# Install jlib's build dependencies on macOS, with Homebrew.
#
# The companion to install-ubuntu-deps.sh, and deliberately not a translation
# of it: macOS differs in what it needs, not only in what things are called.
#
#   OpenGL and GLU      come from -framework OpenGL, so there is nothing to
#                       install and no pkg-config module to find.  configure
#                       special-cases Darwin for exactly this.
#   ncurses             Apple ships one with a .pc file, which is what
#                       configure finds.  Homebrew's is keg-only and would
#                       not be seen without help, so it is not wanted.
#   X11                 XQuartz, and a cask rather than a formula.  See below.
#   the compiler        Xcode Command Line Tools, not Homebrew.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

usage() {
    cat <<'USAGE'
usage: ./install-mac-deps.sh [minimal|full] [-n|--dry-run]

  minimal   the toolchain and the dependencies configure refuses to run
            without.  Builds the library and most of it.  (default)

  full      everything optional as well, so every module and every app
            builds rather than being skipped.

  -n        print the brew command instead of running it.

Not installed, and why:

  XQuartz       brew install --cask xquartz
                An X server, a GUI application, and a logout to take effect.
                Only jlib/x and jlib/glx want it; the GLFW backend does not,
                and that is the one the 4-D apps use.

  Metal         no package.  The GPU backend builds whenever the SDK is
                present, which it is with the Command Line Tools.

  live tests    the *_live_test targets want an IMAP server, a POP3 server,
                a proxy and a web server.  They skip cleanly without them.
USAGE
}

mode=minimal
dry=no

for arg in "$@"; do
    case "$arg" in
        minimal|full)  mode=$arg ;;
        -n|--dry-run)  dry=yes ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "install-mac-deps.sh: unknown argument '$arg'" >&2
                       usage >&2
                       exit 2 ;;
    esac
done

# autoconf pulls m4 in as a dependency, but it is listed because the reason
# is not obvious: Apple still ships GNU M4 1.4.6, from 2006, and autoconf
# wants newer.  pkgconf is the formula; "pkg-config" is an alias for it.
toolchain="autoconf automake libtool pkgconf autoconf-archive m4"

# configure stops dead without these.  libgpg-error is explicit even though
# gpgme depends on it, because crypt/crypt.hh calls gpg_strerror inline and
# every consumer links it directly.
required="json-c gpgme libgpg-error openssl@3"

#   libsodium    the elliptic curve code, and password hashing
#   portaudio    audio output; jlib/media builds without it but is mute
#   glfw         the current windowing backend, and the 4-D apps
#   imagemagick  the jneural image classifiers
optional="libsodium portaudio glfw imagemagick"

case "$mode" in
    minimal) packages="$toolchain $required" ;;
    full)    packages="$toolchain $required $optional" ;;
esac

# Deliberately unquoted: the list is meant to split into arguments.
# shellcheck disable=SC2086
set -- $packages

# Before the brew check, so the list is readable on a machine without it.
if [ "$dry" = yes ]; then
    echo "brew install $*"
    exit 0
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "install-mac-deps.sh: Homebrew not found.  See https://brew.sh" >&2
    echo "Run with -n to see the formula list." >&2
    exit 1
fi

# The compiler is not Homebrew's problem, and the failure if it is missing is
# much more confusing than this message.
if ! xcode-select -p >/dev/null 2>&1; then
    echo "install-mac-deps.sh: no Xcode Command Line Tools." >&2
    echo "Run: xcode-select --install" >&2
    exit 1
fi

echo "==> $mode: $# formulae"

brew install "$@"

echo
echo "==> done.  Next:"
echo "      ./autogen.sh && ./configure && make -j\$(sysctl -n hw.ncpu)"
