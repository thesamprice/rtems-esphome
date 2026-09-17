# Proposed upstream patches

Patches meant for a **different project's** upstream, kept here so they are not
lost between being written and being sent.

Nothing here is applied to anything. Each submodule in `src/` carries the
changes this port needs as commits on a branch of a fork, so there is no
patch-applying step at all -- see `.gitmodules`. These are submissions, and
the copy this port actually runs lives in the submodule.

`git format-patch` from the relevant branch is how one of these is produced
when it is ready to send.

## `lwip/`

For lwIP itself, at `git.savannah.nongnu.org/git/lwip.git`. Send to lwip-devel.

`0001-mdns-fix-probing-on-single-stack-netifs-in-dual-stac.patch` — the mDNS
responder never finishes probing on a dual-stack build whose interface holds
only an IPv4 address, so it never announces. Generated against master
`d08f4773ed` (2026-09-01), where the defect is still present. See
rtems-esphome#75 for how it was found, #79 for why it needs to go upstream
rather than be carried, and #80 for the review that shaped this version.

**Why it cannot simply live in rtems-lwip:** `src/apps/mdns/mdns.c` is listed in
that project's `file-import.json`, so the next re-import from upstream silently
overwrites any local edit — no failure at import time, and no sign until someone
notices a node is undiscoverable.
