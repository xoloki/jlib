# -*- mode: Python -*-
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
#
# Regenerates the tables in unicode.hh:
#
#     python3 unicode.py > /tmp/tables && $EDITOR unicode.hh
#
# It writes the three tables and nothing else -- the file's comments and its
# in_ranges() are hand-written and stay.  This exists because a generated
# table with no generator is a table nobody can check: the version it came
# from is a comment, and the comment is the only thing that says so.
#
# unicodedata is Python's own copy of UnicodeData.txt, which is the same
# source llama.cpp generates its tables from.


import unicodedata

def ranges(keep):
    out, lo, prev = [], None, None
    for cp in range(0x110000):
        if keep(cp):
            if lo is None: lo = cp
            prev = cp
        elif lo is not None:
            out.append((lo, prev)); lo = None
    if lo is not None: out.append((lo, prev))
    return out

WS = set(range(0x09,0x0e)) | {0x20,0x85,0xa0,0x1680} | set(range(0x2000,0x200b)) \
   | {0x2028,0x2029,0x202f,0x205f,0x3000}          # the White_Space property

TABLES = [("LETTER", lambda cp: unicodedata.category(chr(cp)).startswith("L")),
          ("NUMBER", lambda cp: unicodedata.category(chr(cp)).startswith("N")),
          ("WHITESPACE", lambda cp: cp in WS)]

print("// Unicode " + unicodedata.unidata_version)
for name, keep in TABLES:
    rs = ranges(keep)
    print("static const range %s[] = {" % name)
    for i in range(0, len(rs), 4):
        print("    " + " ".join("{0x%05X,0x%05X}," % r for r in rs[i:i+4]))
    print("};\nstatic const std::size_t %s_COUNT = %d;\n" % (name, len(rs)))
