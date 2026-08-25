# jlib

A personal C++ utility library by Joey Yandle, begun ~1999-2000 and worked on
sporadically ever since (CVS → SourceForge → git import in 2011). Apache-2.0
since 2026, GPL v2+ before that. Version 1.2.0; installs headers under
`$(includedir)/jlib-1.2/jlib/<module>` and ships a `jlib-1.2.pc` pkg-config
file.

The name of the game is "STL-compatible C++ utility classes": most of the library
either models an STL container/iterator concept or plugs into `std::streambuf` /
`std::iostream`. Everything lives in `namespace jlib::<module>`.

## Layout

Each subdirectory of `jlib/` is one automake-built libtool library named
`libj<module>.la`. `jlib/apps/` holds the executables, `tests/` the test programs.

| Dir | Library | What it is |
| --- | --- | --- |
| `jlib/sys` | `libjsys` | The foundation. iostream-based wrappers over OS facilities: `socketstream`, `sslstream`/`tlsstream`, `proxystream`, `sslproxystream`, `serialstream`, `pstream` (subprocess), `tfstream` (temp file). Plus `Servent`/`ASServent` (threaded worker + command queue), `sync<T>` (mutex-wrapped value, C++11), `ringbuffer<T>` (lock-free, one producer and one consumer, written for the audio callback), `pipe`, `Directory`, `joystick`, `Object` (a polymorphic base), `signal<R(Args...)>`. |
| `jlib/util` | `libjutil` | String/format grab bag: `util.hh` (tokenize, trim, chop, base64/qp/URI codecs, byte `get`/`set`), `Regex` (POSIX `regex.h`), `Date` (RFC-822 etc.), `Headers` (MIME header folding), `MimeType`, `URL`, `Timer`, a hand-rolled XML tokenizer/parser/DOM (`xml.hh`, `xmlparser.hh`, `xmltokenizer.hh`), and `json.hh` (a C++ facade over json-c). |
| `jlib/crypt` | `libjcrypt` | `crypt.hh` wraps GPGME (OpenPGP encrypt/sign/verify). The `curve`/`schnorr`/`groth` trio is the recent work: ristretto255 `Scalar`/`Point`/`Commitment` over libsodium, Schnorr proofs (single, double, `GeneralProof<N>`), and Groth binary/zero-argument proofs. Built only when libsodium *with* ristretto headers is present. |
| `jlib/net` | `libjnet` | Email client stack: `Email` (MIME parsing), `MailBox`/`MBox`/`Imap4Box`, `Pop3`, `Imap4`, `MailFolder`, plus `AS*` async variants layered on `sys::ASServent`. `net.cc` has the address-extraction logic the `net_extract_address_*` tests cover. |
| `jlib/media` | `libjmedia` | Audio via PortAudio, driven from its **callback**: `write()` fills a `sys::ringbuffer` and the device's callback drains it, so nothing in the path sleeps or polls. `AudioSink` is the device interface and `PortAudioSink` the implementation; `Player` (a `Servent`) runs a feeder thread so transport commands never wait on the device. Plus `AudioFile`/`WavFile`, `PlayList`, and streambuf-based `datastream`/`wavstream`/`notestream`/`audiofilestream`. `Type.hh` is a template-specialization table over PCM sample formats. `Dsp` is the retired OSS backend, kept but not built. |
| `jlib/math` | `libjmath` | Header-only despite being a `lib_LTLIBRARIES` (its `_SOURCES` are all `.hh`). `matrix`, `vertex`, `tensor`, `buffer`, `polynomial` (templated on a Power type so it can hold curve `Scalar`s), and `Plot<T>` — the abstract plotting base, with `projection_mode` for perspective, orthographic or mixed. `object<T>` holds index-based topology — vertices, edges, and 2-faces built lazily — with `cuboid`, `pyramoid`, `spheroid`, `staroid` and `torus` (a flat k-torus in n dimensions; equal radii and k=2 is the Clifford torus). |
| `jlib/x` | `libjx` | Raw Xlib: `Display`, `Window`, and an X11 `Plot`. |
| `jlib/gl`, `glu`, `glx`, `glfw` | `libjgl`, … | Thin OpenGL layers: lights/buffers/shapes, textures/projection, a GLX window (Linux only), and a GLFW window that works everywhere. `glx/Plot.hh` and `glfw/Plot.hh` are backends for `math::Plot<T>`. |
| `jlib/ai` | `libjai` | Norvig-style agent scaffolding (`Agent`, `Environment`, `Percept`, `Action`, `vacuum`) plus `neural.hh`, a templated feed-forward net. |
| `jlib/cuda` | `libjcuda` | cuBLAS `gemm` and a CUDA port of `neural.hh`. Built only with `--with-cuda`. |
| `jlib/bio` | — | Not in `SUBDIRS`, has no `Makefile.am`, and does not compile (duplicate `class Homo`, undeclared base `Homini`). A sketch, not code. |

Dependency direction: `sys` ← `util` ← `crypt` ← `net`; `gl` ← `glu` ←
`glx`/`glfw`, with `glx` additionally on `x`. Nothing depends on `math`, `ai`,
or `cuda`.

### Apps (`jlib/apps/`)

- `jlib-mail` — the flagship: a command-line mail client over `net` + `crypt`.
- `jcrypt` — GPGME encrypt/decrypt filter. `jcurve` — ristretto/proof driver.
- `jhyper`, `jglxhyper`, `jglfwhyper`, `jhardhyper` — the 4-D hypercube/torus
  visualizations, named for their backend. Shared logic is in `Hyper.hh`
  (`HyperPlot<T, Plot>`, templated on the plot backend so the same code renders
  under X11 raster, GLX, or GLFW), with `color.hh` holding the HSV helpers both
  it and `jhardhyper` use. The two are deliberately different: **jglfwhyper**
  reduces all the way to 2-D in software and is where the projection modes are
  demonstrated; **jhardhyper** reduces N→3 and hands real 3-D to the GPU, so it
  is the one that draws **solids** — depth-sorted translucent faces, per-frame
  normals, and a stereographic mode that shows the Hopf foliation as nested
  tori. It still carries a forked copy of `HyperPlot` for that reason (#28).
  `jgltorus`, `jglxbox` are simpler GL demos.
- `jneural-zero`, `jneural-alpha`, `jneural-search` — neural net experiments
  (`-alpha` does image classification via ImageMagick++/MNIST).
- `jjoystick`, `jjoy2xev` — Linux joystick → X events, with `*-{axes,buttons}-*.map`
  data files for WoW / KSP / SW:TOR on PS3 and X360 pads.
- `jnote`, `jmelody`, `jm3u`, `jpoisoned`, `jcublas`.
- `jhypermusic.cc` and `jneural.cc` are **not** in `bin_PROGRAMS` — scratch files.

## Build

GNU autotools, out-of-tree builds expected:

```sh
./autogen.sh
mkdir build && cd build
../configure
make -j8
make check
```

Builds clean on macOS (Apple clang, arm64) and modern Linux (gcc 13). C++20 is
required and selected by an explicit probe in `configure.ac`, which also sets
`-Werror=return-type` — a missing return is undefined behaviour that gcc turns
into a crash at -O2, and it is never intentional.

`configure` requires json-c, gpgme, gpg-error and openssl. libsodium+ristretto,
PortAudio, GLFW, ImageMagick++, X11, GL/GLU and CUDA are optional and gate their
modules via `AM_CONDITIONAL`; configure prints a summary of what it found.
libsigc++ and glibmm are gone — `sys/signal.hh` and `std::` replaced them.

`Dockerfile` builds an Ubuntu 24.04 image and compiles jlib inside it; the source
is copied in rather than bind-mounted, because a bind mount on macOS crosses the
VM's filesystem layer and is dramatically slower. See `DOCKER`.

Tests are plain `main()` programs registered in `TESTS` — no framework. They
`std::cerr` a message and return non-zero on failure. Exit 77 means SKIP, which
is how tests needing a display or an audio device report a headless machine.
The media tests are silent unless passed `--play` or `--play-all`.

## Conventions

- Headers are `.hh`, sources `.cc`. Include guard `JLIB_<DIR>_<FILE>_HH`.
- Every file opens with the emacs modeline `/* -*- mode: C++ c-basic-offset: 4 -*-`
  and the Apache-2.0 boilerplate, ending in an `SPDX-License-Identifier:`
  line. Keep both when adding files.
- Includes of jlib headers are always absolute: `#include <jlib/util/util.hh>`
  (built with `-I$(top_srcdir)`).
- Each class nests its own `class exception : public std::exception` holding a
  `std::string m_msg`, prefixed with the class name. Errors are thrown, not returned.
- Members are `m_`-prefixed. Container-like classes forward the full STL typedef
  block (`rep_type`, `iterator`, `size_type`, …) from their underlying container.
- 4-space indent. Older code indents one level per `namespace`; newer code
  (`sys/sync.hh`, `sys/ASServent.hh`, `crypt/curve.hh`, `math/tensor.hh`,
  `util/json.hh`) does not. `TODO.md` says the flat style wins — prefer it, and
  don't re-indent old files gratuitously.
- Inline template definitions go at the *bottom* of the header, after the class
  bodies, not inside them (see `crypt/curve.hh`, `math/tensor.hh`).
- In `.cc` files, drop redundant `jlib::` qualifiers — the file is already in the
  namespace.
- Strings that carry a payload go by `const&`, or by value plus `std::move` when
  the callee stores them. Much of the library predates C++11, when libstdc++'s
  `std::string` was copy-on-write and passing one by value was free; it is a deep
  copy now. A handful take by value deliberately — `Headers::decode`,
  `net::convert_to_crlf`, the note parser — because they modify and return the
  argument, and say so.
- A user-declared destructor suppresses the implicit move operations, so an empty
  one silently costs a class its move semantics. Prefer none; where one is needed
  (a polymorphic base, or it anchors the vtable), default the other four.

## State of things

Between 2020 and 2021 the active work was `crypt` (curve/schnorr/groth) and the
`math::polynomial` support behind it. In 2026 the library was ported to build on
macOS and modern Linux, in a seven-phase plan that is now **complete**: autotools
modernized to C++20, libsigc++ and glibmm replaced with `std::`, OSS replaced
with PortAudio, GLUT replaced with GLFW, 4-D objects rendered as solid 3-D
geometry, and an API pass over copies, moves and exception specifications.

The headline goal since 1999 — rendering 4D+ objects as solid 3-D geometry — is
done. `jhardhyper` draws translucent depth-sorted faces with per-frame normals,
for hypercubes and for the flat torus, and under stereographic projection shows
the Hopf foliation as nested tori.

Both `TODO.md` items are done — libsigc++ is gone, and `sys/sslstream.hh` now
uses the system trust store and verifies the hostname with `SSL_set1_host`.

Remaining work is tracked in GitHub issues; the phase labels are historical now.
What is left is a normal backlog rather than porting: one network bug (#11,
SIGPIPE terminates the process on a dropped connection), the graphics chain that
would let one `HyperPlot` serve both apps (#20 → #24 → #28), tessellation and
face enumeration for the remaining shapes (#31, #35), and some efficiency and
API tidying (#47, #53).

Two habits from this work worth keeping. **Measure before diagnosing** — several
"obvious" causes here were wrong, and the counter-example was usually a ten-line
program under a counting `operator new` or a sanitizer. And **say what a test
does not establish**: `sys_ringbuffer_test` passes, and neither it nor TSan
verifies the memory ordering it depends on, which is written down in both.
