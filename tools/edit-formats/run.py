#!/usr/bin/env python3
#
# Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

"""Whole files or search/replace hunks: which one a real coding harness should ask for.

    tools/edit-formats/run.py --jcode build/jlib/apps/jcode \\
        --url http://127.0.0.1:8099/v1 --model coder --out /tmp/formats

Runs every case in cases.json through jcode twice -- once asking for whole
files, once for hunks -- against a real model, and checks what landed on disk.
See README.md for what it measures, what it does not, and why it is here.
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Qwen2.5-Coder's measured overall bytes per token over jlib source (jcode.hh's
# own measurement). Used only to *estimate* output tokens, because jcode prints
# what the server counted for the prompt but not for the reply -- and the
# estimate is labelled as one wherever it is printed.
BYTES_PER_TOKEN = 3.84


def lines_lost(before, after):
    """How many of the original's lines are gone, counted as a multiset.

    **The check that catches an elided whole file.** A model that writes
    `// ... rest unchanged` instead of the rest produces a file that parses,
    compiles in parts, and has lost the code -- and jcode only asks it not to.
    Blank lines are ignored, and so is leading and trailing space, so a
    re-indent is not counted as a loss.
    """
    want = {}

    for l in before.splitlines():
        k = l.strip()

        if k: want[k] = want.get(k, 0) + 1

    for l in after.splitlines():
        k = l.strip()

        if k in want and want[k] > 0: want[k] -= 1

    return sum(want.values())


def syntax(root, name, build, cflags):
    """Whether the file compiles with -fsyntax-only, the edited copy first.

    The copy sits under `root` at its own path, and `-I root` comes before
    `-I repo`, so `#include <jlib/...>` finds the edited file and everything
    else from the tree. A header is checked through a one-line unit that
    includes it.
    """
    cmd = ["c++", "-std=c++20", "-fsyntax-only", "-w",
           "-I", root, "-I", REPO, "-I", build] + cflags

    if name.endswith(".hh"):
        unit = "#include <%s>\n" % name

        r = subprocess.run(cmd + ["-x", "c++", "-"], input=unit, text=True,
                           capture_output=True)
    else:
        r = subprocess.run(cmd + [os.path.join(root, name)], text=True,
                           capture_output=True)

    return r.returncode == 0, r.stderr


def cause(err, what, lost, max_lost, missing, ok_syntax):
    """Why an edit is wrong, so the summary can tell format from everything else.

    **The prompt's own placeholder is its own cause, and the first one found.**
    Both system prompts show the model `path/to/filename.cc` as the example,
    and the smoke test for this file had a correct hunk -- search text exact,
    replacement right -- refused because the model copied that name verbatim.
    That hits either format, so counting it as a hunks failure would charge
    the format for a prompt defect.
    """
    # **Applied first.** A reply can carry a refused block *and* a written one
    # -- openai-sort's had a block superseded by a later one for the same
    # file, and the later one wrote 22 lines over 1074 -- so a refusal line
    # only explains a result that was not applied. The first version checked
    # refusals first and labelled that destruction "refused".
    if what == "applied":
        if lost > max_lost: return "took code with it"
        if missing: return "did not do what was asked"
        if not ok_syntax: return "does not compile"

    m = re.search(r"^jcode: refused (\S+): (.*)$", err, re.M)

    if m and re.search(r"filename\.\w+$", m.group(1)): return "placeholder name"

    # **A whole-file block that could not be placed** (#464) is a cause of its
    # own, and before the hunk rule below on purpose: its reasons say "not in
    # the file" too, and that rule labelled the first refusal of one, in
    # sampler-ties, a search that did not match.
    if m and re.match(r"(this block|too many of this block)", m.group(2)):
        for pattern, label in (
                (r"first (line is|\d+ lines are) not in", "its first line is not in the file"),
                (r"last (line is|\d+ lines are) not in", "its last line is not in the file"),
                (r"leaves out", "it leaves out part of the file"),
                (r"no line in common", "nothing to place it by"),
                (r"alike", "too many lines alike")):
            if re.search(pattern, m.group(2)): return "not placed: " + label

        return "not placed: " + m.group(2)[:60]

    if m and re.search(r"(not in|does not (appear|match)|twice|more than once)",
                       m.group(2)): return "search text did not match"
    if m: return "refused: " + m.group(2)[:60]
    if what != "applied": return what
    if lost > max_lost: return "took code with it"
    if missing: return "did not do what was asked"
    if not ok_syntax: return "does not compile"
    return ""


def outcome(err):
    """What jcode says happened, from its stderr."""
    if re.search(r"^jcode: (written|created) ", err, re.M): return "applied"
    if re.search(r"^jcode: refused ", err, re.M): return "refused"
    if "is already what the model sent" in err: return "unchanged"
    if "conversation failed" in err: return "failed"
    return "nothing"


def run_one(case, fmt, a, cflags, logdir):
    root = tempfile.mkdtemp(prefix="edit-formats-")

    try:
        src = os.path.join(REPO, case["file"])
        dst = os.path.join(root, case["file"])

        os.makedirs(os.path.dirname(dst))
        shutil.copyfile(src, dst)

        before = open(src, encoding="utf-8").read()

        argv = [a.jcode, "--url", a.url, "--root", root, "--yes",
                "--no-stream", "--format", fmt, "--context", str(a.context),
                "--temp", str(a.temp),
                "--timeout", "3600", case["request"], case["file"]]

        if a.model: argv[1:1] = ["--model", a.model]

        t0 = time.monotonic()

        r = subprocess.run(argv, text=True, capture_output=True, timeout=4000)

        seconds = time.monotonic() - t0

        after = open(dst, encoding="utf-8").read()

        tag = "%s-%s" % (case["id"], fmt)

        with open(os.path.join(logdir, tag + ".out"), "w") as f: f.write(r.stdout)
        with open(os.path.join(logdir, tag + ".err"), "w") as f: f.write(r.stderr)
        with open(os.path.join(logdir, tag + ".result"), "w") as f: f.write(after)

        what = outcome(r.stderr)

        counted = re.search(r"the server counted (\d+)", r.stderr)

        # **"Must" means "appears more than it did"**, not "appears". A
        # pattern already in the untouched file -- std::sort elsewhere in a
        # large one, say -- would otherwise pass for a model that did nothing
        # at the place it was asked to.
        missing = [p for p in case["must"]
                   if len(re.findall(p, after)) <= len(re.findall(p, before))]
        present = [p for p in case["must_not"] if re.search(p, after)]

        lost = lines_lost(before, after)

        ok_syntax, why = syntax(root, case["file"], a.build, cflags)

        changed = after != before

        # **Correct means all of it**, and in this order of importance: it
        # landed, it does what was asked, it does not do what was ruled out,
        # it did not take code with it, and it still compiles.
        correct = (what == "applied" and changed and not missing and
                   not present and lost <= case["max_lost"] and ok_syntax)

        refused = re.search(r"^jcode: refused .*$", err := r.stderr, re.M)

        return {
            "id": case["id"], "format": fmt, "outcome": what,
            "correct": correct, "seconds": round(seconds, 1),
            "cause": "" if correct else cause(err, what, lost, case["max_lost"],
                                              missing, ok_syntax),
            "refused": refused.group(0) if refused else "",
            "prompt_tokens": int(counted.group(1)) if counted else None,
            "reply_bytes": len(r.stdout),
            "reply_tokens_est": int(len(r.stdout) / BYTES_PER_TOKEN),
            "lines_lost": lost, "max_lost": case["max_lost"],
            "missing": missing, "forbidden_present": present,
            "syntax_ok": ok_syntax, "syntax_err": why[:400],
            "exit": r.returncode,
        }
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])

    p.add_argument("--jcode", required=True)
    p.add_argument("--url", required=True)
    p.add_argument("--model", default="")
    p.add_argument("--build", default=os.path.join(REPO, "build"))
    p.add_argument("--context", type=int, default=16384)

    # Passed on every request rather than left to how jserve was started, so
    # the setting a result was measured at is in the harness, not in a shell.
    p.add_argument("--temp", type=float, default=0.2)
    p.add_argument("--out", required=True, help="where logs and results go")
    p.add_argument("--formats", default="hunks,whole")
    p.add_argument("--only", default="", help="comma-separated case ids")

    a = p.parse_args()

    cases = json.load(open(os.path.join(HERE, "cases.json")))

    if a.only:
        keep = set(a.only.split(","))
        cases = [c for c in cases if c["id"] in keep]

    cflags = subprocess.run(["pkg-config", "--cflags", "json-c"], text=True,
                            capture_output=True).stdout.split()

    os.makedirs(a.out, exist_ok=True)

    # **The check is checked first.** A syntax check that fails on the
    # untouched original would mark every edit wrong for a reason that has
    # nothing to do with the model, and look exactly like a result.
    for c in cases:
        root = tempfile.mkdtemp(prefix="edit-formats-baseline-")

        try:
            dst = os.path.join(root, c["file"])

            os.makedirs(os.path.dirname(dst))
            shutil.copyfile(os.path.join(REPO, c["file"]), dst)

            good, why = syntax(root, c["file"], a.build, cflags)

            if not good:
                sys.exit("the syntax check fails on the untouched %s, so it "
                         "cannot judge an edit to it:\n%s" % (c["file"], why))

            orig = open(dst, encoding="utf-8").read()

            # A forbidden pattern absent from the original rules nothing out,
            # and a check that cannot fail is not one -- so say which.
            for pat in c["must_not"]:
                if not re.search(pat, orig):
                    print("note: %s: %r is not in the original, so forbidding "
                          "it checks nothing" % (c["id"], pat), file=sys.stderr)
        finally:
            shutil.rmtree(root, ignore_errors=True)

    print("baseline: all %d files pass the syntax check untouched" % len(cases),
          flush=True)

    results = []

    for c in cases:
        for fmt in a.formats.split(","):
            r = run_one(c, fmt, a, cflags, a.out)

            results.append(r)

            print("%-20s %-6s %-9s %-7s %6.1fs  reply ~%5d tok  lost %d/%d  %s" % (
                r["id"], fmt, r["outcome"], "correct" if r["correct"] else "WRONG",
                r["seconds"], r["reply_tokens_est"], r["lines_lost"],
                r["max_lost"], r["cause"]), flush=True)

            with open(os.path.join(a.out, "results.json"), "w") as f:
                json.dump(results, f, indent=1)

    print()

    for fmt in a.formats.split(","):
        mine = [r for r in results if r["format"] == fmt]

        if not mine: continue

        print("%-6s applied %2d/%d   correct %2d/%d   median %6.1fs   "
              "total %7.1fs   median reply ~%d tok" % (
                  fmt,
                  sum(r["outcome"] == "applied" for r in mine), len(mine),
                  sum(r["correct"] for r in mine), len(mine),
                  statistics.median(r["seconds"] for r in mine),
                  sum(r["seconds"] for r in mine),
                  statistics.median(r["reply_tokens_est"] for r in mine)))

        causes = {}

        for r in mine:
            if r["cause"]: causes[r["cause"]] = causes.get(r["cause"], 0) + 1

        for k, n in sorted(causes.items(), key=lambda x: -x[1]):
            print("         %2d  %s" % (n, k))


if __name__ == "__main__":
    main()
