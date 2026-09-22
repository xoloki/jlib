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
# dovecot is here for tests/net_{imap,pop3}_live_test, which start a real
# server on high ports with a seeded Maildir and drive jlib::net::Imap4 and
# jlib::net::Pop3 against it.  tinyproxy is for the same reason one layer out:
# jlib can reach a server through an HTTP CONNECT proxy, and until there was a
# proxy to test against, that path had never run.  It is the only way to
# exercise the literal handling end to end: a std::istringstream can be made to
# produce any response, but only a server decides when to send a literal.
#
# nginx is here for tests/net_http_live_test, and for the same reason.  The
# HTTP client is tested against a server written in the test as well, because
# that is the only way to get a chunked body whose data looks like its own
# framing -- but a server somebody else wrote sends a Date and a Server and an
# ETag and a 404 body nobody here thought of, and those are what an in-process
# server can never provide.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        debhelper \
        dh-autoreconf \
        fakeroot \
        g++ \
        gdb \
        make \
        autoconf \
        automake \
        libtool \
        pkg-config \
        autoconf-archive \
        ca-certificates \
        libncurses-dev \
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
        dovecot-pop3d \
        tinyproxy \
        nginx-light \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/jlib

COPY autogen.sh configure.ac Makefile.am AUTHORS ChangeLog NEWS README ./
COPY m4/ ./m4/
COPY jlib/ ./jlib/
COPY tests/ ./tests/
COPY tools/ ./tools/

# The packaging, so the image can build a .deb as well as run the suite. Small,
# and it means `debian/` is tested by the same container that tests the code --
# a rules file that only works on the maintainer's machine is the sort of thing
# that is discovered at release time.
COPY debian/ ./debian/

RUN ./autogen.sh

RUN mkdir -p build && cd build && ../configure

RUN cd build && make -j"$(nproc)"

# clang, for the coverage-guided fuzzers in tests/fuzz (#268). Apple's clang
# does not ship libFuzzer -- there is no libclang_rt.fuzzer_osx.a in the Xcode
# toolchain -- and Ubuntu's does, which is why the fuzzers run here.
#
# **A late layer, deliberately.** Adding clang to the dependency layer above
# would invalidate the cached build of the entire tree for everyone who only
# ever runs the suite, to install a compiler they never invoke.
# **Without --no-install-recommends, unlike every other apt line here.** The
# sanitizer runtimes -- libclang_rt.fuzzer, libclang_rt.asan -- are recommended
# by clang rather than depended on, so the lean install yields a compiler that
# accepts -fsanitize=fuzzer,address and then fails at link with four missing
# archives. libclang-rt-18-dev is the package, named for a version that will
# move; letting apt follow the recommendation survives the next toolchain.
RUN apt-get update && apt-get install -y clang \
    && rm -rf /var/lib/apt/lists/*

# Where the fuzz corpus and any crashing inputs live inside the container.
# They are copied in and out with `docker cp` rather than bind-mounted: a
# corpus is thousands of small files that libFuzzer opens one at a time at
# startup and appends to throughout a run, which is precisely the access
# pattern a macOS bind mount punishes hardest. Two bulk transfers at the
# boundary cost nothing; tens of thousands of small ones over FUSE would
# dominate a run whose entire purpose is executions per second.
RUN mkdir -p /corpus /artifacts

CMD ["/bin/bash"]
