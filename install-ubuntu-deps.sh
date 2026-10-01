#!/bin/sh
#
# Install jlib's build dependencies on Debian and Ubuntu.
#
# README already lists every dependency, with versions, and INSTALLATION
# already names pkg-config and autoconf-archive.  What it lists are the
# *upstream* names -- "json-c >= 0.9" -- and the missing step is knowing that
# Debian calls that libjson-c-dev.  This is that mapping, executable.
#
# Prompted by a fresh EC2 instance, where the first thing missing was
# pkg-config and the error for it was
# "configure.ac:188: error: possibly undefined macro: AC_CHECK_LIB" --
# naming a core macro, 180 lines from the real problem.  configure.ac now
# guards against that; this stops you reaching it.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

usage() {
    cat <<'USAGE'
usage: ./install-ubuntu-deps.sh [minimal|full] [-n|--dry-run]

  minimal   the toolchain and the dependencies configure refuses to run
            without.  Builds the library and most of it.  (default)

  full      everything optional as well, so every module and every app
            builds rather than being skipped.

  -n        print the apt-get command instead of running it.

Not installed by either mode, because they are services rather than
libraries and installing them changes what the machine does:

  live tests    dovecot-imapd dovecot-pop3d tinyproxy nginx-light
                (the *_live_test targets skip cleanly without them)

  .deb building debhelper dh-autoreconf fakeroot
USAGE
}

mode=minimal
dry=no

for arg in "$@"; do
    case "$arg" in
        minimal|full)  mode=$arg ;;
        -n|--dry-run)  dry=yes ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "install-ubuntu-deps.sh: unknown argument '$arg'" >&2
                       usage >&2
                       exit 2 ;;
    esac
done

# The toolchain.  autoconf-archive and pkg-config are the two that are easy
# to miss and hard to diagnose: configure.ac guards against both now, but
# only after autoreconf gets far enough to read the guard.
toolchain="build-essential g++ make autoconf automake libtool pkg-config autoconf-archive"

# configure stops dead without these -- they are PKG_CHECK_MODULES calls with
# no action-if-not-found.  libgpgme-dev brings libgpg-error-dev, which the
# build needs directly because crypt/crypt.hh calls gpg_strerror inline.
required="libjson-c-dev libgpgme-dev libssl-dev"

# Everything here degrades: configure reports "no" and the module or app that
# wanted it is skipped.
#
#   libsodium-dev      the elliptic curve code, and password hashing
#   libncurses-dev     jalpaca's terminal front end
#   portaudio19-dev    audio output; jlib/media builds without it but is mute
#   libglfw3-dev       the current windowing backend (jhardhyper, jgltorus)
#   libx11-dev
#   libxext-dev        the X11 backend, which draws without OpenGL
#   libgl-dev
#   libglu1-mesa-dev   OpenGL for the glx and glfw plots
#   libmagick++-dev    the jneural image classifiers
optional="libsodium-dev libncurses-dev portaudio19-dev libglfw3-dev \
libx11-dev libxext-dev libgl-dev libglu1-mesa-dev libmagick++-dev"

case "$mode" in
    minimal) packages="$toolchain $required" ;;
    full)    packages="$toolchain $required $optional" ;;
esac

# Deliberately unquoted: the list is meant to split into arguments.
# shellcheck disable=SC2086
set -- $packages

# Before the apt-get check, deliberately: printing the list is exactly what
# you want on a machine that cannot run it -- to read it, or to carry it to
# one that can.
if [ "$dry" = yes ]; then
    echo "apt-get install -y $*"
    exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "install-ubuntu-deps.sh: no apt-get here; this script is for Debian and Ubuntu." >&2
    echo "Run with -n to see the package list, or read README." >&2
    exit 1
fi

sudo=
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        sudo=sudo
    else
        echo "install-ubuntu-deps.sh: not root and no sudo; re-run as root." >&2
        exit 1
    fi
fi

echo "==> $mode: $# packages"

# update first, or install fails on a stale index -- which is the usual state
# of a freshly booted cloud image.
$sudo apt-get update
$sudo apt-get install -y "$@"

echo
echo "==> done.  Next:"
echo "      ./autogen.sh && ./configure && make -j\$(nproc)"
