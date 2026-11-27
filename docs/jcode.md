# jcode

A map. Every type named here is documented at its declaration, and where the two disagree the header is right.

- `jlib/apps/jcode.hh`, `jcode.cc`, `jcode_main.cc`
- `docs/jserve.md` — the server it talks to

A coding harness over jserve: read the files, lay out a request that fits, send it down a connection that stays open, stream the reply, read the edits out of it, and put them on disk after saying what will happen.

**It asks before it writes**, unless `--yes`. A harness that writes without asking is fine when version control is watching and reckless when it is not, and jcode cannot tell which it is in. `--dry-run` decides everything and writes nothing.

## Two edit formats, because they fail differently

This is the part worth understanding, and the reason both exist (#348):

| | `whole` | `hunks` |
|---|---|---|
| shape | filename, fence, the entire file, fence | filename, then search/replace blocks |
| needs tool calling | no | no |
| how it goes wrong | **by omission, into the file** | **by not matching, into a refusal** |

A whole-file reply that quietly drops a function writes a file missing a function, and nothing notices until something fails to build. A hunk whose `search` text is not in the file cannot be applied, so it is refused and said out loud. `search` is the text **as it is now, exactly** — not a pattern and not a paraphrase — and `apply()` refuses a hunk it cannot find *and one it finds twice*, rather than guessing which was meant.

**Measured, the omission is not a quietly dropped function — it can be the whole file** (#464). Asked for whole files, Qwen2.5-Coder-7B sometimes returned only the part it changed, often in a fence with no filename; given the only name there was, jcode wrote it over the file and said `written`. Ten edits in `tools/edit-formats`: `sampler.cc` went from 203 lines to 8, `openai.cc` from 1074 to 22, and `listener.cc` lost its licence and `#include`s, two of the three around a change that was itself correct. So a whole-file block **replaces the part of the file it accounts for**, which for a well-formed whole file is all of it, exactly as before, and for one function is that function. It is placed by the lines that occur once in the file and once in the block — patience diff's idea, because a unique line says where it is and `}` says nothing — and its other lines are lined up between and beyond those. Replayed through it, the recorded replies become a correct edit at lines 964–989 of `openai.cc`, another at lines 37–297 of `listener.cc`, and a refusal for `sampler.cc`, whose first line the model had rewritten; the six correct whole-file edits are unchanged.

A block that cannot be placed with certainty is refused, with the reason: its first or last line is not in the file, so where it begins or ends is not known; it leaves out a run of the file between two parts it shows, which is two snippets pasted together; or it is shorter than the file and has no line in common with it that occurs once in each. **What that costs, said out loud**: a fragment that deletes lines inside the code it shows has the seam's shape and is refused — a whole file or a hunk can delete, and a fragment cannot — and a whole file that changes one of a run of identical lines at its very end, such as one of three closing braces, keeps the file's own last one as well. The comment at `place()` in `jcode.cc` has the rules and the cases that taught each. Every write question says what it touches — `write jlib/ai/openai.cc? lines 964-989 of 1074` for a placed block, `203 to 154 lines` for a whole file — and so does every line saying a write landed, which matters most under `--yes`, where nobody was asked. Hunks are untouched by any of it: their deletions are explicit.

**And every file jcode wrote whole lost its empty lines**, from #365 until #464 found it. The search/replace parser holds each line back in case it names the file the next block is for, knew whether it held one by whether the line was empty, and so never let an empty line go; a blank line holding spaces survived. The measurement judged edits by their non-blank lines and could not see it: every whole-file edit it called correct had also stripped the file — `sampler.cc` of all 51 of its blank lines. Hunk edits kept every line.

The two are alternatives rather than layers: a reply is in one form or the other, and an edit carries whichever arrived.

## Lenient about the envelope, with every guess on the record

Models get the wrapper wrong constantly — they bold the filename, end it with a colon, make it a heading, wrap it in backticks, or paste the `path/to` prefix out of the prompt's own example. A harness that refuses a reply over any of that is useless, so each is absorbed.

**Each absorption is a guess, and each is recorded against the edit it produced.** `edit::guesses` carries them and a caller is expected to print them, because a guess nobody can see is how the wrong file gets written and nobody learns why. Anything not on the list is refused rather than guessed at.

## A fence that contradicts its file is an illustration

Found the hard way (#364): forty-one lines of C became one line of `make`, because the last fenced block in a reply was an illustration and the harness took it for the edit.

So a fence whose own language contradicts the file it would be written to —
```` ```sh ```` against a `.c` file — is not an edit (#366). The model said
both things and one of them is wrong; it is not jcode's job to decide which,
and it *is* jcode's job not to write a shell snippet over a source file.

**Narrow on purpose**: only a recognised language against a recognised
extension can disagree. An unknown info string, or none, says nothing and
stays lenient, because models fence source with no info string all the time.

## Tool calling, and the two loops around it

`toolbox()` registers three, all bounded by `--root`, which no path may
escape:

| tool | does |
|---|---|
| `read_file` | a file the model asked to see |
| `search` | finds a string in the tree |
| `build` | runs the project's build and answers with its output |

**`build` is not registered at all without `--build`.** A default jcode can
run nothing, which is the #242 constraint made concrete rather than promised.

**Two loops, bounding different runaways.** `--rounds` (default 8) bounds tool
calls *within* one exchange; `--attempts` (default 3) bounds write-build-retry
cycles. A model calling `read_file` forever and a model rewriting the same
file forever are different failures and one number cannot bound both.

The outer loop is what makes this a harness rather than a patch generator:
write the edits, run the named build, feed its failure back, try again
(#341).

## The write is not a tool, deliberately

The single most important decision in jcode. A `write_file` the model could
call would make the loop considerably simpler, and it is exactly what #242
forbids: it moves the decision inside the loop where nobody sees it.

So the write stays outside — announced and confirmed — and only its *result*
goes back to the model. Every edit is still subject to `--yes` and
`--dry-run`.

## Why the markup is named here

`system_prompt()` takes the tool list, and the prompt states the markup
explicitly rather than trusting the model's own:

    <tool_call>
    {"name": "the tool", "arguments": {"...": "..."}}
    </tool_call>

Measured against Qwen2.5-Coder-7B at Q4_K_M: with no system turn it answers
`<function_call>` inside a fenced xml block; with one it answers bare JSON and
no markers at all. **Its own template asked for `<tool_call>` in both cases.**

Bare JSON is the one thing this reader cannot accept, and the reason is
jcode-specific: it sends whole files, so a reply containing a `.json` file
would be read as a call and swallowed out of the content. Naming the markup
is the half of the contract this end controls — the same arrangement as the
filename-and-fence rules, for the same reason.

## What it is not

jcode does not choose its own files: the layout decides what fits in the
request, and a file nobody named is a file the model never sees.

It does not link `ai::engine`. It speaks the OpenAI wire format to jserve,
which is what lets it point at a hosted model instead without knowing the
difference — and is why `--url` is a flag rather than a default that could
reach past the machine.
