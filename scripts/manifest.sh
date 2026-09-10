#!/bin/bash
#
# Report what is checked out against what this repository records, and exit
# non-zero if they disagree.
#
# Usage:
#   scripts/manifest.sh           report, exit non-zero on any disagreement
#   scripts/manifest.sh -q        exit status only, no output
#   scripts/manifest.sh -j        JSON, for CI to consume
#
# The pin is what makes a result attributable to a configuration.  A test
# number, a footprint, a coverage figure -- none of them mean anything without
# knowing which revision of which input produced them.  This is the check that
# a claim and a tree actually correspond.
#
# A submodule that is not fetched is reported and is NOT a disagreement: every
# submodule here is declared "update = none" and a default clone has none of
# them.  What is a disagreement is a submodule that IS fetched and sits on a
# different commit than the one recorded, because then anything built from it
# is attributable to nothing.

set -u

top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top" || exit 2

quiet=no
json=no

while [ $# -gt 0 ]; do
    case "$1" in
        -q) quiet=yes; shift ;;
        -j) json=yes; shift ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "$0: unknown argument $1" >&2; exit 2 ;;
    esac
done

rc=0
first=yes

[ "$json" = yes ] && echo "["

# git ls-files -s prints "160000 <sha> 0<TAB><path>" for each gitlink.
git ls-files -s src/ | while read -r mode sha _stage path; do
    [ "$mode" = 160000 ] || continue

    name="${path#src/}"
    url=$(git config -f .gitmodules --get "submodule.$path.url" 2>/dev/null)
    branch=$(git config -f .gitmodules --get "submodule.$path.branch" 2>/dev/null)

    if [ -d "$path/.git" ] || [ -f "$path/.git" ]; then
        actual=$(git -C "$path" rev-parse HEAD 2>/dev/null)
        dirty=clean
        [ -n "$(git -C "$path" status --porcelain 2>/dev/null)" ] && dirty=dirty
        if [ "$actual" = "$sha" ]; then
            state=match
        else
            state=MISMATCH
            rc=1
        fi
    else
        actual=""
        dirty=""
        state=not-fetched
    fi

    if [ "$json" = yes ]; then
        [ "$first" = yes ] || echo ","
        first=no
        printf '  {"path": "%s", "recorded": "%s", "actual": "%s", "state": "%s", "worktree": "%s", "url": "%s", "branch": "%s"}' \
            "$path" "$sha" "$actual" "$state" "$dirty" "$url" "$branch"
    elif [ "$quiet" = no ]; then
        case "$state" in
            match)       printf '  %-16s %s  %s\n' "$name" "${sha:0:12}" "$dirty" ;;
            not-fetched) printf '  %-16s %s  not fetched\n' "$name" "${sha:0:12}" ;;
            MISMATCH)    printf '  %-16s %s  MISMATCH, checked out %s\n' \
                             "$name" "${sha:0:12}" "${actual:0:12}" >&2 ;;
        esac
    fi
done

[ "$json" = yes ] && { echo; echo "]"; }

# The loop above runs in a subshell because of the pipe, so its rc does not
# survive.  Recompute the verdict here rather than restructure the loop.
mismatch=0
for path in $(git ls-files -s src/ | awk '$1=="160000" {print $4}'); do
    sha=$(git ls-files -s "$path" | awk '{print $2}')
    if [ -d "$path/.git" ] || [ -f "$path/.git" ]; then
        actual=$(git -C "$path" rev-parse HEAD 2>/dev/null)
        [ "$actual" = "$sha" ] || mismatch=1
    fi
done

if [ "$mismatch" != 0 ]; then
    [ "$quiet" = no ] && echo "$0: a fetched submodule is not on its recorded commit" >&2
    exit 1
fi
exit 0
