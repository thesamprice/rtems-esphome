#!/usr/bin/env python3
"""Check that every ESPHome component either works on RTEMS or says it does not.

Usage:
    scripts/audit_component_gating.py [-j N] [--tree DIR] [--out FILE] [names...]

Why this exists
---------------
Rule 11: an unsupported component must fail validation or compilation clearly,
never behave as a partial stub that appears to work.  A component that is
simply absent from this platform is fine.  A component that raises KeyError,
AttributeError or NotImplementedError from inside ESPHome's own code is not --
the user cannot tell whether they made a mistake, and neither can a reviewer.

That failure mode is not hypothetical here: #37 and #38 are both instances of
it, found one at a time.  This finds them all at once.

What it does
------------
For each component, write a minimal configuration naming it and run
`esphome config`.  Only validation runs; nothing is compiled.  The outcome is
classified by what came back, not by what the component claims:

  accepted    validation passed and code generation ran, so the component
              really does work here rather than merely claiming to
  gated       refused with a platform message -- the rule 11 outcome
  needs-args  refused for missing or invalid keys, which is this probe's
              fault rather than the component's, and says nothing either way
  unclear     a Python traceback, or an exception type that is not a
              validation error.  These are the bugs.

Validation alone is not enough, and that is the whole reason for the second
stage.  `wifi:` used to pass validation on RTEMS and then fail code generation
with KeyError: 'power_save_mode', because CONF_POWER_SAVE_MODE is defaulted
only by platform-specific branches and to_code() reads it unconditionally.  An
audit that stopped at `esphome config` would have called that "accepted" and
reported a clean sheet.

So anything that validates is then run through `esphome compile`, which does
code generation before it builds.  Reaching "Compiling app" means codegen
succeeded, and the run is killed there -- building 700 firmwares is not the
question being asked.

Know the mesh size of this net
------------------------------
A component is probed by naming it with no options.  Anything that needs a
required key therefore lands in "needs-args" and is NOT checked for the codegen
failure above -- the probe never gets far enough to look.

`wifi:` is exactly that case, and it is worth being blunt about: the KeyError
described above was found by hand, not by this script, because bare `wifi:`
fails validation on the missing ssid.  Covering components like that needs a
real fixture per component, which is a much larger undertaking than this.

So a clean run means "nothing in the probeable set fails unclearly".  It does
not mean the tree is clean.

Exit status is non-zero if anything lands in "unclear", so this can gate CI.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

BSP = "riscv/esp32c3db"

# A component named alone is usually not a complete configuration.  These
# patterns mean "you did not give me enough", which is the probe's doing.
def first_reason(out):
    """The first line that looks like ESPHome explaining itself."""
    skip = ("INFO ", "WARNING ", "Failed config", "")
    for line in out.splitlines():
        s = line.strip()
        if s and not any(line.startswith(p) for p in skip) and not s.startswith("#"):
            return s[:160]
    return ""


NEEDS_ARGS = re.compile(
    r"required key not provided"
    r"|is a required option"
    r"|expected a dictionary"
    r"|Component (?:\S+) cannot be loaded"
    r"|does not support this configuration"
    r"|Unable to find action|Unable to find condition"
    r"|value for dictionary value|expected .* but got"
    r"|is required on"
    r"|At least one platform must be specified"
    r"|must be specified|cannot be empty",
    re.I,
)


def codegen(esphome, component, path):
    """Did code generation run, or did it raise?

    Killed as soon as the build starts: the question is whether to_code()
    works, not whether 700 firmwares link.
    """
    try:
        r = subprocess.run(
            [esphome, "compile", path],
            capture_output=True, text=True, timeout=300,
        )
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode() if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        out += (exc.stderr or b"").decode() if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        if "Compiling app" in out or "Generating C++ source" in out:
            return component, "accepted", ""
        return component, "unclear", "code generation timed out"
    if "Compiling app" in out:
        return component, "accepted", ""
    m = UNCLEAR.search(out)
    if m:
        line = next((l.strip() for l in out.splitlines() if m.group(0) in l), m.group(0))
        return component, "unclear", f"validates, then fails codegen: {line[:160]}"
    # Codegen did not obviously run and nothing raised: most likely the build
    # backend refused for a reason of its own, which is still a clear failure.
    return component, "refused-other", first_reason(out)

# The rule 11 outcome: refused because the platform is not supported.
GATED = re.compile(
    r"only available on"
    r"|not (?:yet )?supported on"
    r"|is not supported by"
    r"|requires (?:an? )?(?:esp32|esp8266|arduino|rp2|libretiny|host)"
    r"|Platform \S+ is not supported",
    re.I,
)

# A traceback, or an exception that escaped validation, is the failure this
# audit exists to find.
UNCLEAR = re.compile(
    r"Traceback \(most recent call last\)"
    r"|NotImplementedError"
    r"|AttributeError"
    r"|KeyError"
    r"|TypeError"
    r"|IndexError"
    r"|ERROR Unexpected exception",
)


def probe(esphome, component, tmpdir):
    # A name per component, because the codegen stage runs `esphome compile`
    # and that writes to .esphome/build/<name>.  A shared name means every
    # parallel worker races in one directory, which showed up as two or three
    # components changing verdict between otherwise identical runs.
    safe = re.sub(r"[^a-z0-9]+", "-", component.lower()).strip("-")
    body = (
        f"esphome:\n  name: gp-{safe}\n"
        f"rtems:\n  bsp: {BSP}\n"
        f"logger:\n  level: INFO\n"
        f"{component}:\n"
    )
    path = os.path.join(tmpdir, f"{component}.yaml")
    with open(path, "w") as fh:
        fh.write(body)
    try:
        r = subprocess.run(
            [esphome, "config", path],
            capture_output=True, text=True, timeout=180,
        )
    except subprocess.TimeoutExpired:
        return component, "unclear", "validation timed out"
    out = r.stdout + r.stderr
    if r.returncode == 0:
        return codegen(esphome, component, path)
    # Order matters: a traceback is unclear even if a gating message is also
    # present, because the traceback is what the user is left looking at.
    m = UNCLEAR.search(out)
    if m:
        line = next((l.strip() for l in out.splitlines()
                     if m.group(0) in l), m.group(0))
        return component, "unclear", line[:200]
    if GATED.search(out):
        return component, "gated", ""
    if NEEDS_ARGS.search(out):
        return component, "needs-args", ""
    # A refusal this audit does not recognise is still a refusal with a message,
    # which is the rule 11 outcome.  Calling it "unclear" would bury the real
    # tracebacks under noise, which is the opposite of the point.
    return component, "refused-other", first_reason(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--tree", default="src/esphome")
    ap.add_argument("--esphome", default=".venv/bin/esphome")
    ap.add_argument("--out", default="audit/component-gating.json")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()

    cdir = os.path.join(a.tree, "esphome", "components")
    names = a.names or sorted(
        c for c in os.listdir(cdir)
        if os.path.isdir(os.path.join(cdir, c))
        and os.path.exists(os.path.join(cdir, c, "__init__.py"))
    )

    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.j) as ex:
            futs = [ex.submit(probe, a.esphome, n, tmp) for n in names]
            for i, f in enumerate(concurrent.futures.as_completed(futs), 1):
                name, verdict, detail = f.result()
                results[name] = {"verdict": verdict, "detail": detail}
                if i % 50 == 0:
                    print(f"  {i}/{len(names)}", file=sys.stderr)

    counts = {}
    for v in results.values():
        counts[v["verdict"]] = counts.get(v["verdict"], 0) + 1

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as fh:
        json.dump({"bsp": BSP, "counts": counts, "components": results}, fh,
                  indent=1, sort_keys=True)

    for k in ("accepted", "gated", "needs-args", "refused-other", "unclear"):
        print(f"{counts.get(k, 0):5d}  {k}")

    unclear = sorted(n for n, v in results.items() if v["verdict"] == "unclear")
    if unclear:
        print(f"\n{len(unclear)} component(s) fail unclearly:")
        for n in unclear:
            print(f"  {n}: {results[n]['detail']}")
    print(f"\nwritten to {a.out}")
    return 1 if unclear else 0


if __name__ == "__main__":
    sys.exit(main())
