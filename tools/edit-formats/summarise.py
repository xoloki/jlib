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

"""One table from several runs, later runs replacing earlier ones.

    tools/edit-formats/summarise.py /tmp/formats-1 /tmp/formats-2 ...

**Why runs are merged at all.** The first run of this lost its server partway
through -- a background task's time limit killed jserve -- and every case after
that "failed" in no time, which is a connection refused and not a result. The
lost cases were rerun in smaller batches. A result for the same case and format
in a later directory replaces the earlier one, so the order of the arguments is
the order of trust.
"""

import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run


def main():
    merged = {}

    for d in sys.argv[1:]:
        for r in json.load(open(os.path.join(d, "results.json"))):
            # Re-derived rather than trusted, so a result recorded under an
            # earlier version of the rule is labelled by the current one. The
            # stderr is still beside it.
            err = open(os.path.join(d, "%s-%s.err" % (r["id"], r["format"]))).read()

            if not r["correct"]:
                r["cause"] = run.cause(err, r["outcome"], r["lines_lost"],
                                       r["max_lost"], r["missing"],
                                       r["syntax_ok"])

            merged[(r["id"], r["format"])] = r

    order = []

    for d in sys.argv[1:]:
        for r in json.load(open(os.path.join(d, "results.json"))):
            if r["id"] not in order: order.append(r["id"])

    print("| case | hunks | whole |")
    print("| --- | --- | --- |")

    def cell(r):
        if not r: return "--"

        verdict = "**correct**" if r["correct"] else (r["cause"] or "wrong")

        return "%s, ~%d tok, %ds" % (verdict, r["reply_tokens_est"],
                                     round(r["seconds"]))

    for cid in order:
        print("| `%s` | %s | %s |" % (cid, cell(merged.get((cid, "hunks"))),
                                      cell(merged.get((cid, "whole")))))

    print()

    for fmt in ("hunks", "whole"):
        mine = [r for k, r in merged.items() if k[1] == fmt]

        if not mine: continue

        bad_connect = [r for r in mine if r["outcome"] == "failed" and
                       r["seconds"] < 1]

        print("%-6s correct %d/%d, applied %d/%d, median reply ~%d tok, "
              "median %ds%s" % (
                  fmt, sum(r["correct"] for r in mine), len(mine),
                  sum(r["outcome"] == "applied" for r in mine), len(mine),
                  statistics.median(r["reply_tokens_est"] for r in mine),
                  statistics.median(r["seconds"] for r in mine),
                  ("  -- %d still a lost connection, not a result" %
                   len(bad_connect)) if bad_connect else ""))

        # **Code destroyed, counted on its own.** An edit that was written and
        # took more of the file than it accounts for is the one failure that
        # costs the user their work, and it hides inside "applied".
        destroyed = [r for r in mine if r["outcome"] == "applied" and
                     r["lines_lost"] > r["max_lost"]]

        for r in destroyed:
            print("         wrote it and lost %d lines of the original: %s" %
                  (r["lines_lost"], r["id"]))


if __name__ == "__main__":
    main()
