#!/bin/bash
#
# Point every submodule's push at a fork, so no push can reach upstream.
#
# Why this is a script and not just .gitmodules: git reads "url" from
# .gitmodules but not "pushurl", so fetch can be configured for everyone and
# push cannot.  A fresh clone therefore has every read-only submodule pushing
# straight at its upstream, which is one "git push" in the wrong directory
# away from an unwanted commit on rtems/rtos/rtems or esphome/esphome.  This
# was not hypothetical: a push in src/esphome went to origin, and origin was
# esphome/esphome.  It failed only because the URL was https and there were
# no credentials.
#
# Run once after cloning:  scripts/setup-remotes.sh
# Safe to re-run; it only sets values.
#
# Three cases, and which one a submodule gets depends on whether a fork
# exists and whether it has the pinned commit:
#
#   fork has the pin      fetch and push both go to the fork
#   fork is stale/absent  fetch stays on upstream so the pin resolves,
#                         push is redirected to the fork
#   no fork at all        push is pointed at a path that does not exist, so
#                         it fails with a message saying why
#
# Checking the pin matters.  TheSamPrice/rtems exists but its head is months
# behind, so making it the fetch URL would break "git submodule update" for
# anyone cloning fresh -- the pinned commit is not in it.

set -u

top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top" || exit 1

set_push() {
  # $1 submodule path, $2 push URL
  if [ -d "$1/.git" ] || [ -f "$1/.git" ]; then
    git -C "$1" remote set-url --push origin "$2" && echo "  $1 -> $2"
  else
    echo "  $1 (not checked out, skipped)"
  fi
}

echo "Redirecting submodule pushes to forks:"

# Fetch and push both on the fork; .gitmodules already names the fork, so
# these are here only to repair a clone whose origin was changed by hand.
set_push src/esphome    git@github.com:thesamprice/esphome.git
set_push src/rtems-lwip git@github.com:thesamprice/rtems-lwip.git
set_push src/esp-qemu   git@github.com:thesamprice/qemu.git

# Fetch from upstream because the fork does not carry the pinned commit;
# push to the fork so an accidental push cannot land upstream.
set_push src/rtems      https://gitlab.rtems.org/TheSamPrice/rtems.git
set_push src/rsb        https://gitlab.rtems.org/TheSamPrice/rtems-source-builder.git
set_push src/rtems-libbsd https://gitlab.rtems.org/TheSamPrice/rtems-libbsd.git

# Vendored, read only, and no fork exists.  A path that is not a repository
# fails with the reason in the error text.
set_push src/ArduinoJson "/nonexistent/ArduinoJson-is-vendored-read-only-there-is-no-fork-to-push-to"
set_push src/osal        "/nonexistent/osal-is-vendored-read-only-there-is-no-fork-to-push-to"

echo
echo "Push targets now:"
git submodule foreach --quiet \
  'printf "  %-22s %s\n" "$name" "$(git remote get-url --push origin 2>/dev/null)"'
