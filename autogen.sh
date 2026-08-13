#!/bin/sh
# Bootstrap the autotools build from a fresh checkout.
#
#   ./autogen.sh && mkdir build && cd build && ../configure && make
#
# Requires autoconf, automake, libtool, pkg-config and autoconf-archive
# (for AX_CXX_COMPILE_STDCXX and AX_PTHREAD).

set -e

srcdir=$(dirname "$0")
cd "$srcdir"

autoreconf --install --force --warnings=all "$@"

echo "bootstrap complete; now run ./configure"
