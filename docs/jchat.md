# jchat

Talk to a GGUF model from a terminal. The reference client, and the tested
one — which is why jalpaca is a separate program.

A map. Every type named here is documented at its declaration, and where the
two disagree the header is right.

- `jlib/apps/jchat.cc`
- `docs/ai.md` — the engine it drives
- `docs/jalpaca.md` — the same loop with a curses transcript

Two modes, one program. With a prompt on the command line it answers once and
exits, which is what a script or a test wants; with none it reads turns from
standard input and keeps the conversation, which is what a person wants.

**Progress and timings to stderr, the model's words to stdout**, so
`jchat "..." > answer.txt` is the answer and nothing else.

## Why it is the tested one

`app_jchat_test` drives it down a pipe: arguments, answers and conversation
memory are all checked that way. That is possible precisely because jchat
writes to a stream rather than to a terminal, and it is the reason the curses
front end is jalpaca rather than a flag here — see `docs/jalpaca.md`.
