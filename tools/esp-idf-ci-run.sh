#!/bin/bash
# Run an ESPHome/ESP-IDF firmware under Espressif QEMU and report PASS or FAIL.
#
# This is the reference lane: the A side of the A/B.  Its job is to notice when
# a change to common ESPHome code breaks the existing ESP-IDF backend, which
# CONTRIBUTING.md rule 7 requires of every refactor commit.  It is not a test of
# RTEMS and shares no code with the RTEMS runner beyond the idea.
#
# Usage:
#   tools/esp-idf-ci-run.sh [options] <build-dir>
#
#   <build-dir>   directory holding bootloader.bin, partitions.bin and
#                 firmware.bin -- for ESPHome that is
#                 tests/esp-idf-ci/.esphome/build/<name>/.pioenvs/<name>
#
# Options:
#   -q QEMU   qemu-system-riscv32   (default: $QEMU_ESP32C3 or src/esp-qemu/build/)
#   -o DIR    output dir            (default: ./esp-idf-ci-results.<timestamp>)
#   -t SECS   hard timeout          (default: 90)
#   -m TEXT   extra required marker, repeatable
#
# $QEMU_DATA_DIR, if set, is passed as -L.  QEMU finds pc-bios relative to its
# own build tree, so a binary copied somewhere else -- a CI artifact, an
# install -- cannot find the ESP32-C3 ROM and dies immediately with
# "ROM code binary not found".  Point this at the pc-bios directory in that
# case.
#
# Exit status is 0 only if every required marker was seen and no failure
# signature was.
#
# The flash layout
#   Unlike the RTEMS images, which are direct boot -- raw binary from offset 0,
#   ROM jumps straight into it -- an ESP-IDF image is a real boot chain: a
#   second stage bootloader at 0x0, a partition table at 0x8000, and the
#   application at 0x10000 where the table says it lives.  QEMU is handed one
#   flash file with all three placed in it.
#
#   Offsets are read from flasher_args.json when the build provides one, rather
#   than assumed, because a configuration that moves the app partition would
#   otherwise produce an image that boots the wrong thing or nothing at all.
#
# Why markers rather than "did it crash"
#   A firmware that reaches setup() and then wedges looks identical, on the
#   serial line, to one that is idling correctly.  The reference node prints
#   CI-MARKER lines from its own boot hook and from a periodic component, so a
#   pass means the scheduler ran, not merely that nothing exploded.

set -u

TOP="$(cd "$(dirname "$0")/.." && pwd)"
QEMU=${QEMU_ESP32C3:-$TOP/src/esp-qemu/build/qemu-system-riscv32}
OUT=""
TMO=90
FLASH_SIZE=${ESP_CI_FLASH_SIZE:-$((4 * 1024 * 1024))}
DATA_ARGS=""
[ -n "${QEMU_DATA_DIR:-}" ] && DATA_ARGS="-L ${QEMU_DATA_DIR}"

# Markers the firmware must print.  Both come from reference-node.yaml.
MARKERS=("CI-MARKER boot ok" "CI-MARKER scheduler ok")

# Signatures that mean stop now rather than wait out the timeout.  A panic is a
# result; making CI spend 90 seconds discovering it is waste.
FAILSIGS='Guru Meditation|Panic|panic|abort\(\)|assert failed|CORRUPT HEAP|StackCanary|rst:0x[0-9a-f]+ \(.*(PANIC|WDT|BROWN)'

while getopts "q:o:t:m:" opt; do
  case $opt in
    q) QEMU=$OPTARG;;
    o) OUT=$OPTARG;;
    t) TMO=$OPTARG;;
    m) MARKERS+=("$OPTARG");;
    *) exit 2;;
  esac
done
shift $((OPTIND - 1))

BUILD=${1:-}
[ -n "$BUILD" ] || { echo "error: no build directory given" >&2; exit 2; }
[ -d "$BUILD" ] || { echo "error: no such build directory: $BUILD" >&2; exit 2; }
[ -x "$QEMU" ] || { echo "error: QEMU not found: $QEMU" >&2; exit 2; }
"$QEMU" -M help 2>/dev/null | grep -q '^esp32c3 ' || {
  echo "error: $QEMU has no esp32c3 machine; it is not the Espressif fork" >&2
  exit 2
}

[ -n "$OUT" ] || OUT=esp-idf-ci-results.$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
log="$OUT/serial.log"
flash="$OUT/flash.bin"

# ---------------------------------------------------------------- flash image
# Prefer the offsets the build recorded.  flasher_args.json is what esptool is
# driven with, so it is the same answer a real flash would use.
args_json=""
for cand in "$BUILD/flasher_args.json" "$BUILD/../flasher_args.json"; do
  [ -f "$cand" ] && { args_json=$cand; break; }
done

# bs takes a plain byte count, not "1m".  That suffix is BSD's; GNU dd spells
# it "1M" and rejects the lowercase form, so this worked on a developer's Mac
# and produced a zero-length image in CI -- where QEMU then refused the drive
# and the harness reported a timeout, because nothing checked that the image
# had been built.
if ! dd if=/dev/zero of="$flash" bs=1048576 count=$((FLASH_SIZE / 1048576)) 2>"$OUT/dd.err"; then
  echo "error: could not create the $((FLASH_SIZE / 1048576))MB flash image" >&2
  cat "$OUT/dd.err" >&2
  exit 2
fi
rm -f "$OUT/dd.err"

place() {
  # place <offset-hex-or-dec> <file>
  local off=$1 file=$2
  [ -f "$file" ] || { echo "error: missing $file" >&2; return 1; }
  local dec=$(( off ))
  if ! dd if="$file" of="$flash" bs=1 seek="$dec" conv=notrunc 2>/dev/null; then
    echo "error: could not place $file at $dec in the flash image" >&2
    return 1
  fi
  printf '  %-14s %#010x  %s\n' "$(basename "$file")" "$dec" "$(wc -c < "$file" | tr -d ' ') bytes"
}

echo "assembling flash image"
if [ -n "$args_json" ]; then
  # flash_files maps offset -> path, relative to the json's directory.
  base=$(dirname "$args_json")
  while IFS=$'\t' read -r off rel; do
    [ -n "$off" ] || continue
    case "$rel" in /*) f=$rel;; *) f=$base/$rel;; esac
    place "$off" "$f" || exit 2
  done < <(python3 - "$args_json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for off, path in sorted(d.get("flash_files", {}).items(), key=lambda kv: int(kv[0], 16)):
    print(f"{off}\t{path}")
PY
)
else
  echo "  (no flasher_args.json; using the standard esp32c3 offsets)"
  place 0x0     "$BUILD/bootloader.bin" || exit 2
  place 0x8000  "$BUILD/partitions.bin" || exit 2
  place 0x10000 "$BUILD/firmware.bin"   || exit 2
fi

# Check the image before handing it to QEMU.  QEMU accepts only 2, 4, 8 and
# 16MB flash images and refuses anything else with a drive error -- which the
# run loop below cannot distinguish from a firmware that never booted, so it
# reports a timeout and the real cause goes in qemu.err where nobody looks.
# This lane has already lost a CI run to exactly that.
actual=$(wc -c < "$flash" | tr -d ' ')
if [ "$actual" != "$FLASH_SIZE" ]; then
  echo "error: flash image is $actual bytes, expected $FLASH_SIZE" >&2
  exit 2
fi

# ---------------------------------------------------------------- run
rm -f "$log"
"$QEMU" -M esp32c3 -display none -monitor none -no-reboot $DATA_ARGS \
  -serial file:"$log" \
  -drive file="$flash",if=mtd,format=raw \
  > "$OUT/qemu.out" 2> "$OUT/qemu.err" &
qpid=$!

verdict=TIMEOUT
for _ in $(seq 1 $((TMO * 2))); do
  if [ -s "$log" ]; then
    if grep -qE "$FAILSIGS" "$log" 2>/dev/null; then
      verdict=FAIL
      break
    fi
    seen=1
    for m in "${MARKERS[@]}"; do
      grep -qF "$m" "$log" 2>/dev/null || { seen=0; break; }
    done
    if [ "$seen" = 1 ]; then
      verdict=PASS
      break
    fi
  fi
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done

# QEMU does not exit on its own here: the firmware idles forever, which is the
# correct behaviour for a node that is working.
kill -9 $qpid 2>/dev/null
wait $qpid 2>/dev/null

# ---------------------------------------------------------------- report
{
  echo "verdict: $verdict"
  echo "markers:"
  for m in "${MARKERS[@]}"; do
    if grep -qF "$m" "$log" 2>/dev/null; then echo "  seen    $m"; else echo "  MISSING $m"; fi
  done
  if grep -qE "$FAILSIGS" "$log" 2>/dev/null; then
    echo "failure signature:"
    grep -nE "$FAILSIGS" "$log" | head -5 | sed 's/^/  /'
  fi
} | tee "$OUT/result.txt"

echo
echo "serial log: $log"
[ "$verdict" = PASS ] && exit 0
exit 1
