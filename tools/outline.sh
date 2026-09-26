#!/bin/sh
#
# A header with the prose taken out: just the declarations.
#
#     tools/outline.sh jlib/net/http_server.hh
#     make outline FILE=jlib/net/http_server.hh
#
# **This exists instead of shortening the headers.** They are 66-71% comment,
# which makes the interface hard to see -- and the obvious fix, moving the
# prose to docs/, turns out to move almost nothing: measured on reactor.hh and
# http_server.hh, the rule "narrative leaves, reasoning stays" yields about 1%
# each, because nearly all of it is reasoning. The comments are load-bearing:
# HANGUP not meaning end of stream, why a token is not a descriptor, why the
# rate limiter runs before authentication. Deleting those to make a header
# skim better trades a permanent hazard for a temporary convenience, and the
# hazard is the thing that has actually caught mistakes.
#
# So the prose stays and the reader gets a filter. docs/ carries the map that
# spans files; this carries the shape of one.
#
# A reading aid, not a parser: it does not know a comment from the inside of a
# string literal, and headers here do not contain "/*" in one.
set -eu

f=${1:?usage: outline.sh <header>}

[ -r "$f" ] || { echo "cannot read $f" >&2; exit 1; }

sed -E 's://.*$::' "$f" \
    | perl -0pe 's:/\*.*?\*/::gs' \
    | grep -vE '^[[:space:]]*$'
