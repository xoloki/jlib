# juanaco

jcode with a transcript that stays put: a single pane, the task summarised at the top, a prompt pinned below, and the reply — with its tool calls, its build output and its edits — arriving into the transcript between them.

**Planned, not built.** #444 tracks the arc; this document is the design it was filed from, and it will be wrong in the places the code disagrees. Every type named here is documented at its declaration, and where the two disagree the header is right.

- `jlib/apps/jcode.hh` — the library underneath, unchanged
- `docs/jcode.md` — the same loop, written to a stream
- `docs/jalpaca.md` — the same relationship, one layer down
- `docs/jserve.md` — what it talks to

## Why it is a second program and not a flag on jcode

Because jcode is the tested one. `app_jcode_test` covers the pieces and `app_jcode_run_test` drives the whole program against a scripted endpoint and asserts what lands on disk — both by driving it down a pipe, and **a curses program cannot be driven down a pipe**: it wants a terminal and refuses to start without one.

This is jalpaca's argument, unchanged, because it is the same argument. The loop here is deliberately the same loop over the same pieces, and what differs is where the characters go.

## What it reuses, which is almost everything

`jcode.hh` is a library rather than a monolith, and its three injection points are exactly what a terminal interface needs. They exist so the loop can be tested with no model, no server and no disk; that is what makes them reusable for presentation.

| seam | juanaco supplies |
| --- | --- |
| `exchange` | SSE against jserve, deltas pushed to the pane |
| `builder` | build output streamed into the transcript |
| `writer` | per-edit confirmation |

The `writer` comment is the one the whole design turns on: *"Injected because this is where the user is asked, and asking is the half of 'every write announced' that cannot live in a library."* **The seam juanaco needs is the seam that already exists.**

So `converse()`, `until_built()`, `lay_out()`, `parse()` and `apply()` are untouched, and stay covered by jcode's tests rather than needing new ones.

## Streaming without changing a tested typedef

`exchange` returns a *complete* `ai::openai::answer`, which reads like a prohibition on token-by-token rendering. It is not one. juanaco's implementation reads the SSE chunks as they arrive, parses each with `ai::openai::delta::parse()`, pushes every delta to the pane as a side effect, and returns the assembled answer.

The signature never changes, so nothing jcode's tests cover moves.

## The layout

    top line, pinned     task summary, model, mode
    middle, scrolling    user turns, assistant text, tool calls, build output, edits
    prompt box, pinned   multi-line
    status line          tokens in and out, context used, elapsed

The transcript is a list of typed entries rendered on demand, not a string that gets appended to. Resize, scrollback and `/files` all want to re-render the same content differently, and a program that has already flattened everything to characters cannot.

## Where the work runs

The harness blocks: `exchange` makes a synchronous HTTP request, `builder` runs a compiler, `apply` touches disk. So `until_built()` runs on a `sys::relay` thread — jlib's established answer for a long external action, and what jserve already does for generation — reporting back through a pipe while the curses loop reads keys with a timeout and drains events.

`net::http` being synchronous is right here rather than a limitation: the async machinery exists for things that are not a single connection in their own thread, and this is a single connection in its own thread.

**One event flows the other way.** The `writer` confirm needs the worker to ask, block, and receive an answer only the UI thread can collect. Nothing else in the relay pattern is bidirectional, so it is the piece to design rather than assume.

## Escape

It stops a reply without ending the conversation — jalpaca's rule, and for the same reason: the turns so far are the context for the next prompt.

What is new is that it stops the *server* too. Closing the socket makes jserve notice between tokens via `peer_gone()`, and **during prefill** since #402 — which matters more here than anywhere, because a coding prompt is thousands of tokens, and before #402 a cancelled request left jserve prefilling for minutes against a reply nobody would read.

## Confirmation is per edit, and the ordering is the point

jcode_main runs a dry run first and only asks about edits that would actually write:

| outcome | juanaco |
| --- | --- |
| `refused` | a transcript line with the reason — never a dialog |
| `unchanged` | "already what the model sent" — never a dialog |
| `created` / `write` | a dialog, per file |

*"Decided first, written second: a caller is asked about an edit that is already known to be applicable, rather than being asked and then told it was refused."*

juanaco reproduces that control flow exactly and changes only the presentation. **The interface is a skin, not a second policy** — if it decided for itself which edits were worth asking about, jcode and juanaco could drift in what they will write to a disk, and only one of them is covered by `app_jcode_run_test`.

## Slash commands

| command | what it answers |
| --- | --- |
| `/model` | what is available from `/v1/models`, and which is current |
| `/stats` | tokens in and out, elapsed, rounds and attempts, **context used against the ceiling** |
| `/mode` | edit format (`whole` or `hunks`), build on or off, rounds, attempts |
| `/files` | what `lay_out()` put in context, what it trimmed, and the guesses it recorded |
| `/help`, `/quit` | |

`/files` surfaces state that already exists and is invisible today, and it answers the question you most want answered when a model edits the wrong thing: what did it actually see?

`/stats` is not decoration either, and it answers two questions rather than one.
#437 measured a conversation's key-value cache at 1.53x the model's file size by
2048 tokens, and a coding session accumulates context faster than a chat does — files, tool results, diffs. On a 16 GB machine with a 4.4 GB coder model that is a limit reached in ordinary use, and the symptom is paging rather than an error, because **a load that does not fit succeeds**.

The second question is speed. Decode falls from 20.1 to 9.6 tok/s between 256 and 4096 tokens of context — halved — so the same number explains why a long session feels slower than a fresh one. A number on screen is the cheapest thing that turns both from a mystery into a decision.

## What a working juanaco does not establish

**That the prefill wait is reported**, and it is the dominant fact about how this feels. Measured on `Qwen2.5-Coder-7B-Instruct-Q4_K_M` over Metal, machine quiet and swap empty throughout:

| prompt | time to first token | decode after it |
| --- | --- | --- |
| 256 | 1.14 s | 20.1 tok/s |
| 1024 | 3.66 s | 16.7 tok/s |
| 2048 | 12.30 s | 13.4 tok/s |
| 4096 | 30.13 s | 9.6 tok/s |

Prefill is superlinear — doubling the prompt roughly triples the wait — and at 4096 tokens, an ordinary context for a harness that sends whole files, it is **about 93% of the total**: thirty seconds of silence, then a 2.4 second reply. Nothing is on the wire for any of it, so the interface has elapsed time and nothing else.

Two consequences, and the first is the one that surprised me:

- **Decode needs no attention at all.** At 9.6 to 20 tok/s, rendering a token at
a time is free, and throughput polish would be effort spent where there are no seconds to save.
- **An elapsed counter is the floor, not the answer.** Thirty seconds of silence
with a ticking number still reads as hung. #451 is the real fix and it belongs to jserve: the prefill loop already calls a predicate every 256 tokens, added in #402 so a departed client could be noticed, and a loop that asks "does anyone still want this?" can answer "and this is chunk 12 of 16" for nothing.

There is no progress bar in juanaco itself, deliberately. Prefill is superlinear and machine-dependent, so a client-side estimate would be fabricated, and a fabricated bar is worse than a counter that is merely unhelpful.

**That any endpoint but jserve works.** v1 points at jserve only. An arbitrary OpenAI-compatible endpoint brings key handling and rate limits, and `--url` already exists on jcode, so this is a scope decision rather than a missing mechanism.

**That a session survives the process.** There is no persistence in v1. Persistence is the first thing that turns into a file format.

**That two models can be resident at once.** jserve advertises every model it was given, and #278's admission half is unbuilt, so asking for a second one is answered by whatever a failed allocation looks like. `/model` should report a failure rather than appear to hang, which is the most juanaco can do about somebody else's open issue.

## Deliberately absent

No mouse, no split panes, no syntax highlighting, no diff viewer beyond plain hunks, no MCP, no sub-agents. Each is where a terminal interface stops being a program and becomes a project.
