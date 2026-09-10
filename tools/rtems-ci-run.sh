#!/bin/bash
# Run an ESPHome/RTEMS image under Espressif QEMU and report PASS or FAIL.
#
# The B side of the A/B; tools/esp-idf-ci-run.sh is the A side.  Same job,
# different boot mechanism, so they are siblings rather than one script with a
# flag.
#
# Usage:
#   tools/rtems-ci-run.sh [options] <image.bin>
#
#   <image.bin>   the raw image the RTEMS build backend produced, which is
#                 <build-path>/<name>.bin
#
# Options:
#   -q QEMU   qemu-system-riscv32   (default: $QEMU_ESP32C3 or src/esp-qemu/build/)
#   -o DIR    output dir            (default: ./rtems-ci-results.<timestamp>)
#   -t SECS   hard timeout          (default: 90)
#   -m TEXT   extra required marker, repeatable (added to the defaults)
#   -M TEXT   required marker, repeatable, REPLACING the defaults -- for a
#             config that does not print them, such as a self-test
#   -I        disable instruction counting
#
# Exit status is 0 only if every required marker was seen and no failure
# signature was.
#
# The flash image
#   Nothing to assemble, unlike the ESP-IDF lane's four-part boot chain.  The
#   BSP builds for the ROM's direct boot mode -- start.S emits the two
#   0xaedb041d words the ROM looks for at flash offset 0 and the ROM jumps
#   straight to 0x42000008 -- so the build backend's objcopy output *is* the
#   flash contents, and this only pads it to the flash size.
#
# Why -icount is on by default
#   Instruction counting ties the guest clock to executed instructions rather
#   than to host scheduling, which makes runs reproducible and lets QEMU skip
#   guest idle.  A node that sleeps a second between updates then costs
#   milliseconds of wall clock instead of a second.
#
#   It also changes what a timing number means: with icount the guest runs
#   *faster* than real time while idle, so this harness must not be used to
#   measure an interval in wall-clock terms.  Pass -I for that.

set -u

TOP="$(cd "$(dirname "$0")/.." && pwd)"
QEMU=${QEMU_ESP32C3:-$TOP/src/esp-qemu/build/qemu-system-riscv32}
OUT=""
TMO=90
FLASH_SIZE=${ESP_CI_FLASH_SIZE:-$((4 * 1024 * 1024))}
ICOUNT_ARGS="-icount shift=0,sleep=off"

MARKERS=("CI-MARKER boot ok" "CI-MARKER scheduler ok")

# RTEMS reports a fatal through bsp_fatal_extension, which prints the source and
# code and then the shutdown banner.  "RTEMS shutdown" alone is not a failure --
# a clean rtems_shutdown_executive() prints it too -- so match the fatal header
# rather than the banner.
FAILSIGS='\*\*\* FATAL \*\*\*|fatal source:|RTEMS_FATAL_SOURCE|assertion .* failed'

replaced=0
while getopts "q:o:t:m:M:I" opt; do
  case $opt in
    q) QEMU=$OPTARG;;
    o) OUT=$OPTARG;;
    t) TMO=$OPTARG;;
    m) MARKERS+=("$OPTARG");;
    M) if [ $replaced = 0 ]; then MARKERS=(); replaced=1; fi
       MARKERS+=("$OPTARG");;
    I) ICOUNT_ARGS="";;
    *) exit 2;;
  esac
done
shift $((OPTIND - 1))

IMAGE=${1:-}
[ -n "$IMAGE" ] || { echo "error: no image given" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "error: no such image: $IMAGE" >&2; exit 2; }
[ -x "$QEMU" ] || { echo "error: QEMU not found: $QEMU" >&2; exit 2; }
"$QEMU" -M help 2>/dev/null | grep -q '^esp32c3 ' || {
  echo "error: $QEMU has no esp32c3 machine; it is not the Espressif fork" >&2
  exit 2
}

[ -n "$OUT" ] || OUT=rtems-ci-results.$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
log="$OUT/serial.log"
flash="$OUT/flash.bin"

# Direct boot: the build backend's objcopy output is already the flash
# contents, so this only pads it out to the size QEMU should model.
dd if=/dev/zero of="$flash" bs=1m count=$((FLASH_SIZE / 1024 / 1024)) 2>/dev/null
dd if="$IMAGE" of="$flash" conv=notrunc 2>/dev/null
printf 'image: %s (%s bytes)\n' "$IMAGE" "$(wc -c < "$IMAGE" | tr -d ' ')"

rm -f "$log"
# shellcheck disable=SC2086
"$QEMU" -M esp32c3 -display none -monitor none -no-reboot $ICOUNT_ARGS \
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

# QEMU never exits on its own here: a working node loops forever, which is
# correct behaviour rather than a hang.
kill -9 $qpid 2>/dev/null
wait $qpid 2>/dev/null

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
