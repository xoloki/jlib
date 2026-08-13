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
