# jlib

A personal C++ utility library by Joey Yandle, begun ~1999-2000 and worked on
sporadically ever since (CVS → SourceForge → git import in 2011). GPL v2+.
Version 1.2.0; installs headers under `$(includedir)/jlib-1.2/jlib/<module>` and
ships a `jlib-1.2.pc` pkg-config file.

The name of the game is "STL-compatible C++ utility classes": most of the library
either models an STL container/iterator concept or plugs into `std::streambuf` /
`std::iostream`. Everything lives in `namespace jlib::<module>`.

## Layout

Each subdirectory of `jlib/` is one automake-built libtool library named
`libj<module>.la`. `jlib/apps/` holds the executables, `tests/` the test programs.

| Dir | Library | What it is |
| --- | --- | --- |
| `jlib/sys` | `libjsys` | The foundation. iostream-based wrappers over OS facilities: `socketstream`, `sslstream`/`tlsstream`, `proxystream`, `sslproxystream`, `serialstream`, `pstream` (subprocess), `tfstream` (temp file). Plus `Servent`/`ASServent` (threaded worker + command queue), `sync<T>` (mutex-wrapped value, C++11), `pipe`, `Directory`, `joystick`, `Object` (a `sigc::trackable`). |
| `jlib/util` | `libjutil` | String/format grab bag: `util.hh` (tokenize, trim, chop, base64/qp/URI codecs, byte `get`/`set`), `Regex` (POSIX `regex.h`), `Date` (RFC-822 etc.), `Headers` (MIME header folding), `MimeType`, `URL`, `Timer`, a hand-rolled XML tokenizer/parser/DOM (`xml.hh`, `xmlparser.hh`, `xmltokenizer.hh`), and `json.hh` (a C++ facade over json-c). |
| `jlib/crypt` | `libjcrypt` | `crypt.hh` wraps GPGME (OpenPGP encrypt/sign/verify). The `curve`/`schnorr`/`groth` trio is the recent work: ristretto255 `Scalar`/`Point`/`Commitment` over libsodium, Schnorr proofs (single, double, `GeneralProof<N>`), and Groth binary/zero-argument proofs. Built only when libsodium *with* ristretto headers is present. |
| `jlib/net` | `libjnet` | Email client stack: `Email` (MIME parsing), `MailBox`/`MBox`/`Imap4Box`, `Pop3`, `Imap4`, `MailFolder`, plus `AS*` async variants layered on `sys::ASServent`. `net.cc` has the address-extraction logic the `net_extract_address_*` tests cover. |
| `jlib/media` | `libjmedia` | OSS audio (`/dev/dsp`). `Player` (a `Servent`), `AudioFile`/`WavFile`, `PlayList`, `Dsp`, and streambuf-based `datastream`/`wavstream`/`notestream`/`audiofilestream`. `Type.hh` is a template-specialization table over PCM sample formats. Requires `sys/soundcard.h`, so it does not build on modern Linux or macOS. |
| `jlib/math` | `libjmath` | Header-only despite being a `lib_LTLIBRARIES` (its `_SOURCES` are all `.hh`). `matrix`, `vertex`, `tensor`, `buffer`, `polynomial` (templated on a Power type so it can hold curve `Scalar`s), and `Plot<T>` — the abstract plotting base. |
| `jlib/x` | `libjx` | Raw Xlib: `Display`, `Window`, and an X11 `Plot`. |
| `jlib/gl`, `glu`, `glut`, `glx` | `libjgl`, … | Thin OpenGL layers: lights/buffers, textures/projection, GLUT main loop, GLX window. `glx/Plot.hh` and `glut/Plot.hh` are backends for `math::Plot<T>`. |
| `jlib/ai` | `libjai` | Norvig-style agent scaffolding (`Agent`, `Environment`, `Percept`, `Action`, `vacuum`) plus `neural.hh`, a templated feed-forward net. |
| `jlib/cuda` | `libjcuda` | cuBLAS `gemm` and a CUDA port of `neural.hh`. Built only with `--with-cuda`. |
| `jlib/bio` | — | Not in `SUBDIRS`, has no `Makefile.am`, and does not compile (duplicate `class Homo`, undeclared base `Homini`). A sketch, not code. |

Dependency direction: `sys` ← `util` ← `crypt` ← `net`; `x` ← `gl` ← `glu` ←
`glx`/`glut`. Nothing depends on `math`, `ai`, or `cuda`.

### Apps (`jlib/apps/`)

- `jlib-mail` — the flagship: a command-line mail client over `net` + `crypt`.
- `jcrypt` — GPGME encrypt/decrypt filter. `jcurve` — ristretto/proof driver.
- `jhyper`, `jglxhyper`, `jgluthyper`, `jhardhyper` — the 4-D hypercube/torus
  visualizations. Shared logic is in `Hyper.hh` (`HyperPlot<T, Plot>`, templated
  on the plot backend so the same code renders under X11, GLX, or GLUT).
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
autoreconf -i          # or: libtoolize && aclocal && autoheader && automake -a && autoconf
mkdir build && cd build
../configure
make -j8
make check
```

`configure` hard-requires libsigc++-2.0, glibmm-2.4, gthread-2.0, ImageMagick++,
json-c, gpgme, and openssl. X11, GL/GLU/GLUT, OSS audio, libsodium+ristretto, and
CUDA are all optional and gate their modules via `AM_CONDITIONAL`.

That dependency set no longer resolves on a current distro or on macOS, so the
supported path is Docker — see `DOCKER`. `Dockerfile` is a symlink to one of
`Dockerfile.gcc{49,5,61,75,10}`; `gcc75` (Ubuntu 18.04) is the default and the one
that actually builds. Building the image runs `configure` + `make` inside it; the
usual workflow is then to bind-mount the source tree and build again in the
container.

Tests are plain `main()` programs registered in `TESTS` — no framework. They
`std::cerr` a message and return non-zero on failure. Media and curve tests are
conditionally included; `x_window_test` is unconditional and needs a display.

## Conventions

- Headers are `.hh`, sources `.cc`. Include guard `JLIB_<DIR>_<FILE>_HH`.
- Every file opens with the emacs modeline `/* -*- mode: C++ c-basic-offset: 4 -*-`
  and the GPL v2+ boilerplate. Keep both when adding files.
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

## State of things

Active work since ~2020 is almost entirely in `crypt` (curve/schnorr/groth) and
the `math::polynomial` support behind it; the last commit before that was 2021,
and 2025 touched only the Dockerfile. Most of the rest of the library is
essentially frozen 2000s-era code.

`TODO.md` records the standing intentions: migrate off libsigc++ to `std::function`
(starting with `jlib-mail.cc`), and verify certificates in the SSL code —
`sys/sslstream.hh` sets `SSL_VERIFY_PEER` and loads `/etc/ssl/certs`, but never
checks the hostname against the peer certificate.
