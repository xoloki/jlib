#!/bin/sh
#
# Refuse the mermaid GitHub will not render.
#
#     tools/check-mermaid.sh docs/*.md
#     make check-docs
#
# **Because there is no renderer on this machine.** GitHub renders these
# diagrams and nothing here does -- no node, no mmdc -- so a syntax error is
# found by a human opening the page, which is exactly what happened to
# docs/async.md:
#
#     S->>S: accept; caps and per-address checks
#
# Mermaid treats ";" as a statement separator, so that message split in two
# and the parser then wanted an arrow where the next line's "Note" was. The
# diagram did not render and the markdown around it did, so nothing looked
# broken from the shell.
#
# This checks the handful of things that have actually bitten rather than
# trying to be a parser. Add to it when something else does.
set -eu

bad=0

for f in "$@"; do
    [ -r "$f" ] || { echo "$f: cannot read" >&2; bad=1; continue; }

    awk -v file="$f" '
        /^```mermaid$/ { inblk = 1; next }
        /^```$/        { inblk = 0; next }

        !inblk { next }

        # A semicolon ends a statement, wherever it appears -- including
        # inside an HTML entity like &lt;, which is how a template parameter
        # in a node label breaks a diagram that looks fine.
        /;/ {
            printf "%s:%d: semicolon in mermaid (statement separator): %s\n",
                   file, NR, $0
            bad = 1
        }

        # Raw angle brackets in a label are the other half of the same
        # problem: escaping them reintroduces the semicolon, so write the
        # type in words instead.
        /\[[^]]*[<>][^]]*\]/ {
            printf "%s:%d: angle bracket in a node label: %s\n", file, NR, $0
            bad = 1
        }

        END { exit bad }
    ' "$f" || bad=1
done

[ "$bad" -eq 0 ] && echo "mermaid ok: $*"

exit "$bad"
