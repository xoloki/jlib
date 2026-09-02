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
| `jlib/sys` | `libjsys` | The foundation. iostream-based wrappers over OS facilities: `socketstream`, `sslstream`/`tlsstream`, `proxystream`, `sslproxystream`, `serialstream`, `pstream` (subprocess), `tfstream` (temp file), and the accepting end jlib did without for twenty-six years: `listener` (bind/listen/accept, and who connected), `tls_context` (a refcounted `SSL_CTX`, so a server reads its certificate once rather than per connection), and `server` — accept, optionally secure, hand each connection to a handler as a `socketstream&`, dispatching through a `job_queue` so that serial and threaded are one code path and a number. `basic_tlsbuf` answers a handshake as well as starting one, so `sslstream` works in both directions. Sockets resolve with `getaddrinfo` (so IPv6 works and two threads no longer share `gethostbyname`'s static buffer), take a connect deadline, and can bound a read; `basic_socketbuf` adopts an accepted descriptor with the `sys::adopt` tag. `run(argv, out, err)` executes a program with no shell involved — reach for it rather than `shell()`, which hands its argument to `/bin/sh`. Plus `Servent`/`ASServent` (threaded worker + command queue; neither spins any more — `ASServent` blocks on a `sync<T>` predicate wait for requests and keeps a pipe only for *responses*, because that read end is what a GUI event loop selects on, while `Servent` blocks on its command pipe unless a caller registered a condition or connected `cycle`, which is the only reason to want a tick), `sync<T>` (mutex-wrapped value) and the `job_queue` built on one — post a functor, and either a pool thread runs it or, with no pool, the thread that posted it does, `ringbuffer<T>` (lock-free, one producer and one consumer, written for the audio callback), `pipe`, `Directory`, `joystick`, `Object` (a polymorphic base), `signal<R(Args...)>`. |
| `jlib/util` | `libjutil` | Two halves. The **parsing** half is the 2026 work: `abnf.hh` is a parser-combinator core with an RFC 5234 text front end on top (`compile()` reads an RFC's grammar as it stands), and the grammars themselves are pasted into headers — `rfc5322.hh` (3.2 lexical tokens, 3.3 date-time), `rfc2045.hh`, `rfc2047.hh`, `rfc3986.hh`, and `rfc9110.hh`/`rfc9112.hh` (HTTP, pasted whole) — with `content_type`, `encoded_word`, `URL`, `Date`, `xml` and `http` reading them. `http.hh` is the HTTP *message* layer — a request or a status line, a field section, and how the body after them is framed — and it lives here rather than in `net` because `sys/proxystream.hh` reads the answer to CONNECT with it and `sys` may not include `net`. The **grab bag** is the rest: `util.hh` (tokenize, trim, chop, base64/qp/URI codecs, byte `get`/`set`), `Regex` (POSIX `regex.h`, one live caller left), `Headers` (MIME header folding), `MimeType`, `Timer`, `json.hh` (a facade over json-c). |
| `jlib/crypt` | `libjcrypt` | `crypt.hh` wraps GPGME (OpenPGP encrypt/sign/verify). The `curve`/`schnorr`/`groth` trio is the recent work: ristretto255 `Scalar`/`Point`/`Commitment` over libsodium, Schnorr proofs (single, double, `GeneralProof<N>`), and Groth binary/zero-argument proofs. Built only when libsodium *with* ristretto headers is present. |
| `jlib/net` | `libjnet` | Email client stack: `Email` (MIME), `MailBox`/`MBox`/`Imap4Box`, `Pop3`, `Imap4`, `MailFolder`, plus `AS*` async variants layered on `sys::ASServent`. The protocol grammars live here — `rfc5322.hh` (addresses, appended to util's lexical block) read by `address.{hh,cc}`, and `rfc3501.hh` (IMAP responses) read by `imap_response.{hh,cc}`. `imap::read()` follows literals, which is why a response is not a line; `imap::quote()` writes command arguments. `http.{hh,cc}` is the client and `http_server.{hh,cc}` the server, both deliberately narrow and both saying so; `oauth.{hh,cc}` is OAuth2: the refresh grant, the authorization-code grant with PKCE, a loopback `redirect_receiver`, and the XOAUTH2 message that carries a token to IMAP. |
| `jlib/media` | `libjmedia` | Audio via PortAudio, driven from its **callback**: `write()` fills a `sys::ringbuffer` and the device's callback drains it, so nothing in the path sleeps or polls. `AudioSink` is the device interface and `PortAudioSink` the implementation; `Player` (a `Servent`) runs a feeder thread so transport commands never wait on the device. Plus `AudioFile`/`WavFile`, `PlayList`, and streambuf-based `datastream`/`wavstream`/`notestream`/`audiofilestream`. `Type.hh` is a template-specialization table over PCM sample formats. `Dsp` is the retired OSS backend, kept but not built. |
| `jlib/math` | `libjmath` | Header-only despite being a `lib_LTLIBRARIES` (its `_SOURCES` are all `.hh`). `matrix`, `vertex`, `tensor`, `buffer`, `polynomial` (templated on a Power type so it can hold curve `Scalar`s), and `Plot<T>` — the abstract plotting base, with `projection_mode` for perspective, orthographic or mixed. `object<T>` holds index-based topology — vertices, edges, and 2-faces built lazily — with `cuboid`, `pyramoid`, `spheroid`, `staroid` and `torus` (a flat k-torus in n dimensions; equal radii and k=2 is the Clifford torus). |
| `jlib/x` | `libjx` | Raw Xlib: `Display`, `Window`, and an X11 `Plot`. |
| `jlib/gl`, `glu`, `glx`, `glfw` | `libjgl`, … | Thin OpenGL layers: lights/buffers/shapes, textures/projection, a GLX window (Linux only), and a GLFW window that works everywhere. `glx/Plot.hh` and `glfw/Plot.hh` are backends for `math::Plot<T>`. |
| `jlib/ai` | `libjai` | Norvig-style agent scaffolding (`Agent`, `Environment`, `Percept`, `Action`, `vacuum`) and `neural.hh`, a templated feed-forward net -- plus, since 2026, enough to run a quantized transformer: `gguf` reads the file, `model`/`transformer`/`attention` are the layers (llama, qwen2 and gemma2 -- and **which RoPE layout is not in the file**, it is a per-architecture constant, which is why getting it wrong reads as fluent nonsense rather than an error), `backend` the compute (Metal where there is one), `sampler` picks the next token -- temperature, top-k, top-p and an optional repetition penalty, **off by default because it is a preference rather than a correctness fix**: Qwen 2.5's generation_config asks for 1.1 and Llama 3.2's asks for none, and a GGUF carries neither file -- and `generate` drives the loop. `tokenizer` reads both vocabulary conventions -- SentencePiece and GPT-2 byte-level -- from `tokenizer.ggml.model`, because **the vocabulary flavour and the architecture are independent axes** and assuming either from the other is wrong in both directions. Which of merges or scores drives the tokenizer is a property of the **file**, not the flavour -- TinyLlama and Gemma 2 are both `llama` vocabularies and are exact complements, one carrying 61249 merges and all-zero scores, the other no merges and 256000 real ones. A byte-level vocabulary needs its text cut into chunks before the merges run, and `pretokenizer` is that cut written as an ABNF grammar with the regex pasted above it -- `llama-bpe` and `qwen2`, which differ in one character and are refused by name when a file asks for a third -- over the Unicode category tables in `unicode.hh` (generated by `unicode.py`, which is in the tree so they can be re-derived). `chat` lays a conversation out the way the model's own template says, by *rendering* it: `util::jinja` evaluates `tokenizer.chat_template`, and the template's text stays marked apart from the user's so a marker a user types is not read as one. |
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
- `jneural.cc` is **not** in `bin_PROGRAMS` — a scratch file. (`jhypermusic` is
  built, under the media conditional.)

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
macOS and modern Linux — autotools to C++20, libsigc++ and glibmm replaced with
`std::`, OSS replaced with PortAudio, GLUT with GLFW, 4-D objects rendered as
solid 3-D geometry — and then rewritten to parse by the RFCs rather than by
`find()` and `substr()`.

The headline goal since 1999 — rendering 4D+ objects as solid 3-D geometry — is
done. `jhardhyper` draws translucent depth-sorted faces with per-frame normals,
for hypercubes and for the flat torus, and under stereographic projection shows
the Hopf foliation as nested tori.

**jlib speaks OAuth2, which is what it needed to reach a real mailbox.** Gmail
has wanted it since 2022 and Outlook.com has required it for personal accounts
since September 2024, so a client with only LOGIN and AUTH PLAIN could not
connect to either. Closing that took an HTTP client (RFC 9110 and 9112 pasted
whole, `check()`-clean), a `jlib::sys` that can accept a connection, a real
`AUTHENTICATE` driver, and a `util::json` that survives a stranger's reply.
`net_imap_live_test` runs the whole chain against a real Dovecot: jlib fetches a
token over HTTP, presents it over IMAP with XOAUTH2, and Dovecot goes back over
HTTP to check it — using an in-process server that is the token endpoint and the
tokeninfo endpoint at once, because Google's has the same shape.

What that cannot establish, and no test here can: **neither Google nor Microsoft
will issue a client id without the user registering an application first.** The
library can be complete and the feature still not work for a given user.

**The parsing arc is done too, and it is what most of the library now rests
on.** An ABNF parser was built (`util::abnf`), an RFC's grammar can be pasted
into a header and read as it stands, and everything that used to guess now does
not: addresses (RFC 5322), MIME headers (2045/2047/2231), URLs (3986), dates
(5322 §3.3), IMAP responses (3501), HTTP messages (9110/9112), and XML. The XML parser was replaced
clean-room along the way, which removed the last copyright the author did not
hold and **let the whole library relicense from GPL v2+ to Apache 2.0**.

jlib serves as well as connects now. `sys::server` accepts a connection and
hands it to a handler as a stream, plain or TLS; `net::http::server` reads a
request against the same RFC 9112 grammar the client's responses go through. It
is a server for a loopback OAuth2 redirect and a test harness, **not for a
public port**, and both headers say so — the way a narrow thing becomes a broad
one is by nobody writing down that it was meant to be narrow. An event-driven
design belongs with #4; what had to survive that change is the handler contract,
which is why `run()` is a thin loop over `serve_one()` rather than the reverse.

`sys::server` owns no threads of its own. `job_queue(0)` runs a job on the
thread that posts it and `job_queue(N)` runs it on a pool, with the same
semantics either way, so the server posts and the number decides — where an
earlier draft carried one branch calling the handler inline and another
dispatching it, with a live count, a mutex and a condition variable to go with
them. What it does still own is admission control: the queue is a place
connections can pile up as held descriptors, so the accept loop waits on the
queue's depth *before* it polls, and the overflow stays in the kernel's listen
backlog where a connection is supposed to wait.

That work paid for itself immediately in deletions: `tests/httpserver.hh` lost
its hand-rolled `SSL_accept`, its own descriptor ownership and its byte-at-a-time
head reader, `sys_tls_sigpipe_test` lost twenty lines of raw OpenSSL, and
`oauth::redirect_receiver` stopped parsing a request line by hand. All three
kept passing unchanged, which is what makes it a de-duplication rather than a
rewrite.

The `*_live_test` programs start a real server — Dovecot, tinyproxy, nginx — and
drive jlib against it. They exist because a `std::istringstream` produces only
the responses somebody thought of, and so does an in-process server you wrote
yourself: an IMAP literal, a Date and an ETag in an order nobody chose, a
proxy's 403, are all things only a server decides to send. The in-process
servers (`tests/httpserver.hh`, and the scripted one in `net_imap_sasl_test`)
are for the opposite case — a chunked body whose data looks like its own
framing, a challenge that never ends — which no real server will produce on
request. Both kinds are needed and neither substitutes for the other.

Remaining work is tracked in GitHub issues; the phase labels are historical now.
The HTTP arc left #104 (proxy authentication — and `rfc9110.hh` already records
that `credentials`/`challenge` need reordering under ordered choice before it
can work) and #105 (SOCKS5). Otherwise what is left is the graphics chain that would let one `HyperPlot` serve both
apps (#20 → #24 → #28), tessellation and face enumeration for the remaining
shapes (#31, #35), some efficiency and API tidying
(#47, #69).

Three habits from this work worth keeping. **Measure before diagnosing** —
several "obvious" causes here were wrong, and the counter-example was usually a
ten-line program under a counting `operator new` or a sanitizer. **Say what a
test does not establish**: `sys_ringbuffer_test` passes, and neither it nor TSan
verifies the memory ordering it depends on, which is written down in both. And
**mark every departure from a published grammar at the point of use** — the
`rfc*.hh` headers say `; jlib:` on each one and why, because the whole value of
pasting the RFC is that a reader can check it against the document.
