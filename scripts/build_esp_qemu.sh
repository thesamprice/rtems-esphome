#!/bin/bash
#
# Build the Espressif QEMU fork in src/esp-qemu, patched, for riscv/esp32c3db.
#
# Usage:
#   scripts/build_esp_qemu.sh              fetch, patch, configure, build
#   scripts/build_esp_qemu.sh -j N         parallelism
#   scripts/build_esp_qemu.sh -p DIR       also install to this prefix
#   scripts/build_esp_qemu.sh -C           reconfigure from scratch
#
# The result is left runnable in the build tree at
#
#     src/esp-qemu/build/qemu-system-riscv32
#
# which is where tools/esp32c3-run-tests.sh looks.  Installing is optional and
# only worth doing to put it on a PATH.
#
# Only riscv32-softmmu is built.  The xtensa targets would cover the ESP32, S2
# and S3, and RTEMS has no Xtensa port, so they are build time spent on
# machines nothing here can boot.
#
# Why build rather than download
# ------------------------------
# Espressif publish binaries, and they run hello on this BSP, but they cannot
# run anything that takes an interrupt.  hw/riscv/esp32c3_intmatrix.c
# implements the map, priority, threshold, enable and type registers and
# returns 0 for everything else -- INTERRUPT_CORE0_INTR_STATUS_0 and _1 at 0xf8
# and 0xfc included.  Those two are how software finds out which peripheral
# raised a CPU interrupt line, because the matrix maps 62 sources onto 31 lines
# and the BSP shares lines deliberately.  Reading 0 there makes every interrupt
# dispatch as vector 0, the invalid vector, so the first clock tick ends the run
# in RTEMS_FATAL_SOURCE_SPURIOUS_INTERRUPT.
#
# patches/esp-qemu/intmatrix-status.patch returns the value the model already
# keeps for its own use.  See docs/esp32c3-bsp.md.
#
# Needs: a C toolchain, ninja, python3, pkg-config, glib and libgcrypt >= 1.8.

set -u

top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top" || exit 2

src="$top/src/esp-qemu"
patch="$top/patches/esp-qemu/intmatrix-status.patch"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
prefix=""
reconfigure=no

while [ $# -gt 0 ]; do
    case "$1" in
        -j) jobs="$2"; shift 2 ;;
        -p) prefix="$2"; shift 2 ;;
        -C) reconfigure=yes; shift ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "$0: unknown argument $1" >&2; exit 2 ;;
    esac
done

[ -f "$patch" ] || { echo "$0: no $patch" >&2; exit 1; }

# src/esp-qemu is declared "update = none", so a default clone has an empty
# directory here and fetching it is this script's job rather than something the
# caller has to know to do first.
if [ ! -d "$src/.git" ] && [ ! -f "$src/.git" ]; then
    echo "fetching src/esp-qemu"
    git -C "$top" submodule update --init --checkout --depth=1 src/esp-qemu \
        || exit 1
fi

# Idempotent: a reverse check that succeeds means the patch is already in.
if git -C "$src" apply --reverse --check "$patch" > /dev/null 2>&1; then
    echo "patch already applied"
elif git -C "$src" apply --check "$patch" > /dev/null 2>&1; then
    git -C "$src" apply "$patch" || exit 1
    echo "patch applied"
else
    echo "$0: $patch does not apply to src/esp-qemu and is not already applied" >&2
    echo "  (checked out: $(git -C "$src" describe --tags --always 2>/dev/null))" >&2
    exit 1
fi

cd "$src" || exit 2

[ "$reconfigure" = yes ] && rm -rf build

if [ ! -f build/build.ninja ]; then
    echo "configuring (riscv32-softmmu only)"
    # --disable-containers is not about what gets built.  configure probes for
    # a container engine to find cross compilers with, and the probe runs
    # "docker probe" whenever a docker binary is on PATH.  With Docker
    # installed but not running, that call blocks rather than failing, and
    # configure hangs there with no output -- for as long as you let it.
    # Nothing here cross builds through a container, so skip the probe.
    conf_args="--target-list=riscv32-softmmu --disable-werror --disable-containers"
    #
    # --enable-gcrypt is required, not a preference.  The esp32c3 machine
    # always instantiates its AES, RSA, DS and XTS-AES devices, and
    # hw/misc/meson.build only compiles those when gcrypt is found.  QEMU's own
    # meson does not look for gcrypt at all when gnutls provides crypto, which
    # it does on a normal Homebrew or distro machine, so the build succeeds and
    # then the machine dies at init:
    #
    #     qemu-system-riscv32: unknown type 'misc.esp32c3.aes'
    #
    # --disable-gnutls goes with it.  Asking for gcrypt takes gnutls out of the
    # crypto path but leaves it in the build, and that combination does not
    # compile: crypto/tlscredspriv.h is reached without gnutls's include path
    # and every file that pulls it in fails on "gnutls/gnutls.h: file not
    # found".  Nothing here needs TLS.
    conf_args="$conf_args --enable-gcrypt --disable-gnutls"
    [ -n "$prefix" ] && conf_args="$conf_args --prefix=$prefix"
    # shellcheck disable=SC2086
    ./configure $conf_args || exit 1
fi

echo "building with -j $jobs"
ninja -C build -j "$jobs" || exit 1

qemu="$src/build/qemu-system-riscv32"
[ -x "$qemu" ] || { echo "$0: no $qemu after build" >&2; exit 1; }
"$qemu" -M help | grep -q '^esp32c3 ' || {
    echo "$0: $qemu has no esp32c3 machine" >&2; exit 1; }

if [ -n "$prefix" ]; then
    echo "installing to $prefix"
    ninja -C build install || exit 1
fi

echo
echo "built: $qemu"
echo "tools/esp32c3-run-tests.sh finds it there by default."
