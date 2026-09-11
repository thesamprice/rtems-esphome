#!/bin/bash
# Unattended RTEMS testsuite runner for the riscv/esp32c3db BSP on the
# Espressif QEMU fork (-M esp32c3).  Runs the whole flow in one invocation and
# writes a classified report; intended to be started once and inspected when
# done.
#
# Usage:
#   esp32c3-run-tests.sh [options] [test-name-or-glob ...]
#
# Options:
#   -b DIR   RTEMS build dir     ($RTEMS_BUILD, or -b; required.  It is the
#                                 directory containing testsuites/, e.g.
#                                 <rtems>/build-esp32c3db/riscv/esp32c3db)
#   -q QEMU  qemu-system-riscv32 (default: $QEMU_ESP32C3 or the one
#                                 scripts/build_esp_qemu.sh leaves in
#                                 src/esp-qemu/build/)
#   -o DIR   output dir          (default: ./esp32c3-test-results.<timestamp>)
#   -j N     parallel QEMU instances               (default: 2)
#   -t SECS  per-test wall-clock timeout           (default: 120)
#   -T SECS  solo-retry timeout for failed tests   (default: 400)
#   -P       include performance suites (benchmarks, tmtests, psxtmtests,
#            rhealstone); excluded by default
#   -n       no solo retry of failures
#   -I       disable QEMU instruction counting (also: ESP32C3_NO_ICOUNT=1)
#   -x LIST  space-separated names classified XFAIL-KNOWN when they fail
#
# Structure and defaults follow the RTEMS testsuite runners: QEMU is reaped per
# test rather than waited on, because the image spins in bsp_reset and QEMU
# never exits on its own; the serial log is polled for the end marker instead
# of running out the timeout; and failures are re-run solo before being
# reported.
#
# What is different: the image.
#   The ESP32-C3 boots from flash, not from a loader device, so there is no
#   "-device loader,file=...exe" to hand QEMU an ELF.  The BSP builds a direct
#   boot image -- start.S emits the two 0xaedb041d words the ROM looks for at
#   flash offset 0 and the ROM jumps to 0x42000008 -- so the runnable artifact
#   is objcopy -O binary of the ELF, padded out to the flash size and attached
#   with -drive if=mtd.  The ELF's LMAs are already flash offsets (text at 0,
#   rodata and the data load images at ESP32C_CODE_REGION_SIZE), so objcopy
#   lays the image out correctly with no further placement.
#
#   The images are built into a temporary directory and deleted as they are
#   run, so the peak cost is -j times the flash size rather than 632 times it.
#
# What is also different: QEMU must be patched.
#   Its interrupt matrix model does not implement the two
#   INTERRUPT_CORE0_INTR_STATUS registers, which are how the BSP finds out
#   which peripheral raised a CPU interrupt line.  Unpatched, every interrupt
#   resolves to vector 0 and the first clock tick is a fatal spurious
#   interrupt, so everything past hello fails identically.  This script checks
#   for the fix and refuses to run a whole suite against a QEMU that lacks it;
#   scripts/build_esp_qemu.sh builds one that has it.
#
# Classification (summary.tsv):
#   PASS    END OF TEST marker seen
#   XFAIL   TEST STATE: EXPECTED_FAIL (upstream-known failure)
#   SKIP    TEST STATE: USER_INPUT or BENCHMARK
#   FAIL    anything else (details in <out>/logs/<test>.log)
# Exit status is 0 iff there is no FAIL.

set -u

BUILD=${RTEMS_BUILD:-}
TOP="$(cd "$(dirname "$0")/.." && pwd)"
QEMU=${QEMU_ESP32C3:-$TOP/src/esp-qemu/build/qemu-system-riscv32}
OBJCOPY=${OBJCOPY:-${RTEMS_TOOLS_PREFIX:-$HOME/rtems/7}/bin/riscv-rtems7-objcopy}
OUT=""
JOBS=2
TMO=120
SOLO_TMO=400
WITH_PERF=0
RETRY=1
CHECK_QEMU=1
XFAIL=${ESP32C3_XFAIL:-""}
ICOUNT_ARGS="-icount shift=0,sleep=off"
DRAIN_TMO="${ESP32C3_DRAIN_TMO:-5}"
[ "${ESP32C3_NO_ICOUNT:-0}" = 0 ] || ICOUNT_ARGS=""

# Flash size the linker script was configured for.  A shorter image is padded
# up to it; QEMU sizes the modelled SPI flash from the file.
FLASH_SIZE=${ESP32C3_FLASH_SIZE:-$((4 * 1024 * 1024))}

while getopts "b:q:o:j:t:T:PnIx:Q" opt; do
  case $opt in
    b) BUILD=$OPTARG;;
    q) QEMU=$OPTARG;;
    o) OUT=$OPTARG;;
    j) JOBS=$OPTARG;;
    t) TMO=$OPTARG;;
    T) SOLO_TMO=$OPTARG;;
    P) WITH_PERF=1;;
    n) RETRY=0;;
    I) ICOUNT_ARGS="";;
    x) XFAIL=$OPTARG;;
    Q) CHECK_QEMU=0;;
    *) exit 2;;
  esac
done
shift $((OPTIND - 1))

[ -x "$QEMU" ] || { echo "error: QEMU not found: $QEMU" >&2; exit 2; }
[ -x "$OBJCOPY" ] || { echo "error: objcopy not found: $OBJCOPY" >&2; exit 2; }
[ -n "$BUILD" ] || {
  echo "error: no RTEMS build directory; set \$RTEMS_BUILD or pass -b DIR" >&2
  exit 2
}
[ -d "$BUILD/testsuites" ] || { echo "error: no testsuites in $BUILD" >&2; exit 2; }
"$QEMU" -M help 2>/dev/null | grep -q '^esp32c3 ' || {
  echo "error: $QEMU has no esp32c3 machine; it is not the Espressif fork" >&2
  exit 2
}
[ -n "$OUT" ] || OUT=esp32c3-test-results.$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT/logs"
OUT=$(cd "$OUT" && pwd)

IMGDIR=$(mktemp -d "${TMPDIR:-/tmp}/esp32c3-flash.XXXXXX") || exit 2
trap 'rm -rf "$IMGDIR"' EXIT

# ---------------------------------------------------------------- flash image
make_flash() {
  # make_flash <exe> <out.bin>
  "$OBJCOPY" -O binary "$1" "$2.raw" 2>/dev/null || return 1
  # Pad rather than truncate: the image is smaller than the flash, and QEMU
  # takes the flash size from the file it is given.
  # bs takes a plain byte count.  "1m" is BSD's spelling and GNU dd rejects it,
  # which on Linux leaves a zero-length image that QEMU refuses -- indisting-
  # uishable, from the caller, from a test that failed to boot.
  dd if=/dev/zero of="$2" bs=1048576 count=$((FLASH_SIZE / 1048576)) \
     2>/dev/null || return 1
  dd if="$2.raw" of="$2" conv=notrunc 2>/dev/null || return 1
  rm -f "$2.raw"
  # Refuse to hand QEMU an image it cannot model rather than let the run look
  # like a failed test.
  [ "$(wc -c < "$2" | tr -d ' ')" = "$FLASH_SIZE" ] || return 1
}

# ---------------------------------------------------------------- QEMU check
# A QEMU without the interrupt matrix status registers fails every test that
# takes an interrupt, in the same place, for a reason that has nothing to do
# with the test.  Finding that out 632 times is not a test result, so check
# once: ticker reaches its first clock tick and dies there.
if [ $CHECK_QEMU = 1 ]; then
  probe=$BUILD/testsuites/samples/ticker.exe
  if [ -f "$probe" ]; then
    make_flash "$probe" "$IMGDIR/probe.bin" || exit 2
    "$QEMU" -M esp32c3 -display none -monitor none -no-reboot \
      -serial file:"$OUT/logs/qemu-probe.log" \
      -drive file="$IMGDIR/probe.bin",if=mtd,format=raw \
      > /dev/null 2>&1 &
    ppid=$!
    for i in $(seq 1 60); do
      grep -q "END OF TEST\|FATAL" "$OUT/logs/qemu-probe.log" 2>/dev/null && break
      kill -0 $ppid 2>/dev/null || break
      sleep 0.5
    done
    kill -9 $ppid 2>/dev/null; wait $ppid 2>/dev/null
    rm -f "$IMGDIR/probe.bin"
    if grep -q "SPURIOUS_INTERRUPT" "$OUT/logs/qemu-probe.log" 2>/dev/null; then
      echo "error: this QEMU dispatches every interrupt as spurious." >&2
      echo "       Its interrupt matrix does not implement" >&2
      echo "       INTERRUPT_CORE0_INTR_STATUS_0/1.  Build a patched one with" >&2
      echo "       scripts/build_esp_qemu.sh, or pass -Q to run anyway." >&2
      exit 2
    fi
  fi
fi

# ---------------------------------------------------------------- test list
find "$BUILD/testsuites" -name '*.exe' ! -name '*.norun.exe' | sort \
  > "$OUT/all-tests.txt"

: > "$OUT/queue.txt"
: > "$OUT/deferred-perf.txt"
while IFS= read -r exe; do
  name=$(basename "$exe" .exe)
  if [ $# -gt 0 ]; then
    match=0
    for pat in "$@"; do
      case $name in ($pat) match=1;; esac
    done
    [ $match = 1 ] || continue
  fi
  if [ $WITH_PERF = 0 ]; then
    case $exe in
      */benchmarks/*|*/tmtests/*|*/psxtmtests/*|*/rhealstone/*)
        echo "$exe" >> "$OUT/deferred-perf.txt"; continue;;
    esac
    case $name in
      spintrcritical*) echo "$exe" >> "$OUT/deferred-perf.txt"; continue;;
    esac
  fi
  echo "$exe" >> "$OUT/queue.txt"
done < "$OUT/all-tests.txt"

total=$(wc -l < "$OUT/queue.txt" | tr -d ' ')
echo "$(date '+%H:%M:%S') running $total tests, $JOBS parallel," \
     "timeout ${TMO}s, icount ${ICOUNT_ARGS:-off}" \
     "(deferred perf: $(wc -l < "$OUT/deferred-perf.txt" | tr -d ' '))"

# ---------------------------------------------------------------- one test
run_one() {
  # run_one <exe> <timeout-secs>  -> prints "VERDICT name"
  local exe=$1 tmo=$2
  local name log flash qpid i j
  name=$(basename "$exe" .exe)
  log=$OUT/logs/$name.log
  flash=$IMGDIR/$name.bin
  rm -f "$log"

  if ! make_flash "$exe" "$flash"; then
    echo "NO-IMAGE $name"
    return
  fi

  # $ICOUNT_ARGS is intentionally unquoted: it is either empty or a list of
  # words.
  "$QEMU" -M esp32c3 -display none -monitor none -no-reboot $ICOUNT_ARGS \
    -serial file:"$log" \
    -drive file="$flash",if=mtd,format=raw \
    > /dev/null 2> "$OUT/logs/$name.err" &
  qpid=$!

  for i in $(seq 1 $((tmo * 2))); do
    if grep -q "END OF TEST" "$log" 2>/dev/null; then
      for j in $(seq 1 $((DRAIN_TMO * 4))); do
        grep -q "\[ RTEMS shutdown \]" "$log" 2>/dev/null && break
        kill -0 $qpid 2>/dev/null || break
        sleep 0.25
      done
      sleep 0.2
      break
    fi
    if grep -q "\[ RTEMS shutdown \]" "$log" 2>/dev/null; then
      sleep 0.5
      break
    fi
    kill -0 $qpid 2>/dev/null || break
    sleep 0.5
  done

  kill -9 $qpid 2>/dev/null
  wait $qpid 2>/dev/null
  rm -f "$flash"

  local v
  if grep -q "END OF TEST" "$log" 2>/dev/null; then
    v=PASS
  elif grep -q "TEST STATE: EXPECTED_FAIL" "$log" 2>/dev/null; then
    v=XFAIL
  elif grep -q "TEST STATE: USER_INPUT" "$log" 2>/dev/null; then
    v=SKIP-USER-INPUT
  elif grep -q "TEST STATE: BENCHMARK" "$log" 2>/dev/null; then
    v=SKIP-BENCHMARK
  elif [ ! -s "$log" ]; then
    v=NO-OUTPUT
  else
    v=FAIL
  fi
  echo "$v $name"
}

worker() {
  # worker <list-file> <timeout> <result-file>
  local exe
  while IFS= read -r exe; do
    run_one "$exe" "$2" >> "$3"
  done < "$1"
}

# ---------------------------------------------------------------- main run
rm -f "$OUT"/chunk.* "$OUT"/result.*
split -n r/"$JOBS" "$OUT/queue.txt" "$OUT/chunk." 2>/dev/null \
  || split -l $(( (total + JOBS - 1) / JOBS )) "$OUT/queue.txt" "$OUT/chunk."

for c in "$OUT"/chunk.*; do
  [ -s "$c" ] || continue
  worker "$c" "$TMO" "$OUT/result.$(basename "$c")" &
done
wait

cat "$OUT"/result.* > "$OUT/first-pass.txt" 2>/dev/null

# ---------------------------------------------------------------- solo retry
awk '$1=="FAIL" || $1=="NO-OUTPUT" {print $2}' "$OUT/first-pass.txt" \
  > "$OUT/retry-names.txt"
: > "$OUT/retry-results.txt"

if [ $RETRY = 1 ] && [ -s "$OUT/retry-names.txt" ]; then
  echo "$(date '+%H:%M:%S') retrying $(wc -l < "$OUT/retry-names.txt" | tr -d ' ')" \
       "failures solo with ${SOLO_TMO}s timeout"
  while IFS= read -r name; do
    exe=$(grep "/$name\.exe\$" "$OUT/queue.txt" | head -1)
    [ -n "$exe" ] && run_one "$exe" "$SOLO_TMO" >> "$OUT/retry-results.txt"
  done < "$OUT/retry-names.txt"
fi

# ---------------------------------------------------------------- summarize
awk -v xfail="$XFAIL" '
  BEGIN { split(xfail, xa, " "); for (i in xa) xf[xa[i]] = 1 }
  NF==2 { v[$2] = $1 }
  END {
    for (n in v) {
      verdict = v[n]
      if ((verdict == "FAIL" || verdict == "NO-OUTPUT") && (n in xf))
        verdict = "XFAIL-KNOWN"
      print verdict "\t" n
    }
  }
' "$OUT/first-pass.txt" "$OUT/retry-results.txt" \
  | sort -k2 > "$OUT/summary.tsv"

echo
echo "==== esp32c3db testsuite summary ($OUT/summary.tsv) ===="
cut -f1 "$OUT/summary.tsv" | sort | uniq -c
fails=$(awk -F'\t' '$1=="FAIL" || $1=="NO-OUTPUT" || $1=="NO-IMAGE"' "$OUT/summary.tsv")
if [ -n "$fails" ]; then
  echo "---- failures ----"
  echo "$fails"
  exit 1
fi
exit 0
