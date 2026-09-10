#!/bin/bash
#
# Build the QEMU in src/esp-qemu, which carries the ESP32-C3 fixes, for
# riscv/esp32c3db.
#
# Usage:
#   scripts/build_esp_qemu.sh              fetch, configure, build
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
# run anything that takes an interrupt, and their clock runs slow in proportion
# to how often software reads it.  Two defects, both in the ESP32-C3 models:
#
#   3d3909d  the interrupt matrix does not implement INTR_STATUS_0/_1, which is
#            how software identifies which peripheral raised a CPU line
#   69094f5  the systimer discards the truncated remainder of every ns-to-ticks
#            conversion, and every guest read is a conversion
#
# src/esp-qemu is a fork carrying both as commits on esp32c3-rtems-fixes, so
# there is nothing to apply here -- fetching the submodule is enough.  Both are
# candidates for espressif/qemu and neither is RTEMS-specific; when they land
# upstream, point the submodule back and delete the branch.
#
# See docs/esp32c3-bsp.md for how each was found.
#
# Needs: a C toolchain, ninja, python3, pkg-config, glib and libgcrypt >= 1.8.

set -u

top="$(cd "$(dirname "$0")/.." && pwd)"
cd "$top" || exit 2

src="$top/src/esp-qemu"
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

# src/esp-qemu is declared "update = none", so a default clone has an empty
# directory here and fetching it is this script's job rather than something the
# caller has to know to do first.
if [ ! -d "$src/.git" ] && [ ! -f "$src/.git" ]; then
    echo "fetching src/esp-qemu"
    git -C "$top" submodule update --init --checkout --depth=1 src/esp-qemu \
        || exit 1
fi

# The fixes are commits on the pinned branch, not patches, so the only thing
# to check is that the submodule is actually on its recorded commit.  A
# submodule left on some other revision would build a QEMU that no result here
# can be attributed to.
recorded=$(git -C "$top" ls-files -s src/esp-qemu | awk '{print $2}')
actual=$(git -C "$src" rev-parse HEAD 2>/dev/null)
if [ "$recorded" != "$actual" ]; then
    echo "$0: src/esp-qemu is on $actual, not the recorded $recorded" >&2
    echo "  git submodule update --checkout src/esp-qemu" >&2
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
