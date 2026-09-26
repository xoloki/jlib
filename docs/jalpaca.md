# jalpaca

jchat with a transcript that stays put: a scrolling transcript above, a
prompt pinned below, and the reply arriving into the transcript a token at a
time.

A map. Every type named here is documented at its declaration, and where the
two disagree the header is right.

- `jlib/apps/jalpaca.cc`
- `docs/jchat.md` — the same loop, written to a stream
- `docs/ai.md` — the engine underneath

## Why it is a second program and not a flag on jchat

Because jchat is the tested one. `app_jchat_test` drives it down a pipe —
arguments, answers, conversation memory — and **a curses program cannot be
driven down a pipe**: it wants a terminal and refuses to start without one.

Folding the two together would mean either giving up that test or carrying
two output paths through every function in jchat. So the loop here is
deliberately the same loop, over the same pieces — gguf, tokenizer, chat,
model, generate — and what differs is where the characters go.

## What curses buys beyond appearance

**Escape**, and it is not a cosmetic feature. Reading a keystroke *while*
generating needs the terminal out of canonical mode, which otherwise buffers
input until Return. Having taken it out, something has to put it back on
exit, on an exception **and on a signal**, or the shell is left with no echo.

curses owns both halves. Doing it by hand with `tcsetattr` is about forty
lines and every one of them is a way to leave somebody's terminal broken.

## The cost, stated

**It is the only program here with no test in `make check`.** jserve, jchat,
jcode and jhttpd all have one; this has none — which is the same fact as the
first section, seen from the other side. The argument that a curses program
cannot be driven down a pipe is also the reason nothing drives this one.

A harness that *can* test it exists — a pty with a terminal emulator on the
other end, so an assertion is made against a rendered screen rather than a
byte stream — and it lives outside the repository, which is **#247**. Until
that lands, changes here are checked by running it.
