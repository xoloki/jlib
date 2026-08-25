# Build jlib on a current Linux toolchain.
#
# This replaces the old per-compiler Dockerfiles (gcc 4.9 through 10), which
# pinned toolchains too old for C++20 and installed dependencies jlib no
# longer uses (libsigc++, glibmm).
#
#   docker build -t jlib-build .
#   docker run --rm -ti jlib-build
#
# The source is copied into the image and built during "docker build" rather
# than bind-mounted: on macOS a bind mount crosses the VM's filesystem
# virtualization layer, which makes C++ compilation dramatically slower.
# Dependencies are installed in their own layer so edits to the source do not
# re-run apt.
#
# dovecot-imapd is here for tests/net_imap_live_test, which starts a real IMAP
# server on a high port with a seeded Maildir and drives jlib::net::Imap4
# against it.  It is the only way to exercise the literal handling end to end:
# a std::istringstream can be made to produce any response, but only a server
# decides when to send a literal.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        g++ \
        gdb \
        make \
        autoconf \
        automake \
        libtool \
        pkg-config \
        autoconf-archive \
        ca-certificates \
        libjson-c-dev \
        libgpgme-dev \
        libssl-dev \
        libsodium-dev \
        portaudio19-dev \
        libglfw3-dev \
        libx11-dev \
        libxext-dev \
        libgl-dev \
        libglu1-mesa-dev \
        freeglut3-dev \
        dovecot-imapd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/jlib

COPY autogen.sh configure.ac Makefile.am AUTHORS ChangeLog NEWS README ./
COPY m4/ ./m4/
COPY jlib/ ./jlib/
COPY tests/ ./tests/

RUN ./autogen.sh

RUN mkdir -p build && cd build && ../configure

RUN cd build && make -j"$(nproc)"

CMD ["/bin/bash"]
