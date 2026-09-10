#!/bin/bash
# Prove the RTEMS build path end to end, without ESPHome in the way.
#
# Usage:
#   tests/rtems-build-probe/build-and-run.sh [-b arch/bsp] [-p PREFIX]
#
# What it establishes, which is the whole point of it:
#
#   * A C++20 translation unit -- virtuals, std::string, std::vector,
#     std::unique_ptr -- compiles and links for the BSP.  A generated ESPHome
#     image is C++ and uses all of that, so "C compiles" would not have been
#     an answer.
#   * pkg-config alone is enough.  The BSP installs
#     <arch>-rtems7-<bsp>.pc carrying Cflags and Ldflags, so a build backend
#     needs no knowledge of RTEMS' own build system: no waf, no cmake, no
#     RTEMS makefiles.  That is what makes the ESPHome integration small.
#   * The direct boot image works for an application we built ourselves, not
#     only for RTEMS' own test executables.
#
# Run it when the toolchain or BSP install changes.  A failure here means the
# problem is underneath ESPHome, which is worth knowing before debugging
# upwards.

set -u
here="$(cd "$(dirname "$0")" && pwd)"
top="$(cd "$here/../.." && pwd)"

BSP=${RTEMS_BSP:-riscv/esp32c3db}
PREFIX=${RTEMS_TOOLS_PREFIX:-$HOME/rtems/7}
QEMU=${QEMU_ESP32C3:-$top/src/esp-qemu/build/qemu-system-riscv32}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/rtems-probe.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

while getopts "b:p:q:" opt; do
  case $opt in
    b) BSP=$OPTARG;;
    p) PREFIX=$OPTARG;;
    q) QEMU=$OPTARG;;
    *) exit 2;;
  esac
done

ARCH=${BSP%%/*}
BOARD=${BSP##*/}
PC=${ARCH}-rtems7-${BOARD}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

pkg-config --exists "$PC" || {
  echo "error: no pkg-config file for $PC" >&2
  echo "       the BSP is built but not installed; run ./waf install in the RTEMS tree" >&2
  exit 2
}

CXX="$PREFIX/bin/${ARCH}-rtems7-g++"
OBJCOPY="$PREFIX/bin/${ARCH}-rtems7-objcopy"
for t in "$CXX" "$OBJCOPY"; do
  [ -x "$t" ] || { echo "error: missing $t" >&2; exit 2; }
done

CF=$(pkg-config --cflags "$PC")
LF=$(pkg-config --libs "$PC")

echo "building $BSP with:"
echo "  cflags: $CF"
echo "  ldflags: $LF"

# Unquoted on purpose: each is a list of words, not one argument.
# shellcheck disable=SC2086
"$CXX" $CF -O2 -std=gnu++20 -o "$OUT/probe.exe" "$here/main.cpp" $LF || exit 1
echo "linked"

[ -x "$QEMU" ] || { echo "no QEMU at $QEMU; built but not run" >&2; exit 0; }

# Direct boot: the ELF's load addresses are already flash offsets, so objcopy
# places the image with no further work.  See docs/esp32c3-bsp.md.
"$OBJCOPY" -O binary "$OUT/probe.exe" "$OUT/probe.bin"
dd if=/dev/zero of="$OUT/flash.bin" bs=1m count=4 2>/dev/null
dd if="$OUT/probe.bin" of="$OUT/flash.bin" conv=notrunc 2>/dev/null

"$QEMU" -M esp32c3 -display none -monitor none -no-reboot -icount shift=0,sleep=off \
  -serial file:"$OUT/serial.log" -drive file="$OUT/flash.bin",if=mtd,format=raw \
  > /dev/null 2>&1 &
qpid=$!
for _ in $(seq 1 60); do
  grep -q "PROBE-MARKER done" "$OUT/serial.log" 2>/dev/null && break
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done
kill -9 $qpid 2>/dev/null; wait $qpid 2>/dev/null

if grep -q "PROBE-MARKER done" "$OUT/serial.log" 2>/dev/null; then
  grep "PROBE-MARKER" "$OUT/serial.log" | sed 's/^/  /'
  echo "PASS"
  exit 0
fi
echo "FAIL: probe did not complete" >&2
sed 's/^/  /' "$OUT/serial.log" >&2
exit 1
