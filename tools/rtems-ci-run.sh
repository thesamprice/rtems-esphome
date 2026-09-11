#!/bin/bash
# Run an ESPHome/RTEMS image under Espressif QEMU and report PASS or FAIL.
#
# The B side of the A/B; tools/esp-idf-ci-run.sh is the A side.  Same job,
# different boot mechanism, so they are siblings rather than one script with a
# flag.
#
# Usage:
#   tools/rtems-ci-run.sh [options] <image.bin> [-- <qemu args>...]
#
#   <image.bin>   the raw image the RTEMS build backend produced, which is
#                 <build-path>/<name>.bin
#
#   Anything after -- is appended to the QEMU command line.  That is how a
#   configuration that needs something on the other end of a bus gets it, for
#   example:
#
#       -- -device tmp105,address=0x48
#
#   An I2C driver with nothing to talk to can only be tested for not crashing,
#   which is the kind of pass this harness exists to avoid.
#
# Options:
#   -q QEMU   qemu-system-riscv32   (default: $QEMU_ESP32C3 or src/esp-qemu/build/)
#   -o DIR    output dir            (default: ./rtems-ci-results.<timestamp>)
#   -t SECS   hard timeout          (default: 90)
#   -m TEXT   extra required marker, repeatable (added to the defaults)
#   -M TEXT   required marker, repeatable, REPLACING the defaults -- for a
#             config that does not print them, such as a self-test
#   -I        disable instruction counting
#   -Q FILE   QMP commands to run before the guest starts, one JSON object per
#             line.  Implies -S: QEMU is started paused, the commands are sent,
#             then the guest is released with "cont".
#
#             This exists because some device state cannot be set from the
#             command line.  QEMU's tmp105 registers its "temperature" property
#             in instance_init, so -device tmp105,temperature=25000 does apply
#             it -- and then realize() calls tmp105_reset(), which sets
#             temperature back to 0.  Setting it over QMP after realize is the
#             only way to give the device a value, and a sensor reading 0 C is
#             indistinguishable from a sensor that was never read.
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
DATA_ARGS=""
[ -n "${QEMU_DATA_DIR:-}" ] && DATA_ARGS="-L ${QEMU_DATA_DIR}"
ICOUNT_ARGS="-icount shift=0,sleep=off"
QMP_FILE=""

MARKERS=("CI-MARKER boot ok" "CI-MARKER scheduler ok")

# RTEMS reports a fatal through bsp_fatal_extension, which prints the source and
# code and then the shutdown banner.  "RTEMS shutdown" alone is not a failure --
# a clean rtems_shutdown_executive() prints it too -- so match the fatal header
# rather than the banner.
FAILSIGS='\*\*\* FATAL \*\*\*|fatal source:|RTEMS_FATAL_SOURCE|assertion .* failed'

replaced=0
while getopts "q:o:t:m:M:IQ:" opt; do
  case $opt in
    q) QEMU=$OPTARG;;
    o) OUT=$OPTARG;;
    t) TMO=$OPTARG;;
    m) MARKERS+=("$OPTARG");;
    M) if [ $replaced = 0 ]; then MARKERS=(); replaced=1; fi
       MARKERS+=("$OPTARG");;
    I) ICOUNT_ARGS="";;
    Q) QMP_FILE=$OPTARG;;
    *) exit 2;;
  esac
done
shift $((OPTIND - 1))

IMAGE=${1:-}
[ -n "$IMAGE" ] || { echo "error: no image given" >&2; exit 2; }
shift || true

# Anything after -- goes to QEMU untouched.  Expanded below as
# ${EXTRA[@]+"${EXTRA[@]}"} rather than "${EXTRA[@]}": bash 3.2, which is what
# /bin/bash is on macOS, treats the latter as an unbound variable under set -u
# when the array is empty, and the failure is silent -- QEMU is simply never
# started.
EXTRA=()
if [ "${1:-}" = "--" ]; then
  shift
  EXTRA=("$@")
fi
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
# bs takes a plain byte count, not "1m".  That suffix is BSD's; GNU dd spells it
# "1M" and rejects the lowercase form, so this worked on a developer's Mac and
# produced a zero-length image in CI, where QEMU refused the drive and the run
# loop below reported a timeout for a guest that never started.
if ! dd if=/dev/zero of="$flash" bs=1048576 count=$((FLASH_SIZE / 1048576)) 2>"$OUT/dd.err"; then
  echo "error: could not create the $((FLASH_SIZE / 1048576))MB flash image" >&2
  cat "$OUT/dd.err" >&2
  exit 2
fi
rm -f "$OUT/dd.err"

if ! dd if="$IMAGE" of="$flash" conv=notrunc 2>/dev/null; then
  echo "error: could not place $IMAGE in the flash image" >&2
  exit 2
fi

# QEMU accepts only 2, 4, 8 and 16MB flash images and refuses anything else with
# a drive error the run loop cannot tell from a firmware that never booted.
actual=$(wc -c < "$flash" | tr -d ' ')
if [ "$actual" != "$FLASH_SIZE" ]; then
  echo "error: flash image is $actual bytes, expected $FLASH_SIZE" >&2
  exit 2
fi

printf 'image: %s (%s bytes)\n' "$IMAGE" "$(wc -c < "$IMAGE" | tr -d ' ')"

rm -f "$log"

QMP_ARGS=""
if [ -n "$QMP_FILE" ]; then
  [ -r "$QMP_FILE" ] || { echo "error: cannot read QMP file: $QMP_FILE" >&2; exit 2; }
  # A short path: AF_UNIX is capped near 104 characters and an output directory
  # under a scratch tree can exceed it on its own.
  QMP_SOCK=$(mktemp -u /tmp/rtems-ci-qmp.XXXXXX)
  QMP_ARGS="-S -qmp unix:$QMP_SOCK,server,nowait"
fi

# shellcheck disable=SC2086
"$QEMU" -M esp32c3 -display none -monitor none -no-reboot $ICOUNT_ARGS $DATA_ARGS $QMP_ARGS \
  -serial file:"$log" \
  -drive file="$flash",if=mtd,format=raw \
  ${EXTRA[@]+"${EXTRA[@]}"} \
  > "$OUT/qemu.out" 2> "$OUT/qemu.err" &
qpid=$!

if [ -n "$QMP_FILE" ]; then
  if ! python3 "$(dirname "$0")/qmp-preboot.py" "$QMP_SOCK" "$QMP_FILE" \
       > "$OUT/qmp.log" 2>&1; then
    echo "error: QMP setup failed, see $OUT/qmp.log" >&2
    sed -n '1,20p' "$OUT/qmp.log" >&2
    kill "$qpid" 2>/dev/null
    rm -f "$QMP_SOCK" "$flash"
    exit 1
  fi
  rm -f "$QMP_SOCK"
fi

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
