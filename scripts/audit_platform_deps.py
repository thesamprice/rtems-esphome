#!/usr/bin/env python3
"""Inventory ESPHome's platform dependencies and classify every one of them.

Usage:
    scripts/audit_platform_deps.py              write audit/ and print a summary
    scripts/audit_platform_deps.py --check      regenerate and fail if it differs
    scripts/audit_platform_deps.py --tree DIR   audit a tree other than src/esphome

Why this exists
---------------
"ESPHome contains direct FreeRTOS calls" and "the ESPHome core requires
FreeRTOS" are very different statements, and only the second one would make
this port intractable.  Deciding which is true is not a matter of opinion, so
this counts.

What it does NOT do is judge whether a dependency is hard to replace.  It
locates and classifies; the effort estimate is a human's job, informed by this.

Classification
--------------
Every hit lands in exactly one bucket, decided by where it is, not by what it
says:

  platform-backend  Inside a platform's own implementation -- components/esp32,
                    components/host, core/wake/wake_freertos.* and so on.
                    Expected, and the shape an RTEMS backend copies.  Not work.

  seam              In a file whose job is to dispatch to a platform backend or
                    to declare an interface.  A hit here is dispatch, not
                    implementation.  Also not work -- but a seam that grows real
                    implementation is a regression, so they are listed
                    separately rather than folded into platform-backend.

  core-blocker      Anywhere else under esphome/core.  This is the set that has
                    to be dealt with before an RTEMS image can run at all, and
                    it is the number this whole exercise exists to produce.

  component-local   Under esphome/components, outside a platform backend.  These
                    do not block a core port; they decide which components an
                    RTEMS build can offer.  A support manifest, not a blocker
                    list.

  python            Build-time Python -- validators, code generation, platform
                    admission.  Ported separately from the C++ runtime.

Determinism
-----------
Output is sorted and contains no timestamps, paths outside the tree, or hit
counts that depend on filesystem order, so `--check` is meaningful in CI and a
diff in audit/ means the dependency surface actually moved.
"""

import argparse
import csv
import json
import os
import re
import sys

# --- what counts as a platform dependency -----------------------------------
#
# Each rule is (family, name, regex).  Names are stable identifiers; they end up
# in the JSON and in CI diffs, so renaming one is a visible change.
#
# These deliberately match *usage*, not just includes.  A file that includes
# freertos/task.h and calls nothing is less interesting than one that calls
# xTaskCreate through a macro, and the second would be invisible to an
# include-only scan.

RULES = [
    # FreeRTOS
    ("freertos", "include", r'#\s*include\s*[<"]freertos/'),
    ("freertos", "task-create", r"\bxTaskCreate(?:Static|Pinned|StaticPinned)?[A-Za-z]*\s*\("),
    ("freertos", "task-delay", r"\bvTaskDelay\w*\s*\("),
    ("freertos", "task-notify", r"\b(?:xTaskNotify|ulTaskNotify|vTaskNotify)\w*\s*\("),
    ("freertos", "semaphore", r"\bx?(?:Semaphore(?:Create|Take|Give)\w*|SemaphoreHandle_t)\b"),
    ("freertos", "queue", r"\bx?Queue(?:Create|Send|Receive|Reset|Peek|Handle_t)\w*\b"),
    ("freertos", "isr-context", r"\bxPortInIsrContext\s*\("),
    ("freertos", "yield", r"\b(?:portYIELD|vPortYield|taskYIELD)\w*"),
    ("freertos", "tick", r"\b(?:xTaskGetTickCount|pdMS_TO_TICKS|configTICK_RATE_HZ|portTICK_[A-Z_]+)\b"),
    ("freertos", "critical", r"\b(?:portENTER_CRITICAL|portEXIT_CRITICAL|taskENTER_CRITICAL|taskEXIT_CRITICAL)\w*"),
    ("freertos", "handle-type", r"\bTaskHandle_t\b"),
    # lwIP
    ("lwip", "include", r'#\s*include\s*[<"]lwip/'),
    ("lwip", "dns", r"\bdns_gethostbyname\w*\s*\("),
    ("lwip", "addr-type", r"\b(?:ip_addr_t|ip4_addr_t|ip6_addr_t)\b"),
    ("lwip", "netif", r"\bnetif_\w+\s*\(|\bstruct\s+netif\b"),
    ("lwip", "tcpip", r"\btcpip_\w+\s*\("),
    ("lwip", "raw-api", r"\b(?:tcp_new|tcp_bind|tcp_listen|tcp_write|udp_new|udp_sendto|pbuf_\w+)\s*\("),
    ("lwip", "sockets", r"\blwip_\w+\s*\("),
    # ESP-IDF
    ("esp-idf", "include", r'#\s*include\s*[<"](?:esp_|driver/|soc/|hal/|nvs|esp32|spi_flash/|bootloader_)'),
    ("esp-idf", "timer", r"\besp_timer_\w+\s*\("),
    ("esp-idf", "watchdog", r"\besp_task_wdt_\w+\s*\("),
    ("esp-idf", "cpu", r"\besp_cpu_\w+\s*\("),
    ("esp-idf", "nvs", r"\bnvs_\w+\s*\("),
    ("esp-idf", "wifi", r"\besp_wifi_\w+\s*\(|\besp_netif_\w+\s*\("),
    ("esp-idf", "gpio", r"\bgpio_(?:set_level|get_level|config|isr|install|reset_pin|hold)\w*\s*\("),
    ("esp-idf", "i2c-spi", r"\b(?:i2c_(?:master|param|driver|cmd)\w*|spi_(?:bus|device)\w*)\s*\("),
    ("esp-idf", "adc", r"\badc_(?:oneshot|cali|continuous)\w*\s*\("),
    ("esp-idf", "heap-caps", r"\bheap_caps_\w+\s*\(|\bMALLOC_CAP_\w+"),
    ("esp-idf", "iram-attr", r"\b(?:IRAM_ATTR|DRAM_ATTR|RTC_DATA_ATTR)\b"),
    ("esp-idf", "err-type", r"\besp_err_t\b|\bESP_(?:OK|FAIL|ERR_[A-Z_]+)\b"),
    ("esp-idf", "log", r"\bESP_LOG[A-Z]\s*\("),
    # Arduino
    ("arduino", "include", r'#\s*include\s*[<"]Arduino\.h[">]'),
    ("arduino", "guard", r"\bUSE_ARDUINO\b"),
    ("arduino", "ipaddress", r"\bIPAddress\b"),
    ("arduino", "api", r"\b(?:digitalWrite|digitalRead|pinMode|analogRead|analogWrite)\s*\("),
]

COMPILED = [(f, n, re.compile(p)) for f, n, p in RULES]

# Platform backends.  A dependency inside one of these is the platform doing its
# job.  An RTEMS port adds a sibling; it does not remove these.
PLATFORM_COMPONENTS = ("esp32", "esp8266", "libretiny", "rp2", "host", "zephyr")

# core/wake/ is per-platform by design: wake.h dispatches, wake_<platform>.*
# implement.  Treat the implementations as platform backends.
WAKE_BACKEND = re.compile(r"esphome/core/wake/wake_(?!\.)\w+\.(?:h|cpp)$")

# Dispatchers and interfaces.  A hit in one of these should be a #include chosen
# by a USE_<platform> define, or an interface declaration -- not implementation.
SEAM_FILES = {
    "esphome/core/hal.h",
    "esphome/core/wake.h",
    "esphome/core/gpio.h",
    "esphome/core/preferences.h",
    "esphome/components/socket/socket.h",
    "esphome/components/socket/headers.h",
    "esphome/components/uart/uart_component.h",
    "esphome/components/i2c/i2c_bus.h",
}

SOURCE_EXT = (".h", ".cpp", ".c", ".tcc", ".hpp")
PYTHON_EXT = (".py",)


def classify(relpath: str) -> str:
    if relpath in SEAM_FILES:
        return "seam"
    if relpath.endswith(PYTHON_EXT):
        return "python"
    if WAKE_BACKEND.search(relpath):
        return "platform-backend"
    parts = relpath.split("/")
    if len(parts) > 2 and parts[0] == "esphome" and parts[1] == "components":
        if parts[2] in PLATFORM_COMPONENTS:
            return "platform-backend"
        return "component-local"
    if len(parts) > 1 and parts[0] == "esphome" and parts[1] == "core":
        return "core-blocker"
    return "component-local"


def scan(tree: str):
    hits = []
    root_dir = os.path.join(tree, "esphome")
    if not os.path.isdir(root_dir):
        sys.exit(f"{sys.argv[0]}: no esphome/ under {tree}; is the submodule fetched?")
    for root, dirs, files in os.walk(root_dir):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith(SOURCE_EXT + PYTHON_EXT):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, tree)
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    lines = fh.read().splitlines()
            except OSError:
                continue
            bucket = classify(rel)
            for lineno, line in enumerate(lines, 1):
                # Skip obvious comment-only lines: a mention in prose is not a
                # dependency, and counting them would inflate every number here.
                stripped = line.lstrip()
                if stripped.startswith(("//", "*", "/*", "#  ")):
                    continue
                for family, rule, pattern in COMPILED:
                    if pattern.search(line):
                        hits.append(
                            {
                                "file": rel,
                                "line": lineno,
                                "family": family,
                                "rule": rule,
                                "bucket": bucket,
                                "text": line.strip()[:160],
                            }
                        )
    hits.sort(key=lambda h: (h["file"], h["line"], h["family"], h["rule"]))
    return hits


def summarise(hits):
    buckets, families, core_files = {}, {}, {}
    for h in hits:
        buckets[h["bucket"]] = buckets.get(h["bucket"], 0) + 1
        key = (h["family"], h["bucket"])
        families[key] = families.get(key, 0) + 1
        if h["bucket"] == "core-blocker":
            core_files.setdefault(h["file"], {})
            core_files[h["file"]][h["family"]] = (
                core_files[h["file"]].get(h["family"], 0) + 1
            )
    return buckets, families, core_files


def markdown(hits, buckets, families, core_files) -> str:
    out = []
    w = out.append
    w("# ESPHome platform dependency audit\n")
    w("Generated by `scripts/audit_platform_deps.py`. Do not edit by hand;")
    w("regenerate and commit the diff, which is the point of tracking it.\n")
    w("Counts are *hits*, meaning matching lines, not distinct symbols. The")
    w("number that matters is **core-blocker**: dependencies in `esphome/core`")
    w("that are not already behind a platform seam, and so have to be dealt")
    w("with before an RTEMS image runs at all.\n")

    w("## Where the dependencies are\n")
    w("| bucket | hits | meaning |")
    w("|---|---:|---|")
    meanings = {
        "core-blocker": "**must be addressed to boot** — core, not behind a seam",
        "component-local": "decides which components an RTEMS build can offer",
        "platform-backend": "a platform doing its job; an RTEMS port adds a sibling",
        "seam": "dispatch or interface declaration, not implementation",
        "python": "build-time validation and code generation",
    }
    for b in ("core-blocker", "seam", "platform-backend", "component-local", "python"):
        w(f"| {b} | {buckets.get(b, 0)} | {meanings[b]} |")
    w("")

    w("## The core blockers, by file\n")
    if not core_files:
        w("None. Every platform dependency in `esphome/core` is behind a seam.\n")
    else:
        w("These are the files an RTEMS backend has to answer for. A file")
        w("appearing here is not necessarily hard — `millis_internal.h` is a tick")
        w("fast path with an obvious replacement — but it is not optional.\n")
        w("| file | hits | families |")
        w("|---|---:|---|")
        for f in sorted(core_files, key=lambda k: (-sum(core_files[k].values()), k)):
            fam = ", ".join(
                f"{k} ({v})" for k, v in sorted(core_files[f].items())
            )
            w(f"| `{f}` | {sum(core_files[f].values())} | {fam} |")
        w("")

    w("## Families by bucket\n")
    w("| family | core-blocker | seam | platform-backend | component-local | python |")
    w("|---|---:|---:|---:|---:|---:|")
    for fam in ("freertos", "lwip", "esp-idf", "arduino"):
        row = [
            str(families.get((fam, b), 0))
            for b in (
                "core-blocker",
                "seam",
                "platform-backend",
                "component-local",
                "python",
            )
        ]
        w(f"| {fam} | " + " | ".join(row) + " |")
    w("")

    w("## Components with the most platform coupling\n")
    w("Not a blocker list. This is what a support manifest gets built from:")
    w("a component near the top is one an RTEMS build should refuse cleanly")
    w("rather than half-support.\n")
    per = {}
    for h in hits:
        if h["bucket"] != "component-local":
            continue
        comp = "/".join(h["file"].split("/")[:3])
        per[comp] = per.get(comp, 0) + 1
    w("| component | hits |")
    w("|---|---:|")
    for comp, n in sorted(per.items(), key=lambda kv: (-kv[1], kv[0]))[:25]:
        w(f"| `{comp}` | {n} |")
    w("")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="src/esphome")
    ap.add_argument("--out", default="audit")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    top = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(top)

    hits = scan(args.tree)
    buckets, families, core_files = summarise(hits)

    rev = ""
    head = os.path.join(args.tree, ".git")
    if os.path.exists(head):
        rev = os.popen(f"git -C {args.tree} rev-parse HEAD 2>/dev/null").read().strip()

    payload = {
        "tree": args.tree,
        "revision": rev,
        "rules": [{"family": f, "rule": n, "pattern": p} for f, n, p in RULES],
        "totals": dict(sorted(buckets.items())),
        "hits": hits,
    }

    os.makedirs(args.out, exist_ok=True)
    files = {
        os.path.join(args.out, "platform-deps.json"): json.dumps(
            payload, indent=2, sort_keys=False
        )
        + "\n",
        os.path.join(args.out, "platform-deps.md"): markdown(
            hits, buckets, families, core_files
        ),
    }

    csv_path = os.path.join(args.out, "platform-deps.csv")
    rows = ["file,line,family,rule,bucket\n"]
    for h in hits:
        rows.append(f'{h["file"]},{h["line"]},{h["family"]},{h["rule"]},{h["bucket"]}\n')
    files[csv_path] = "".join(rows)

    if args.check:
        differs = []
        for path, content in files.items():
            try:
                with open(path, encoding="utf-8") as fh:
                    if fh.read() != content:
                        differs.append(path)
            except OSError:
                differs.append(path)
        if differs:
            print("dependency surface has moved; regenerate audit/:", file=sys.stderr)
            for d in sorted(differs):
                print(f"  {d}", file=sys.stderr)
            return 1
        print("audit up to date")
        return 0

    for path, content in files.items():
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)

    print(f"{len(hits)} hits in {args.tree} at {rev[:12] or 'unknown'}")
    for b in ("core-blocker", "seam", "platform-backend", "component-local", "python"):
        print(f"  {b:17} {buckets.get(b, 0)}")
    print(f"\ncore blockers in {len(core_files)} files:")
    for f in sorted(core_files, key=lambda k: (-sum(core_files[k].values()), k)):
        print(f"  {sum(core_files[f].values()):4}  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
