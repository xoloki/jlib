# tools/edit-formats

Whole files or search/replace hunks: which one a real coding harness should ask a model for. Measured rather than argued, against a real model, on copies of real jlib files.

    ./jlib/apps/jserve --port 8099 coder=Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf &
    python3 tools/edit-formats/run.py --jcode build/jlib/apps/jcode \
        --url http://127.0.0.1:8099/v1 --model coder --out /tmp/formats

Every case in `cases.json` is run through jcode twice — `--format hunks` and `--format whole` — with `--yes --no-stream`, against a temporary copy of one file. Nothing in the tree is edited. Logs, the model's replies and the files as they ended up all land in `--out`, beside `results.json`.

Never run by `make check`: it needs a model and a GPU, takes most of an hour, and produces numbers to read rather than a pass or a fail. It is in the tree for the reason `tools/perf/` is — a number measured by a script nobody can see is a number nobody can check, and one of those was once wrong by a factor of fifteen (#197).

## What "correct" means

An edit is correct only if all of these hold, which is deliberately stricter than "jcode said written":

| check | why it is there |
| --- | --- |
| jcode applied it, and the file changed | the floor |
| every `must` pattern appears **more** than it did before | "appears" would pass a model that did nothing at the right place, wherever the pattern already existed elsewhere in the file |
| no `must_not` pattern appears | what was ruled out stayed out |
| at most `max_lost` of the original's lines are gone | **the check that catches an elided whole file** — see below |
| the file still passes `-fsyntax-only` | checked first against the untouched original, so a failure is the edit's and not the check's |

**Lines lost is the one that matters most for `whole`.** A model returning a whole file can write `// ... rest unchanged` in place of the rest, and the result parses and is written, and the code is gone. jcode's system prompt asks it not to; nothing in `parse()` detects it. Counted as a multiset of stripped, non-blank lines, so a re-indent is not a loss. A hand-made elision of `sampler.cc` — the right change, and the second half replaced by a comment — loses 71 lines; a correct edit loses at most the lines it rewrites.

## The checks were checked

Before any model was asked anything, an offline pass confirmed that the untouched file is judged wrong for every case, that a hand-made correct edit is judged right, and that the elision above is judged wrong. That pass also caught a broken case: `ringbuffer.hh` already had the `clear()` the first version of a case asked for, so a model that correctly said so would have been scored as failing. It asks for a `peek()` that does not exist instead.

## Why a failure is classified, not only counted

The smoke test turned up a failure that is not about the format at all. A hunk for `sampler.cc` was exactly right — search text character for character, replacement correct — and was refused, because the model named the file `path/to/filename.cc`: **the placeholder in jcode's own system prompt, copied whole.** The basename rescue cannot help, because `filename.cc` is not a file that was sent. Both formats' prompts show the same placeholder, so charging that to hunks would be charging the format for a prompt defect. So every wrong result carries a cause, and the summary splits them:

| cause | means |
| --- | --- |
| placeholder name | the model wrote the prompt's example filename |
| search text did not match | a hunk's SEARCH block is not in the file, or is in it twice |
| took code with it | more of the original is gone than the edit accounts for |
| did not do what was asked | a `must` pattern did not appear |
| does not compile | `-fsyntax-only` refused the result |

## What it does not establish

**One model, one quantisation, one temperature.** Qwen2.5-Coder-7B-Instruct-Q4_K_M at `--temp 0.2`. A stronger model might match hunks more often and elide whole files less, and a weaker one the reverse; aider's own advice is diff formats for capable models and whole files for weak ones, and this measures where one 7B coder sits.

**Ten cases, once each.** The model samples, so a case that passed once can fail the next time. Enough to see a difference of kind — thousands of output tokens against tens — and not enough for a success rate to two significant figures.

**Edits a file at a time, with the instruction spelled out.** Every request names the change precisely and says "change nothing else". A real request is vaguer and may span files, and that makes whole-file replies longer still, not shorter.

**Output tokens are estimated.** jcode prints what the server counted for the prompt but not for the reply, so the reply is measured in bytes and divided by Qwen's measured 3.84 bytes per token. Close enough for a difference of two orders of magnitude, and labelled as an estimate wherever it is printed.

**Wall-clock time is the weaker number.** It depends on the machine — its swap, its other load — where the outcome of an edit does not.

## Results, 2026-11-26

Qwen2.5-Coder-7B-Instruct-Q4_K_M at `--temp 0.2`, through jserve on an M-series laptop, jcode at master `82b42d3`. Merged from three batches with `summarise.py` — the first lost its server partway through, and the lost cases were rerun.

| case | hunks, as jcode handled it | hunks, re-applied under the real name | whole |
| --- | --- | --- | --- |
| `ringbuffer-peek` | refused: placeholder name | the model sent a whole file instead | **correct**, ~1776 tok |
| `ringbuffer-noexcept` | refused: placeholder name | **correct**, 5 blocks | wrote nothing — a tool call written as text |
| `sampler-topk` | refused: placeholder name | **correct** | **correct**, ~1741 tok |
| `sampler-ties` | refused: placeholder name | **correct** | **destroyed the file: 203 lines → 8** |
| `listener-localhost` | refused: placeholder name | refused: whitespace differs | **297 lines → 197** around a correct change |
| `listener-reuseport` | refused: placeholder name | the model sent a whole file instead | **correct**, ~2322 tok |
| `directory-rename` | refused: placeholder name | refused: misquoted a signature | **correct**, ~1654 tok |
| `quant-upper` | refused: `path/to/quant.hh` | **correct** | **correct**, ~1994 tok |
| `relay-idle` | refused: placeholder name | the model sent a whole file instead | **correct**, ~2155 tok |
| `openai-sort` | refused: placeholder name | **correct** | **destroyed the file: 1074 lines → 22** |

**Whole: 6 of 10 correct, and 3 of 10 destroyed code while jcode reported `written`** (#464). The line counts in the table are the files' own, before and after; the lines-lost check, which ignores blank lines, counted 146, 33 and 760. In two of those three the change itself was right and the file around it was gone.

**And all six correct ones stripped the file of its empty lines** — `sampler.cc` all 51 of its blank lines, `Directory.cc` the 4 of its 24 that held nothing at all; the 20 holding spaces survived. That was jcode, not the model: from #365 until #464 found it, an empty line in a reply never reached the whole-file parser. Lines lost ignores blank lines on purpose, so that a re-indent is not a loss, and so could not see it; read the whole column as correct in its code and wrong in its layout. Hunk edits kept every line.

**Hunks: 0 of 10 as jcode handled them**, every one refused for its filename (#465). Behind the names, the seven replies that really were hunks were **5 correct and 2 refused, and none destroyed anything**. The other three ignored the format and sent a whole file.

**Output**: a genuine hunk reply had a median of ~166 tokens against ~1,758 for a whole file — the difference between seconds and minutes, at the 4.1–9.1 tok/s the correct whole-file edits came out at here, prefill included. Wall times varied two- to three-fold between batches with swap and other load, so they are in the logs and not in this table.

What it says: **hunks are the safer format and the cheaper one, and they are unusable until #465 is fixed.** Whole is usable today and destroys files three times in ten. #464 needs fixing whichever format is the default.

## Rerun after #464

**Whole: 8 of 10 correct, and none destroyed.** #464 places a whole-file block in the part of the file it covers rather than writing it over all of it, and keeps the empty lines. The ten whole-format cases were rerun with that jcode on 2026-11-27. Every reply came back the same size as before, token for token, so the replies are the same and the difference is jcode's:

| case | before #464 | after |
| --- | --- | --- |
| `sampler-ties` | **destroyed the file: 203 lines → 8** | refused: its first line, rewritten by the model, is not in the file; file untouched |
| `listener-localhost` | **297 lines → 197** around a correct change | **correct**: `lines 37-297 of 297`, the licence and `#include`s kept |
| `openai-sort` | **destroyed the file: 1074 lines → 22** | **correct**: `lines 964-989 of 1074` |
| the six correct ones | correct in their code, every empty line gone | **correct**, each exactly the file the model sent |
| `ringbuffer-noexcept` | wrote nothing | wrote nothing |

The conclusion above changes in one half: whole format destroyed no file here, and what it costs instead is a refusal or, in one documented shape, a line kept that should have gone (see `place()` in `jlib/apps/jcode.cc`). Hunks are still the cheaper format by ten times in tokens, and still unusable until #465.

## Rerun after #465

**Hunks: 8 of 10 applied and 5 correct, where before every one was refused for its name.** The prompt now shows the model its real file when one is sent, and `clean()` maps the example's name back when a model copies it anyway; no reply used `path/to/filename.cc`, and none was refused for its name. Both halves ran on 2026-11-27 (`build/edit-formats/names`).

| case | hunks | whole |
| --- | --- | --- |
| `ringbuffer-peek` | **correct**, ~588 tok | **correct**, ~1776 tok |
| `ringbuffer-noexcept` | refused: the search text left out the comments between the functions it showed | **correct** — before, it wrote nothing |
| `sampler-topk` | wrong: folded the new check into the old one and rewrote its message | **correct** |
| `sampler-ties` | **correct**, ~77 tok | **correct** — before, a fragment, refused |
| `listener-localhost` | **correct**, ~42 tok | **correct** |
| `listener-reuseport` | wrong: redeclared `on`, and does not compile | **correct** |
| `directory-rename` | wrong: renamed the declarations, not their uses | **correct** |
| `quant-upper` | **correct**, ~116 tok | **correct** |
| `relay-idle` | **correct**, ~77 tok | **correct** |
| `openai-sort` | not applied: a correct hunk, sent with a tool call (#469) | no result: still generating after 44 minutes when the harness's time limit stopped it |

**The three wrong hunks are the model's edits applied as written**, not jcode's: each SEARCH matched exactly once and was replaced by what the model sent. Two of the three do not compile, which is what jcode's `--build` loop hands back to the model; this harness does not run it. The one hunk refused was refused correctly — its SEARCH was not text in the file — and the one not applied was correct, lost to #469.

**Whole: 9 of 9 correct, and the new prompt changed whole-file replies too**: shown its real filename, the model sent the whole file in two cases where before it sent a fragment or nothing.

**Output**: a median hunk reply of ~84 tokens against ~1,776 for a whole file, and a median of 104 s against 258 s a case.

What it says: **hunks apply now**, at a twentieth of the tokens; **whole was right more often in this run**, 9 of 9 against 5 of 10. Two of the wrong hunks went wrong in lines the hunk did not show — the uses of the renamed variables, the `on` already declared above — which a whole file makes the model write out, and one hunk's SEARCH was not the file's text. Neither format destroyed a file. Which should be the default is that trade, and it is worth measuring once more with a build loop on before deciding, to see how many of the wrong hunks a compile error recovers.
