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

"""What a hunk reply refused for its filename would have done with the right one.

    tools/edit-formats/reapply.py /tmp/formats [/tmp/formats-more ...]

**This is not jcode.** It re-applies the SEARCH/REPLACE blocks a model sent
offline, with the real filename in place of the one that was refused, and
judges the result with run.py's own checks. It mirrors jcode's rule as
written in `apply()` -- blocks in order, each SEARCH matched exactly once in
the file as it stands at that point, and one bad block refusing the whole
edit -- and its parsing mirrors `hunks_in`. It answers one question the run
cannot: whether the hunks were *right*, behind a name that was wrong.
"""

import json
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run


def hunks_of(text):
    """The SEARCH/REPLACE blocks in a reply, parsed the way hunks_in parses."""
    out = []
    lines = text.splitlines()
    i = 0

    while i < len(lines):
        if not lines[i].strip().startswith("<<<<<<<"):
            i += 1
            continue

        i += 1
        search, replace, in_replace, closed = "", "", False, False

        while i < len(lines):
            t = lines[i].strip()

            if not in_replace and t == "=======":
                in_replace = True
            elif t.startswith(">>>>>>>"):
                closed = True
                i += 1
                break
            elif in_replace:
                replace += lines[i] + "\n"
            else:
                search += lines[i] + "\n"

            i += 1

        if closed: out.append((search, replace))

    return out


def apply_hunks(content, hunks):
    """jcode's rule: in order, each exactly once, all or nothing."""
    for search, replace in hunks:
        if not search: return None, "a search block with nothing to look for"

        at = content.find(search)

        if at < 0: return None, "a search block does not match"

        if content.find(search, at + 1) >= 0:
            return None, "a search block matches more than once"

        content = content[:at] + replace + content[at + len(search):]

    return content, ""


def main():
    cases = {c["id"]: c for c in
             json.load(open(os.path.join(run.HERE, "cases.json")))}

    import subprocess

    cflags = subprocess.run(["pkg-config", "--cflags", "json-c"], text=True,
                            capture_output=True).stdout.split()

    build = os.path.join(run.REPO, "build")

    for d in sys.argv[1:]:
        for r in json.load(open(os.path.join(d, "results.json"))):
            # **Any refusal about the name, not only the placeholder.** The
            # model also writes `path/to/quant.hh` -- the right basename behind
            # the prompt's prefix -- and the rescue misses that too when the
            # file sent was `jlib/ai/quant.hh`. Either way the name was wrong
            # and the blocks were never looked at, which is what this asks.
            c = cases[r["id"]]

            named = re.match(r"jcode: refused (\S+):", r.get("refused", ""))

            if r["format"] != "hunks" or not named or named.group(1) == c["file"]:
                continue
            reply = open(os.path.join(d, r["id"] + "-hunks.out")).read()
            hunks = hunks_of(reply)

            if not hunks:
                print("%-20s no SEARCH/REPLACE blocks at all -- the model "
                      "answered with a whole file" % r["id"])
                continue

            before = open(os.path.join(run.REPO, c["file"]),
                          encoding="utf-8").read()

            after, why = apply_hunks(before, hunks)

            if after is None:
                print("%-20s %d block(s), refused: %s" % (r["id"], len(hunks),
                                                          why))
                continue

            root = tempfile.mkdtemp(prefix="edit-formats-reapply-")

            try:
                dst = os.path.join(root, c["file"])

                os.makedirs(os.path.dirname(dst))

                with open(dst, "w", encoding="utf-8") as f: f.write(after)

                good, _ = run.syntax(root, c["file"], build, cflags)
            finally:
                shutil.rmtree(root, ignore_errors=True)

            missing = [p for p in c["must"]
                       if len(re.findall(p, after)) <= len(re.findall(p, before))]
            present = [p for p in c["must_not"] if re.search(p, after)]
            lost = run.lines_lost(before, after)

            correct = not missing and not present and lost <= c["max_lost"] and good

            print("%-20s %d block(s), %s  (lost %d/%d%s%s)" % (
                r["id"], len(hunks), "CORRECT" if correct else "wrong",
                lost, c["max_lost"],
                ", missing %s" % missing if missing else "",
                ", does not compile" if not good else ""))


if __name__ == "__main__":
    main()
