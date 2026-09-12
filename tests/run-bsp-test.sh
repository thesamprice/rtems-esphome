#!/bin/sh
#
# Build and run a standalone BSP test on riscv/esp32c3db.
#
# Standalone rather than a testsuite entry: the BSP arrives here as patches
# rather than as a tree we own, so adding a spec/build test would mean another
# patch to maintain against every rebase.  This links against a staged install
# of the BSP, which is what an application does.
#
# Usage:  tests/run-bsp-test.sh <test-dir> [OUTDIR]
#
#   tests/run-bsp-test.sh tests/bsp-pin
#   tests/run-bsp-test.sh tests/bsp-opendrain
#
# Needs: the riscv-rtems7 toolchain (default $HOME/rtems/7) and the Espressif
# QEMU fork that scripts/build_esp_qemu.sh leaves in src/esp-qemu/build/.

set -eu

top="$(cd "$(dirname "$0")/.." && pwd)"
TESTDIR=${1:?usage: run-bsp-test.sh <test-dir> [outdir] [-- qemu args...]}
NAME=$(basename "$TESTDIR")
OUT=${2:-$top/$NAME-results}
shift 2 2>/dev/null || shift $# 
# Anything after -- reaches QEMU, which is how a lane gets a device on a bus.
if [ "${1:-}" = "--" ]; then shift; fi
EXTRA="$*"
PREFIX=${RTEMS_TOOLS_PREFIX:-$HOME/rtems/7}
BUILD=${RTEMS_BUILD:-$top/src/rtems/build-esp32c3db}
QEMU=${QEMU_ESP32C3:-$top/src/esp-qemu/build/qemu-system-riscv32}
FLASH_SIZE=$((4 * 1024 * 1024))

mkdir -p "$OUT"

[ -x "$PREFIX/bin/riscv-rtems7-gcc" ] || { echo "no toolchain at $PREFIX" >&2; exit 2; }
[ -x "$QEMU" ] || { echo "no esp32c3 qemu at $QEMU (scripts/build_esp_qemu.sh)" >&2; exit 2; }

# Stage the BSP so the test links the way an application would, against
# installed headers and libraries rather than against the build tree.
stage=$OUT/stage
rm -rf "$stage"
( cd "$top/src/rtems" && ./waf --out="$BUILD" install --destdir="$stage" ) > "$OUT/install.log" 2>&1

lib=$(find "$stage" -type d -name lib -path '*esp32c3db*' | head -1)
[ -n "$lib" ] || { echo "staged BSP not found under $stage" >&2; exit 1; }

"$PREFIX/bin/riscv-rtems7-gcc" -march=rv32imc -mabi=ilp32 \
  -isystem "$lib/include" -B "$lib" -qrtems -Wl,--gc-sections \
  -o "$OUT/$NAME.exe" "$top/$TESTDIR/init.c"

# Direct boot image: objcopy -O binary padded to the flash size, as
# tools/esp32c3-run-tests.sh explains.  bs takes a plain byte count because
# "1m" is BSD's spelling and GNU dd rejects it.
"$PREFIX/bin/riscv-rtems7-objcopy" -O binary "$OUT/$NAME.exe" "$OUT/flash.raw"
dd if=/dev/zero of="$OUT/flash.bin" bs=1048576 count=$((FLASH_SIZE / 1048576)) 2>/dev/null
dd if="$OUT/flash.raw" of="$OUT/flash.bin" conv=notrunc 2>/dev/null
[ "$(wc -c < "$OUT/flash.bin" | tr -d ' ')" = "$FLASH_SIZE" ] || { echo "bad image size" >&2; exit 1; }

log=$OUT/run.log
rm -f "$log"
"$QEMU" -M esp32c3 -display none -monitor none -no-reboot -icount shift=0,sleep=off \
  -serial file:"$log" -drive file="$OUT/flash.bin",if=mtd,format=raw \
  $EXTRA \
  > /dev/null 2>&1 &
qpid=$!
for _ in $(seq 1 120); do
  grep -q "END OF" "$log" 2>/dev/null && break
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done
sleep 1
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true

cat "$log"

# The marker, not the exit status: QEMU is killed rather than waited on, so
# its status says nothing about the test.
# The marker, not the exit status: QEMU is killed rather than waited on.
grep -q "CI-MARKER" "$log" || { echo "FAILED: no CI-MARKER in $log" >&2; exit 1; }
echo "PASS"
