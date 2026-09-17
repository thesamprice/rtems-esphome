#!/bin/sh
# Populate a fresh work directory ($SP) so that
# tools/build-esp32c3-micropython.sh -- and the C examples under
# src/rtems-esp-wifi/examples/ -- can run immediately afterwards.
#
#   SP=$HOME/build/c3 tools/stage-esp32c3-workdir.sh
#
# This is the "open question" docs/handoff.md flags under "Staging the rest of
# the work directory": nothing populated a fresh $SP end to end, and the
# per-object scripts in src/rtems-esp-wifi/tools/ still name a stale
# $SP/iram-prefix.  Those scripts are left alone; the compiles below are the
# same commands with the prefix corrected and the hardcoded scratchpad path
# removed.  See issue #100.
#
# WHAT THIS BUILDS, AND WHAT IT ONLY CHECKS FOR
#
# It builds everything that is minutes of work: the eight glue objects,
# libwpa.a (278 sources) and libmbedtls.a (62), the staged copies of the glue
# and the HAL, the blobs, and the frozen Python tar for the MicroPython image.
#
# It deliberately does NOT build the two BSP prefixes, rtems-lwip, or
# libmicropython.a.  Each is a ./waf or make invocation in a different tree
# that takes a long time, has its own options that a staging script should not
# be choosing on the user's behalf (which console driver, which BSP), and is
# the kind of thing one wants to run and watch rather than have happen inside
# something called "stage".  Where one is missing this stops and prints the
# exact command, copied from docs/handoff.md, rather than failing later in a
# wall of "no such file" or "no pkg-config file".
#
# Re-running is safe.  The copies are refreshed rather than duplicated, the
# eight objects are recompiled (seconds), and the two archives are rebuilt only
# when a source is newer than the archive or RESTAGE=1 is set.
set -eu

# Same convention as tools/build-esp32c3-micropython.sh: REPO is the directory
# above this script, so it runs from anywhere in a clone.
REPO=${REPO:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
SP=${SP:?set SP to a work directory outside the tree, e.g. SP=$HOME/build/c3}

# Not inside the repository.  The work directory holds a 132 MiB copy of the
# HAL and several hundred object files; inside the tree it would be a
# git-status disaster and the .gitignore does not cover it.
#
# Checked before the mkdir as well as after the canonicalisation, so that
# rejecting a path does not leave the directory it rejected behind.
outside_repo() {
  case "$1/" in "$REPO"/*)
    echo "stage: \$SP ($1) is inside the repository ($REPO)." >&2
    echo "stage: put the work directory outside the tree." >&2
    exit 2 ;;
  esac
}
case $SP in /*) outside_repo "$SP" ;; *) outside_repo "$PWD/$SP" ;; esac

mkdir -p "$SP"
SP=$(CDPATH= cd -- "$SP" && pwd)
outside_repo "$SP"

export PATH=$HOME/rtems/7/bin:$PATH
CC=riscv-rtems7-gcc

command -v $CC >/dev/null 2>&1 || {
  echo "stage: $CC is not on PATH (looked in \$HOME/rtems/7/bin)." >&2
  echo "stage: build the toolchain first -- docs/handoff.md, \"Building the" >&2
  echo "stage: toolchain\": scripts/apply_patches.sh rtems rsb, then" >&2
  echo "stage: src/rsb/rtems/../source-builder/sb-set-builder" >&2
  echo "stage:   --prefix=\$HOME/rtems/7 7/rtems-riscv" >&2
  exit 2; }

# ---------------------------------------------------------------------------
# The source checkouts.  Every submodule is "update = none", so a plain clone
# has none of them and the first symptom would be a missing directory three
# compiles in.
# ---------------------------------------------------------------------------
missing=''
for d in src/rtems-esp-wifi/src src/esp-hal-3rdparty/components \
         src/esp32-wifi-lib/esp32c3 src/esp-phy-lib/esp32c3 src/mbedtls/library
do
  [ -d "$REPO/$d" ] || missing="$missing $(dirname "$REPO/$d" | sed "s|$REPO/||")"
done
[ -z "$missing" ] || {
  echo "stage: these submodules are not checked out:$missing" >&2
  echo "stage: git submodule update --init --checkout --depth=1 \\" >&2
  echo "stage:     src/rtems-esp-wifi src/esp-hal-3rdparty src/esp32-wifi-lib \\" >&2
  echo "stage:     src/esp-phy-lib src/mbedtls" >&2
  exit 2; }

# ---------------------------------------------------------------------------
# The two BSP prefixes.  Checked, not built: see the header.
#
# The difference between them is one option, ESPRESSIF_USE_USB_CONSOLE, and
# the reason there are two is that the console driver is chosen at BSP
# configure time -- a board with a bridge chip and a board on native USB need
# different images from the same sources.
#
# The objects below are compiled against wifi-prefix.  That is the prefix both
# C examples link against and the one lwIP is installed into, so it has to
# exist anyway; and it makes no difference to these eight objects, whose
# bspopts.h differs from usb-prefix's only in ESPRESSIF_USE_USB_CONSOLE, which
# none of these sources reads.  (Verified: compiling adapter.o against each
# prefix produces byte-identical objects.)  tools/build-esp32c3-micropython.sh
# then links them against usb-prefix, which is correct and not an oversight.
# ---------------------------------------------------------------------------
waf_hint() {
  echo "stage:   cd $REPO/src/rtems" >&2
  echo "stage:   ./waf configure -o build-$1 \\" >&2
  echo "stage:       --rtems-config=../rtems-esp-wifi/examples/wifi-init/$2 \\" >&2
  echo "stage:       --rtems-tools=\$HOME/rtems/7 --prefix=$SP/$1-prefix" >&2
  echo "stage:   ./waf build && ./waf install" >&2
}
for p in wifi usb; do
  [ -f "$SP/$p-prefix/riscv-rtems7/esp32c3db/lib/include/bspopts.h" ] && continue
  echo "stage: no installed BSP at $SP/$p-prefix." >&2
  echo "stage: build and INSTALL it (install, not just build -- the .pc file" >&2
  echo "stage: downstream reads comes from ./waf install):" >&2
  case $p in
    wifi) waf_hint wifi wifi-bsp.ini ;;
    usb)  waf_hint usb  wifi-bsp-usb.ini ;;
  esac
  exit 2
done

LIB=$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib

# The BSP must have been configured with the enlarged IRAM window, or the
# blobs' ~44 KiB of .wifi*iram has nowhere to go and the link overflows late,
# naming a region rather than the option that sizes it.
grep -q 'define ESP32C_IRAM_REGION_SIZE 0x0000c000' "$LIB/include/bspopts.h" || {
  echo "stage: $SP/wifi-prefix was not configured with" >&2
  echo "stage: ESP32C_IRAM_REGION_SIZE = 0xC000.  Reconfigure it with" >&2
  echo "stage: src/rtems-esp-wifi/examples/wifi-init/wifi-bsp.ini." >&2
  exit 2; }

# lwIP, installed back into wifi-prefix.  The link scripts check for this file
# by name and refuse to start without it, so checking here only moves the same
# message earlier -- but it moves it to the point where the instructions are
# still relevant.
[ -f "$LIB/liblwip.a" ] || {
  echo "stage: no liblwip.a in $LIB." >&2
  echo "stage: build rtems-lwip against that prefix and install it back into it:" >&2
  echo "stage:   cd $REPO/src/rtems-lwip" >&2
  echo "stage:   ./waf configure --rtems-tools=\$HOME/rtems/7 --rtems=$SP/wifi-prefix \\" >&2
  echo "stage:       --rtems-bsp=riscv/esp32c3db --prefix=$SP/wifi-prefix" >&2
  echo "stage:   ./waf build && ./waf install" >&2
  exit 2; }

# ---------------------------------------------------------------------------
# The staged copies.
#
# $SP/glue and $SP/hal are copies, not symlinks, because that is the layout
# every existing script and docs/handoff.md describe and because the build
# should be reproducible from $SP alone once staged.  They are refreshed to
# match the tree on every run, so they are a staging area and not somewhere to
# keep edits -- edit src/rtems-esp-wifi and re-run this.
#
# .git is excluded: 130 MiB of HAL history that no compile reads.
# ---------------------------------------------------------------------------
copy_tree() {  # copy_tree <src> <dst>
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude '.git' "$1"/ "$2"/
  else
    rm -rf "$2"; mkdir -p "$2"
    ( cd "$1" && tar --exclude .git -cf - . ) | ( cd "$2" && tar -xf - )
  fi
}

echo "stage: copying the glue and the HAL into $SP ..."
mkdir -p "$SP/glue" "$SP/hal"
copy_tree "$REPO/src/rtems-esp-wifi" "$SP/glue"
copy_tree "$REPO/src/esp-hal-3rdparty" "$SP/hal"

# The eight blobs.  Seven come from esp32-wifi-lib's esp32c3 directory; libphy
# comes from esp-phy-lib, which is a separate repository and does not carry the
# other seven.
echo "stage: copying the WiFi blobs ..."
mkdir -p "$SP/blobs"
cp "$REPO"/src/esp32-wifi-lib/esp32c3/*.a "$SP/blobs/"
cp "$REPO"/src/esp-phy-lib/esp32c3/libphy.a "$SP/blobs/"
for b in net80211 pp core phy mesh espnow smartconfig wapi; do
  [ -f "$SP/blobs/lib$b.a" ] || {
    echo "stage: lib$b.a did not stage into $SP/blobs." >&2; exit 2; }
done

GLUE=$SP/glue
HAL=$SP/hal/components

# ---------------------------------------------------------------------------
# The eight objects.
#
# Each of these is src/rtems-esp-wifi/tools/<name>-build.sh with $SP no longer
# hardcoded and iram-prefix corrected to wifi-prefix.  adapter.o has never had
# a script at all; its include list is the one rtems_wifi_os_adapter.c's
# headers require -- esp_wifi, esp_common, esp_event, esp_hw_support, soc and
# esp_rom -- and it is the shortest of the eight.
#
# EXTRA_CFLAGS reaches only wifi_init_rtems.o, which is where the RTEMS_WIFI_*
# knobs (RTEMS_WIFI_CAL_MODE, RTEMS_WIFI_DEBUG_PROBE, RTEMS_WIFI_CLK_ABSOLUTE)
# are read.  It is NOT the same EXTRA_CFLAGS that build-esp32c3-micropython.sh
# takes: that one carries -DMP_HEAP_SIZE and the credentials, which belong to
# the application and not to any object here.
# ---------------------------------------------------------------------------
echo "stage: compiling the glue objects ..."

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include" -I "$GLUE/include/rtems-esp" \
  -I "$HAL/log/include" -I "$HAL/esp_common/include" \
  -I "$HAL/esp_rom/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" \
  -Wall -Wextra -Werror -c -o "$SP/rtems_esp_glue.o" \
  "$GLUE/src/rtems_esp_glue.c"

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" \
  -I "$HAL/esp_wifi/include" \
  -c -o "$SP/ftm.o" "$HAL/esp_wifi/src/ftm_load_calibration.c"

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" -I "$GLUE/include" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_wifi/include/local" \
  -I "$HAL/esp_common/include" -I "$HAL/esp_event/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" -I "$HAL/esp_rom/include" \
  -c -o "$SP/reg.o" "$HAL/esp_wifi/regulatory/esp_wifi_regulatory.c"

# adapter.o -- the OS-adapter table the blobs call into.  No -build.sh existed.
$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" -I "$GLUE/include" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_common/include" \
  -I "$HAL/esp_event/include" -I "$HAL/esp_hw_support/include" \
  -I "$HAL/soc/include" -I "$HAL/soc/esp32c3/include" \
  -I "$HAL/esp_rom/include" \
  -Wall -c -o "$SP/adapter.o" "$GLUE/src/rtems_wifi_os_adapter.c"

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" -I "$GLUE/include" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_wifi/include/local" \
  -I "$HAL/esp_common/include" -I "$HAL/esp_event/include" \
  -I "$HAL/log/include" -I "$HAL/esp_phy/include" -I "$HAL/esp_timer/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" -I "$HAL/esp_rom/include" \
  -Wall ${EXTRA_CFLAGS:-} -c -o "$SP/wifi_init_rtems.o" \
  "$GLUE/src/rtems_esp_wifi_init.c"

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" -I "$GLUE/include" \
  -I "$HAL/esp_phy/esp32c3/include" -I "$HAL/esp_phy/include" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_common/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" -I "$HAL/esp_rom/include" \
  -I "$HAL/log/include" \
  -c -o "$SP/phy_init_data.o" "$HAL/esp_phy/esp32c3/phy_init_data.c"

$CC -march=rv32imc -mabi=ilp32 -isystem "$LIB/include" \
  -I "$GLUE/include" -I "$GLUE/include/rtems-esp" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_common/include" \
  -I "$HAL/esp_event/include" -I "$HAL/log/include" \
  -I "$HAL/esp_rom/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/esp_timer/include" \
  -Wall -c -o "$SP/esp_event_rtems.o" "$GLUE/src/rtems_esp_event.c"

# -DESP_PLATFORM is what gates the u8/u16/u32 typedefs in src/utils/common.h,
# and ESP-IDF defines it for every component rather than per-file.
$CC -march=rv32imc -mabi=ilp32 -DESP_PLATFORM -isystem "$LIB/include" \
  -I "$GLUE/include/rtems-esp" -I "$GLUE/include" \
  -I "$HAL/wpa_supplicant/src" -I "$HAL/wpa_supplicant/src/utils" \
  -I "$HAL/wpa_supplicant/include" -I "$HAL/wpa_supplicant/port/include" \
  -I "$HAL/esp_wifi/include" -I "$HAL/esp_common/include" \
  -I "$HAL/log/include" -I "$HAL/esp_event/include" \
  -I "$HAL/esp_rom/include" -I "$HAL/soc/include" \
  -I "$HAL/soc/esp32c3/include" \
  -I "$HAL/esp_hw_support/include" -I "$HAL/esp_timer/include" \
  -c -o "$SP/wpa_common.o" "$HAL/wpa_supplicant/src/utils/common.c"

for o in rtems_esp_glue ftm reg adapter wifi_init_rtems phy_init_data \
         esp_event_rtems wpa_common; do
  [ -f "$SP/$o.o" ] || { echo "stage: $o.o was not produced." >&2; exit 2; }
done

# ---------------------------------------------------------------------------
# The two archives.
#
# These are the slow part -- 340 compiles between them -- so they are rebuilt
# only when something changed.  The existing scripts take TOP and SP from the
# environment and already name wifi-prefix, so they are invoked rather than
# reimplemented: they carry the reasoning for which sources are skipped and
# why skipping is safe, and duplicating that here would let the two drift.
# ---------------------------------------------------------------------------
newer_source_than() {  # newer_source_than <archive> <tree>...
  a=$1; shift
  [ -f "$a" ] || return 0
  [ -n "$(find "$@" -name '*.c' -newer "$a" -print -quit 2>/dev/null)" ]
}

if [ "${RESTAGE:-0}" = 1 ] || \
   newer_source_than "$SP/libwpa.a" "$REPO/src/esp-hal-3rdparty/components/wpa_supplicant"
then
  echo "stage: building libwpa.a (this is the slow one) ..."
  SP=$SP TOP=$REPO sh "$REPO/src/rtems-esp-wifi/tools/supplicant-build.sh"
else
  echo "stage: libwpa.a is up to date ($(ls -l "$SP/libwpa.a" | awk '{print $5}') bytes)"
fi

if [ "${RESTAGE:-0}" = 1 ] || \
   newer_source_than "$SP/libmbedtls.a" "$REPO/src/mbedtls"
then
  echo "stage: building libmbedtls.a ..."
  SP=$SP TOP=$REPO sh "$REPO/src/rtems-esp-wifi/tools/mbedtls-build.sh"
else
  echo "stage: libmbedtls.a is up to date ($(ls -l "$SP/libmbedtls.a" | awk '{print $5}') bytes)"
fi

# ---------------------------------------------------------------------------
# The frozen Python for the MicroPython image.
#
# tools/build-esp32c3-micropython.sh compiles $SP/mpwifi/pywifi.c and puts
# $SP/mpwifi on its include path, and nothing in the tree generated either.
# ports/rtems/Makefile has pytest.c and pynet.c rules but no pywifi one, so
# this follows the same pattern: substitute the credentials into wifi.py, tar
# it, and turn the tar into a C array.
#
# The placeholders in the checked-in wifi.py are literal __SSID__ and __KEY__.
# Credentials stay on the command line and out of the tree, as everywhere else
# here; with none given the image builds and links and will simply not find
# the network, which is the right default for a build that is only being
# proved to link.
# ---------------------------------------------------------------------------
MPWIFI_SRC=$REPO/src/micropython/ports/rtems/examples/wifi/wifi.py
if [ -f "$MPWIFI_SRC" ]; then
  command -v rtems-bin2c >/dev/null 2>&1 || {
    echo "stage: rtems-bin2c is not on PATH; it comes with the RTEMS tools" >&2
    echo "stage: in \$HOME/rtems/7/bin." >&2; exit 2; }
  echo "stage: freezing wifi.py into $SP/mpwifi ..."
  mkdir -p "$SP/mpwifi"
  sed -e "s/__SSID__/${WIFI_SSID:-rtems-esp}/" \
      -e "s/__KEY__/${WIFI_PASSWORD:-}/" \
      "$MPWIFI_SRC" > "$SP/mpwifi/wifi.py"
  tar -C "$SP/mpwifi" -cf "$SP/mpwifi/pywifi.tar" wifi.py
  rtems-bin2c "$SP/mpwifi/pywifi.tar" "$SP/mpwifi/pywifi.c" >/dev/null
  [ -f "$SP/mpwifi/pywifi.c" ] || {
    echo "stage: rtems-bin2c produced no pywifi.c." >&2; exit 2; }
  [ -n "${WIFI_SSID:-}" ] || echo "stage:   (no WIFI_SSID given -- re-run with" \
    "WIFI_SSID=... WIFI_PASSWORD=... to join a real network)"
else
  echo "stage: src/micropython is not checked out, so \$SP/mpwifi was skipped."
  echo "stage: git submodule update --init --checkout --depth=1 src/micropython"
fi

# ---------------------------------------------------------------------------
# libmicropython.a.  Checked, not built: see the header.  It is a make in
# another tree whose invocation docs/handoff.md still flags as reconstructed,
# and WLAN=1 is the part that is silently wrong when omitted -- the Makefile
# then drops modnetwork.o and the `network` module the example imports is
# simply absent, which surfaces as an ImportError at runtime rather than as a
# link error.
# ---------------------------------------------------------------------------
MPLIB=$REPO/src/micropython/ports/rtems/build/libmicropython.a
if [ ! -f "$MPLIB" ]; then
  echo
  echo "stage: $MPLIB is missing.  Everything else is staged; build it with:"
  echo
  echo "    cd $REPO/src/micropython/ports/rtems && mkdir -p build"
  echo "    RTEMS_PREFIX=$SP/usb-prefix RTEMS_VERSION=7 RTEMS_BSP=riscv/esp32c3db \\"
  echo "    WLAN=1 \\"
  echo "    WLAN_CFLAGS=\"-I $REPO/src/rtems-esp-wifi/include \\"
  echo "      -I $REPO/src/rtems-esp-wifi/include/rtems-esp \\"
  echo "      -I $SP/hal/components/esp_wifi/include \\"
  echo "      -I $SP/hal/components/esp_wifi/include/local \\"
  echo "      -I $SP/hal/components/esp_common/include \\"
  echo "      -I $SP/hal/components/esp_event/include\" \\"
  echo "    LWIP_CFLAGS=\"-isystem $SP/wifi-prefix/riscv-rtems7/esp32c3db/lib/include\" \\"
  echo "      make"
  echo
  echo "stage: WLAN=1 is required, not optional."
fi

# ---------------------------------------------------------------------------
# What is there.
# ---------------------------------------------------------------------------
echo
echo "=== $SP ==="
for f in rtems_esp_glue.o ftm.o reg.o adapter.o wifi_init_rtems.o \
         phy_init_data.o esp_event_rtems.o wpa_common.o \
         libwpa.a libmbedtls.a \
         blobs/libnet80211.a blobs/libpp.a blobs/libcore.a blobs/libphy.a \
         blobs/libmesh.a blobs/libespnow.a blobs/libsmartconfig.a \
         blobs/libwapi.a \
         glue/src/rtems_esp_glue.c hal/components/esp_wifi/include/esp_wifi.h \
         wifi-prefix/riscv-rtems7/esp32c3db/lib/liblwip.a \
         usb-prefix/riscv-rtems7/esp32c3db/lib/librtemsbsp.a \
         mpwifi/pywifi.c
do
  if [ -e "$SP/$f" ]; then
    printf '  %-56s %s\n' "$f" "$(ls -l "$SP/$f" | awk '{print $5}')"
  else
    printf '  %-56s %s\n' "$f" "MISSING"
  fi
done
echo
echo "stage: next --"
echo "    SP=$SP EXTRA_CFLAGS=-DMP_HEAP_SIZE=12288 $REPO/tools/build-esp32c3-micropython.sh"
