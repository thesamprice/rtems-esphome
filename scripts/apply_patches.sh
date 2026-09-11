#!/bin/bash
#
# Apply the local patches under patches/ to the submodules in src/.
#
# Layout:  patches/<module>/*.patch  applies to  src/<module>
#
# The patches carry fixes that are not upstream yet.  Keeping them as files
# rather than as commits in the submodules means the submodule pins stay on
# upstream commits, and "git status" in the submodule shows exactly the local
# delta.  The pin is what makes a result attributable, so it stays on something
# upstream can be asked about.
#
# This is what .github/Dockerfile.toolchain runs before building anything: the
# patches are what make the BSP's clock correct and its headers installable, so
# an image built without them bakes in the bugs.
#
# Usage:
#   scripts/apply_patches.sh                 apply every patch
#   scripts/apply_patches.sh rsb             apply only patches/rsb/
#   scripts/apply_patches.sh -c              check only, change nothing
#   scripts/apply_patches.sh -R              reverse (un-apply)
#   scripts/apply_patches.sh -l              list what would be applied, and where
#
# Applying is idempotent: a patch that is already applied is reported and
# skipped, so this is safe to re-run and safe to put in a build script.
#
# Exit status is 0 only if every selected patch ended up applied (or, with -R,
# un-applied).

set -u

# This script lives in scripts/, so the tree top is one level up.
top="$(cd "$(dirname "$0")/.." && pwd)"
mode=apply
rc=0

while getopts ":cRlh" opt; do
    case "$opt" in
        c) mode=check ;;
        R) mode=reverse ;;
        l) mode=list ;;
        h) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        \?) echo "$0: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

# Selected modules: the arguments, or every directory under patches/.
explicit=0
if [ $# -gt 0 ]; then
    explicit=1
    modules="$*"
else
    modules=
    for d in "$top"/patches/*/; do
        [ -d "$d" ] || continue
        modules="$modules $(basename "$d")"
    done
fi

# Finding nothing is an error, not a quiet success.  A build script that calls
# this and gets a silent zero would take "no patches applied" for "all patches
# applied", which is the one failure mode that must not be quiet.
if [ -z "$(echo $modules)" ]; then
    echo "$0: no patch directories found under $top/patches" >&2
    exit 1
fi

for module in $modules; do
    pdir="$top/patches/$module"
    target="$top/src/$module"

    if [ ! -d "$pdir" ]; then
        echo "$module: no patches/$module directory" >&2
        rc=1
        continue
    fi

    if [ ! -d "$target/.git" ] && [ ! -f "$target/.git" ]; then
        # A submodule declared "update = none" is opt-in: the tree is meant to
        # work without it, so having no checkout to patch is the normal state,
        # not a failure.  Skipping keeps a plain "apply everything" green on a
        # default clone, which matters because a build script calls it and
        # treats non-zero as "the patches are not in".
        #
        # Naming the module on the command line overrides that.  Then the
        # caller asked for this one specifically and a quiet skip would answer
        # a different question than the one asked.
        update=$(git -C "$top" config -f .gitmodules \
                 --get "submodule.src/$module.update" 2>/dev/null)
        if [ "$explicit" = 0 ] && [ "$update" = none ]; then
            echo "$module: not fetched, skipped (update = none)"
            continue
        fi
        echo "$module: src/$module is not a git checkout" >&2
        echo "$module:   git submodule update --init --depth=1 src/$module" >&2
        rc=1
        continue
    fi

    # Sorted, so a numeric prefix can force an order when one is needed.
    for patch in "$pdir"/*.patch; do
        [ -e "$patch" ] || continue
        name="$module/$(basename "$patch")"

        if [ "$mode" = list ]; then
            echo "$name -> src/$module"
            continue
        fi

        # Reverse-check tells us whether it is already applied.  Do this before
        # the forward check, because a partially overlapping patch can satisfy
        # neither and we want the clearer message.
        if git -C "$target" apply --reverse --check "$patch" 2>/dev/null; then
            applied=yes
        else
            applied=no
        fi

        case "$mode:$applied" in
            check:yes)   echo "  applied     $name" ;;
            check:no)
                if git -C "$target" apply --check "$patch" 2>/dev/null; then
                    echo "  applicable  $name"
                else
                    echo "  DOES NOT APPLY  $name" >&2
                    rc=1
                fi
                ;;
            apply:yes)   echo "  already applied  $name" ;;
            apply:no)
                if git -C "$target" apply "$patch" 2>/dev/null; then
                    echo "  applied          $name"
                else
                    echo "  FAILED TO APPLY  $name" >&2
                    rc=1
                fi
                ;;
            reverse:no)  echo "  not applied      $name" ;;
            reverse:yes)
                if git -C "$target" apply --reverse "$patch" 2>/dev/null; then
                    echo "  reversed         $name"
                else
                    echo "  FAILED TO REVERSE  $name" >&2
                    rc=1
                fi
                ;;
        esac
    done
done

exit $rc
