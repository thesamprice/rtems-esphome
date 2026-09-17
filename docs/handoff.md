# Hand-off: building and running this from nothing

For someone who has never seen this tree. Follow it in order. Where a step
could not be reconstructed from the repository it says so instead of guessing.

**Read this first.** There is no build system. The working procedure is shell
scripts that stage a work directory by hand, compile objects one at a time and
link them with an explicit command line. One is now committed
(`tools/build-esp32c3-micropython.sh`); the C examples still build from their
own scripts under `src/rtems-esp-wifi/examples/`. That is issue **#100**, only
partly addressed. Expect to read the scripts, not just run them.

## What this is

A port of [ESPHome](https://esphome.io) to [RTEMS](https://www.rtems.org),
targeting the Espressif ESP32-C3. RTEMS has no Xtensa port, so Espressif's
RISC-V parts are the only reachable ones, and RTEMS has exactly one BSP for
them: `riscv/esp32c3db`. Alongside that the repository carries the thing
ESPHome cannot run without: `src/rtems-esp-wifi`, glue between RTEMS and
Espressif's binary WiFi libraries — an OS-adapter table, a PHY bring-up path,
an event task, and an lwIP netif over `esp_wifi_internal_tx()`.

### What currently works

On **real ESP32-C3 hardware**, driven from a MicroPython script: the radio
initialises and the PHY calibrates; `wlan.scan()` returns a real access-point
list with BSSID, channel, RSSI and auth mode; `wlan.connect()` associates with
a WPA2-PSK network through the supplicant; DHCP returns a lease; and a socket
opens on the leased network.

Under **QEMU**: RTEMS boots and 420 of 459 testsuite executables pass. The
`wifi-net` C image links and runs and reaches `esp_wifi_internal_tx()`, which
refuses — QEMU's `esp32c3` machine has **no WiFi MAC model** (#102), so no
frame can move and nothing can associate. Association, DHCP and traffic are
hardware-only questions.

ESPHome itself is not yet running on RTEMS. What is proven is the layer below
it. `README.md` is stale: it predates the WiFi work and says nothing runs on
silicon.

| file | what it covers |
|---|---|
| `docs/esp32c3-bsp.md` | how the BSP boots, direct boot, QEMU defects, test results |
| `docs/architecture.md` | the standing decisions for the port |
| `docs/esp32c3-jtag-debugging.md` | halting a wedged C3 over its built-in USB-JTAG |
| `src/rtems-esp-wifi/docs/step*.md` | the WiFi bring-up, step by step, with the failures |

## Hardware and host prerequisites

An ESP32-C3-DevKitM, or any ESP32-C3 board. Which USB the board presents
decides one BSP option and nothing else:

* USB-to-UART bridge (CP210x, CH34x, FTDI) → `ESPRESSIF_USE_USB_CONSOLE = False`
* native USB-Serial-JTAG, `/dev/cu.usbmodem*` → `ESPRESSIF_USE_USB_CONSOLE = True`

With the wrong one the image boots perfectly and prints nothing, which looks
exactly like a dead board. `src/rtems-esp-wifi/tools/hw-flash-and-run.sh`
reports which it found rather than assuming.

Host is macOS arm64. Needed: a C toolchain, `ninja`, `python3`, `pkg-config`,
`glib` and `libgcrypt >= 1.8` for QEMU; `screen` for console capture; and, only
for JTAG, Espressif's OpenOCD build — mainline 0.12 does not work against chip
revision v0.4. The scripts hardcode `.venv/bin/esptool`:

```sh
python3 -m venv .venv
.venv/bin/pip install esptool pyusb pyserial
.venv/bin/esptool version          # 5.4.0 is what has been used
```

esptool 5 spells subcommands with hyphens (`write-flash`, `chip-id`); esptool 4
used underscores, and this tree uses the hyphenated form. There is no
`requirements.txt` — **open question**: the package list above is what the
scripts actually invoke, not a pinned manifest recovered from the repo.

## Getting the source

```sh
git clone https://github.com/thesamprice/rtems-esphome
cd rtems-esphome
```

Every submodule is `update = none`, so a plain clone fetches **none** of them:

```sh
git submodule update --init --checkout --depth=1 \
    src/rsb src/rtems src/esp-qemu src/rtems-esp-wifi \
    src/esp-hal-3rdparty src/esp32-wifi-lib src/esp-phy-lib src/mbedtls

# rtems-lwip has submodules of its own that ./waf configure needs, and they
# come from three different hosts
git submodule update --init --checkout --depth=1 --recursive src/rtems-lwip
```

`scripts/manifest.sh` reports what is checked out against what `.gitmodules`
records, and fails if a fetched submodule has drifted off its pin.

| submodule | purpose |
|---|---|
| `src/rtems` | RTEMS itself; `main` *is* the 7 development line |
| `src/rsb` | RTEMS Source Builder — produces the cross toolchain |
| `src/esp-qemu` | Espressif's QEMU fork plus two ESP32-C3 fixes, as commits |
| `src/esphome` | the thing being ported, forked so the RTEMS backend is commits |
| `src/rtems-lwip` | the network stack (chosen over libbsd in #50: 391 files vs 5,602) |
| `src/rtems-libbsd` | kept for comparison, not used — 2.6M lines on a 320 KiB part |
| `src/rtems-esp-wifi` | the RTEMS↔Espressif WiFi glue, netif and examples |
| `src/esp-hal-3rdparty` | Espressif's HAL sources, branch `sync/master.c` |
| `src/esp32-wifi-lib` | the eight WiFi binary blobs for `esp32c3/` |
| `src/esp-phy-lib` | the PHY blob |
| `src/mbedtls` | crypto for the WPA supplicant |
| `src/osal` | NASA OSAL — a selective building block, see `docs/architecture.md` |
| `src/ArduinoJson` | header-only, needed by esphome's `json` component |

**`src/micropython` is not a submodule** (issue **#125**). Clone it by hand
from the fork, branch `rtems-networking`:

```sh
git clone -b rtems-networking \
    https://github.com/thesamprice/micropython.git src/micropython
```

The same branch is also on gitlab.rtems.org as `TheSamPrice/micropython`; the
two are the same commits. `thesamprice/micropython#1` is a pull request from
that branch onto `rtems-port-base` (the tip of the pre-existing RTEMS port)
purely so the delta is reviewable in one place -- seven commits, 14 files,
+2289/-8. It is not proposed upstream and is not meant to be merged.

## Building the toolchain

RSB, into `$HOME/rtems/7`. That path is not freely chosen: the scripts export
`PATH=$HOME/rtems/7/bin:$PATH`, the tool prefix is `riscv-rtems7-`, and the
prefix is baked into the compiler. Apply the patches first.

```sh
scripts/apply_patches.sh rtems rsb

cd src/rsb/rtems
../source-builder/sb-set-builder --prefix=$HOME/rtems/7 7/rtems-riscv
```

Tens of minutes. On failure the RSB prints only "Build FAILED" and writes the
real error into `rsb-report-*.txt`; read that before guessing. Verify with
`riscv-rtems7-gcc --version` — 15.2.0 is what was used.

CI does the same in `.github/Dockerfile.toolchain` and publishes the installed
prefix as a release tarball, so a machine that does not want an hour of RSB can
untar one instead.

## Building the BSP

The BSP is upstream RTEMS plus `patches/rtems/`, which add the GPIO, I2C and
UART drivers and fix a chip header's install path and the systimer frequency.
**Install, do not merely build**: the `.pc` file downstream reads is produced by
`./waf install`, and a built-but-not-installed BSP fails later with "no
pkg-config file", the least obvious symptom in this chain. Two prefixes are
needed, differing only in the console driver:

```sh
SP=$HOME/build/c3        # the work directory used throughout; outside the tree
mkdir -p "$SP"
cd src/rtems

# UART console + WiFi IRAM.  This is the one lwIP is built into.
./waf configure -o build-wifi \
    --rtems-config=../rtems-esp-wifi/examples/wifi-init/wifi-bsp.ini \
    --rtems-tools=$HOME/rtems/7 --prefix=$SP/wifi-prefix
./waf build && ./waf install

# USB-Serial-JTAG console + WiFi IRAM.
./waf configure -o build-usb \
    --rtems-config=../rtems-esp-wifi/examples/wifi-init/wifi-bsp-usb.ini \
    --rtems-tools=$HOME/rtems/7 --prefix=$SP/usb-prefix
./waf build && ./waf install
```

Three options in those `.ini` files are the whole difference from stock:

* **`ESPRESSIF_USE_USB_CONSOLE`** — `False` selects UART0 through the ROM
  routines, which is what QEMU's `-serial` reaches and what a bridge-chip board
  wants. Under QEMU `True` is fatal, not merely useless: that peripheral is a
  stub whose reads return 0 and the BSP's output routine spins on a bit the
  stub never sets, so the first character printed hangs the run.
* **`ESP32C_IRAM_REGION_SIZE = 0xC000`** (48 KiB) — SRAM reached through the
  instruction bus, for code that must not execute from flash; the blobs put
  ~44 KiB of `.wifi*iram`/`.iram1` there. One SRAM, two windows, so every byte
  here comes out of the heap — the non-WiFi config leaves it at `0x2000`.
* **`ESP32C_CODE_REGION_SIZE = 0x180000`** (1.5 MiB) — the C example fits in the
  default 1 MiB with ~70 KiB spare; the MicroPython one overflows it by ~290 KiB
  once the interpreter is linked beside the blobs, lwIP and mbedTLS. The room is
  there because this is a direct-boot image: 4 MiB of flash and nothing
  reserved after it.

Then rtems-lwip, configured *against* the UART prefix and installed back into
it, so `liblwip.a` lands in `$SP/wifi-prefix/riscv-rtems7/esp32c3db/lib/` (the
link scripts check for that file by name and refuse to start without it):

```sh
cd ../rtems-lwip
./waf configure --rtems-tools=$HOME/rtems/7 --rtems=$SP/wifi-prefix \
    --rtems-bsp=riscv/esp32c3db --prefix=$SP/wifi-prefix
./waf build && ./waf install
```

lwIP goes into `wifi-prefix` only. `usb-prefix` has none, which is why the link
scripts carry two prefix variables and take the BSP from one and lwIP from the
other — the two BSPs' `bspopts.h` are byte-identical apart from the console, so
rebuilding lwIP against the USB one would produce the same archive.

## Staging the rest of the work directory

`$SP` must also hold the glue checkout, the HAL, the blobs and about eight
compiled objects before either example will link:

```sh
cp -R src/rtems-esp-wifi     "$SP/glue"
cp -R src/esp-hal-3rdparty   "$SP/hal"
mkdir -p "$SP/blobs"
cp src/esp32-wifi-lib/esp32c3/*.a  "$SP/blobs/"
cp src/esp-phy-lib/esp32c3/libphy.a "$SP/blobs/"   # separate repo
```

The link expects, in `$SP`: `rtems_esp_glue.o`, `ftm.o`, `reg.o`, `adapter.o`,
`wifi_init_rtems.o`, `phy_init_data.o`, `esp_event_rtems.o`, `wpa_common.o`,
`libwpa.a`, `libmbedtls.a`, and
`blobs/lib{net80211,pp,core,phy,mesh,espnow,smartconfig,wapi}.a`. The scripts
that produce each are in `src/rtems-esp-wifi/tools/` — `build-glue.sh`,
`ftm-build.sh`, `reg-build.sh`, `event-build.sh`, `init-build.sh`,
`phy-data-build.sh` and `wpa-common-build.sh` are each a single
`riscv-rtems7-gcc -c` with a long include list; `supplicant-build.sh` and
`mbedtls-build.sh` compile a whole tree and `ar rcs` it into `libwpa.a` (131
objects) and `libmbedtls.a` (62). There is no script for `adapter.o`
(`rtems_wifi_os_adapter.c`) — it is the same one-line compile with the
`esp_wifi`, `esp_common`, `esp_event`, `esp_hw_support`, `soc` and `esp_rom`
include paths.

> **Open question.** Nothing populates a fresh `$SP` end to end, and those
> per-object scripts still point at an older `$SP/iram-prefix` rather than
> `$SP/usb-prefix`. They have to be run one at a time with the prefix
> corrected. This is the sharpest edge of #100 and the most likely place for a
> newcomer to stop.

## Building and flashing the C WiFi example

`src/rtems-esp-wifi/examples/wifi-net` is the C image: WiFi libraries, netif and
lwIP in one binary, with assertions about what linked and what ran.

```sh
SP=$HOME/build/c3 src/rtems-esp-wifi/examples/wifi-net/build-and-run.sh
```

It links, checks that `esp32c3_netif_add` is `T` and not `W` (a `W` means the
weak do-nothing default inside `liblwip.a` won and the image has no interface)
and that `tcpip_input` is present, prints a memory report, builds an eFuse
image, runs the result under QEMU and cats the log. Output is in
`$SP/wifi-net-out/`.

Credentials are compile-time, and the defaults are a placeholder SSID and an
*empty* password — deliberately, since an empty password takes the open-AP
path, the only one that does anything in emulation. For hardware, override
both:

```sh
SP=$HOME/build/c3 \
EXTRA_CFLAGS='-DWIFI_NET_SSID="your-ssid" -DWIFI_NET_PASSWORD="your-passphrase"' \
  src/rtems-esp-wifi/examples/wifi-net/build-and-run.sh
```

Flash the **raw** image, not the 4 MiB padded `flash.bin` — the padding exists
only because QEMU's `-drive if=mtd` wants a whole device:

```sh
src/rtems-esp-wifi/tools/hw-flash-and-run.sh $SP/wifi-net-out/flash.raw
```

That runs `esptool ... write-flash 0x0 <image>`, then opens `screen` on the port
logging to `hw-run.log`. Offset 0 with no bootloader and no partition table is
correct: this is a **direct-boot** image (#103) — `start.S` emits the two
`0xaedb041d` words the ROM looks for at offset 0 and the ROM jumps to
`0x42000008`. It needs no esptool *format*, but it still needs esptool to be
*written*.

On a blank board the C3 resets in a loop and USB never settles; download mode
stops that, but only until something is written.
`src/rtems-esp-wifi/tools/flash-c3.sh` polls four times a second and flashes
within a quarter second of a port appearing, because the window is short enough
to lose by hand:

```sh
SP=$HOME/build/c3 src/rtems-esp-wifi/tools/flash-c3.sh usb
# then: hold BOOT, tap RST, release BOOT
```

## Building and flashing the MicroPython WiFi example

This is the image that actually joins a network. Build MicroPython's library
first; the port `Makefile` takes the BSP and prefix from the environment:

```sh
cd src/micropython/ports/rtems
mkdir -p build
RTEMS_PREFIX=$SP/usb-prefix RTEMS_VERSION=7 RTEMS_BSP=riscv/esp32c3db \
WLAN=1 \
WLAN_CFLAGS="-I $PWD/../../../rtems-esp-wifi/include \
  -I $PWD/../../../rtems-esp-wifi/include/rtems-esp \
  -I $SP/hal/components/esp_wifi/include \
  -I $SP/hal/components/esp_wifi/include/local \
  -I $SP/hal/components/esp_common/include \
  -I $SP/hal/components/esp_event/include" \
LWIP_CFLAGS="-isystem $SP/wifi-prefix/riscv-rtems7/esp32c3db/lib/include" \
  make
```

That leaves `src/micropython/ports/rtems/build/libmicropython.a`. **`WLAN=1` is
required** — without it the Makefile omits `modnetwork.o` and the `network`
module the example imports is simply absent.

> **Open question.** The literal invocation is not recorded anywhere. The form
> above is reconstructed from the port `Makefile`'s own documented
> `WLAN_CFLAGS`/`LWIP_CFLAGS` variables and from the include paths the built
> `build/modnetwork.P` records, both of which agree with it. Check it against
> the `Makefile` before trusting it.

The Python the image runs is `ports/rtems/examples/wifi/wifi.py`, frozen in as a
tar. It carries `SSID = "__SSID__"` and `KEY = "__KEY__"` as literal
placeholders, substituted at staging time and converted to a C array with
`rtems-bin2c`, the way the port's `Makefile` does for its other examples:

```sh
mkdir -p "$SP/mpwifi"
sed -e 's/__SSID__/your-ssid/' -e 's/__KEY__/your-passphrase/' \
    src/micropython/ports/rtems/examples/wifi/wifi.py > "$SP/mpwifi/wifi.py"
tar -C "$SP/mpwifi" -cf "$SP/mpwifi/pywifi.tar" wifi.py
rtems-bin2c "$SP/mpwifi/pywifi.tar" "$SP/mpwifi/pywifi.c"
```

> **Open question.** That substitution is not automated anywhere: the
> `ports/rtems/Makefile` has `pytest.c` and `pynet.c` rules but no `pywifi` one,
> and no script in the tree generates `$SP/mpwifi/pywifi.c`. The three commands
> above are reconstructed from the Makefile's own pattern and from the staged
> file, which does have the placeholders replaced. Verify before relying on
> them.

Then the image. This is the one committed build script:

```sh
SP=$HOME/build/c3 \
EXTRA_CFLAGS='-DMP_HEAP_SIZE=12288 -DWIFI_NET_SSID="your-ssid" -DWIFI_NET_PASSWORD="your-passphrase"' \
  ./tools/build-esp32c3-micropython.sh
```

`REPO` defaults to the directory above the script, so it runs from anywhere in a
clone; override it only for an out-of-tree source checkout. `SP` is required and
must be outside the tree. Credentials are passed on the command line and are
never stored in the repository — keep it that way.

Output is in `$SP/mpwifi-out/`: `mpwifi.exe` (the ELF, for `addr2line` and gdb),
`mpwifi.map`, `flash.raw` and `flash.bin` (the 4 MiB padded image). Despite its
header comment the script does not run QEMU; it stops at the image and the
memory report.

```sh
.venv/bin/esptool --port /dev/cu.usbmodem2101 --after hard-reset \
    write-flash 0x0 $SP/mpwifi-out/flash.bin
```

**`-DMP_HEAP_SIZE=12288` is not optional** (#121). 12 KiB is the tested value
and the default in the example's `main.c`. At 24 KiB about 2 KB of C heap is
left and the radio fails with `ESP_ERR_NO_MEM`, which surfaces not as an
allocation error but as an **empty scan** — `0 access point(s)`, or
`esp_wifi_set_config failed: 257`. At 8 KiB Python starves. Pass it explicitly
so the number is visible in the command that made the image.

## What a successful run looks like

Console at 115200. A real capture, with the network's name and addresses
replaced:

```
rtems-esp-wifi: copied 44924 bytes of IRAM and 496 of DRAM
rtems-esp-wifi: powering the modem domain...
rtems-esp-wifi: cpu on pll, sysclk 000a8400 cpuperconf 0000000d
rtems-esp-wifi: calibrating for MAC ac:a7:04:xx:xx:xx
rtems-esp-wifi: register_chipv7_phy( PHY_RF_CAL_FULL )...
rtems-esp-wifi: no saved calibration, so the PHY calibrated fully
rtems-esp-wifi: PHY registered, esp_wifi_init_internal()...
wifi.pp: pp rom version: 9387209
wifi.net80211: net80211 rom version: 9387209
rtems-esp-wifi: libraries initialised, esp_supplicant_init()...
wifi.osi: rtems_wifi_stub_coex_schm_flexible_period_set is not implemented on RTEMS yet (needs esp_coex)
wifi.osi: rtems_wifi_stub_coex_schm_curr_period_get is not implemented on RTEMS yet (needs esp_coex)
MAC ac:a7:04:xx:xx:xx
scanning...
  6 access point(s)
  your-ssid            aa:bb:cc:00:11:01  ch  6   -34 dBm  WPA2-PSK
  your-ssid Guest      aa:bb:cc:00:11:02  ch  6   -34 dBm  WPA2-PSK
  your-ssid            aa:bb:cc:00:22:01  ch  6   -67 dBm  WPA2-PSK
  your-ssid Guest      aa:bb:cc:00:33:02  ch  6   -73 dBm  WPA2-PSK
connecting to 'your-ssid'...
wifi.osi: rtems_wifi_stub_coex_wifi_channel_set is not implemented on RTEMS yet (needs esp_coex)
wifi.osi: rtems_wifi_stub_coex_wifi_release is not implemented on RTEMS yet (needs esp_coex)
  associated
asking for a DHCP lease...
  address 192.168.4.21  netmask 255.255.252.0  gateway 192.168.4.1
socket created on the leased network: 192.168.4.21

MicroPython drove the radio: scan, join, DHCP, socket.

*** END OF MICROPYTHON WIFI EXAMPLE ***

[ RTEMS shutdown ]
```

The `wifi.osi: ... not implemented` lines are expected and harmless — they are
the coexistence stubs, which matter only with Bluetooth in the picture.

`6 access point(s)` is the line that carries the most. **`0 access point(s)` is
not an empty neighbourhood** — it is how `ESP_ERR_NO_MEM` presents. Read it as
the heap-size symptom (#121) or the warm-reset symptom (#124), not as a radio
that found nothing. `associated` means the four-way handshake completed through
the supplicant; the lease proves frames moved both ways.

## Running under QEMU

Build the emulator once. `src/esp-qemu` carries both ESP32-C3 fixes as commits,
so there is nothing to apply:

```sh
scripts/build_esp_qemu.sh -j 8      # ~10 minutes
# leaves src/esp-qemu/build/qemu-system-riscv32
```

Those fixes are why building beats downloading Espressif's binaries: without
`3d3909d` the interrupt matrix does not implement `INTR_STATUS_0/_1`, so every
interrupt dispatches as the invalid vector and the first clock tick is fatal;
without `69094f5` the systimer discards the remainder of every ns-to-ticks
conversion and the clock runs slow in proportion to how often software reads it.

`examples/wifi-net/build-and-run.sh` runs QEMU itself; its invocation, to drive
an image by hand:

```sh
src/esp-qemu/build/qemu-system-riscv32 -M esp32c3 -display none -monitor none \
  -no-reboot -icount shift=0,sleep=off -serial file:run.log \
  -drive file=flash.bin,if=mtd,format=raw \
  -drive file=efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32c3.efuse,property=drive,value=efuse
```

Use `flash.bin` here, not `flash.raw` — `if=mtd` wants a whole device. Generate
`efuse.bin` with `src/rtems-esp-wifi/examples/wifi-net/efuse.py`; without it the
eFuse reads all zero and the MAC checks pass vacuously. The image must have been
built against `$SP/wifi-prefix`, or the first character printed hangs the run.

The testsuite runs under the same emulator:

```sh
RTEMS_BUILD=src/rtems/build-esp32c3db/riscv/esp32c3db tools/esp32c3-run-tests.sh
```

## Tools

| tool | what it is for |
|---|---|
| `tools/build-esp32c3-micropython.sh` | builds the MicroPython WiFi image into `$SP/mpwifi-out/` |
| `tools/esp32c3-run-tests.sh` | unattended RTEMS testsuite runner under QEMU, with a classified report |
| `tools/rtems-ci-run.sh` | runs one ESPHome/RTEMS image under QEMU and reports PASS or FAIL |
| `tools/esp-idf-ci-run.sh` | the same for an ESPHome/ESP-IDF firmware — the reference lane that catches breakage in common ESPHome code |
| `tools/esp32c3-usbjtag-drain.py` | clears the C3's USB-JTAG IN endpoint after a SIGKILLed OpenOCD wedged it |
| `tools/esp32c3-sample-hung.py` | poor man's profiler: resets over EN, waits for the console to go quiet, then halts repeatedly and symbolizes the PC |
| `tools/qmp-preboot.py` | sends QMP commands to a paused QEMU, then releases the guest — for device state the command line cannot set |
| `scripts/manifest.sh` | what is checked out vs what `.gitmodules` records |

## Debugging

When the console stops printing, coverage cannot say where execution is
sitting; the built-in USB-Serial-JTAG can, over the same cable as the console.
Read `docs/esp32c3-jtag-debugging.md` before trying. It is short, and each of
its three obstacles looks like an unsupported adapter rather than what it is:
mainline OpenOCD is not enough (use Espressif's build with
`board/esp32c3-builtin.cfg`); a SIGKILLed OpenOCD wedges the JTAG endpoint
until `tools/esp32c3-usbjtag-drain.py` drains it; and `reset halt` is not a
neutral way to start, because it issues a software core reset the PHY bring-up
does not survive.

```sh
tools/esp32c3-usbjtag-drain.py
C3_ELF=$SP/mpwifi-out/mpwifi.exe \
C3_OPENOCD=.../openocd-esp32/bin/openocd \
C3_OPENOCD_SCRIPTS=.../openocd-esp32/share/openocd/scripts \
C3_TRIGGER="asking for a DHCP lease" C3_QUIET=12 \
  tools/esp32c3-sample-hung.py
```

Read `mstatus` and the symbol together before concluding anything: a PC parked
in `bsp_reset` with `ra` in `bsp_fatal_extension` and MIE clear is the halt loop
every RTEMS image ends in, including after a clean exit. It means the run
finished, not that it stopped there.

`mstatus`, not `mie`: this target does not expose `mie` or `mip` at all --
OpenOCD answers "register mie not found in current target" -- and an earlier
version of the sampler printed that failed read as `mie=0x00000000`, which
reads as "interrupts are disabled" rather than "not read". MIE is bit 3 of
`mstatus` and the tool decodes it.

## Known rough edges

**#100 — no build system.** Shell scripts staging a work directory by hand. One
is in the tree; the C examples are not, the per-object glue scripts name a stale
prefix, and nothing populates a fresh `$SP` end to end.

**#125 — `src/micropython` is not a submodule.** Cloned by hand from the fork,
branch `rtems-networking`, with nothing checking its revision.

**#121 — the GC heap must be 12 KiB.** `-DMP_HEAP_SIZE=12288`. 24 KiB leaves
~2 KB of C heap and the radio fails with `ESP_ERR_NO_MEM`, reported as an empty
scan; 8 KiB starves Python. Narrow window, and the failure does not name itself.

**#122 — DHCP takes a fixed ~6.8 s.** Every time, not occasionally, and the
figure is deterministic to within milliseconds across runs, which is what gives
it away: it is lwIP's DISCOVER backoff, 2 s + 4 s, for two attempts that got no
answer. The netif raises link-up on `WIFI_EVENT_STA_CONNECTED`, which fires at
*association* -- before the WPA2 four-way handshake has put keys in the
hardware slots -- so the first two DISCOVERs go into a link that cannot carry
encrypted data. Diagnosed, not yet fixed.

**#123, #112 — association fails roughly one run in seven** with `AUTH_EXPIRE`.
Retrying works. The same handshake as #122, failing rather than merely late, so
the two are probably one fault at two severities. The RTEMS clock has been ruled
out by measurement: 100 ticks/s, unchanged either side of the 40 MHz -> 160 MHz
PLL switch that WiFi bring-up performs (`-DRTEMS_TICK_PROBE` in the example).

**#124 — a warm reset leaves the radio dead.** After a software core reset
`esp_wifi_scan` reports zero networks and `esp_wifi_set_config()` answers
`ESP_ERR_NO_MEM`, where the same image reset over EN (`rst:0x15`) scans and
associates normally. So reset over the EN pin, and never start a debug session
with OpenOCD's `reset halt` — it measures a boot the fault never takes. Stock
ESP-IDF's bootloader clears the modem domain on every path; ours depends on
power-on state, and that is a real gap rather than a quirk to work around.

**#103 — the image layout is a cheat.** Direct boot: magic at flash offset 0,
no bootloader, no partition table, no OTA slot. It is why
`ESP32C_CODE_REGION_SIZE` can simply be raised, and why none of the usual ESP32
tooling applies.

**#102 — QEMU has no WiFi MAC model,** so association, receive and DHCP cannot
be exercised under emulation at all.

**A reset over EN lands in the ROM about a quarter of the time.** DTR drives
GPIO9 and RTS drives EN, and a transient leaves the chip in the ROM loader
printing `waiting for download`. That looks identical to firmware hanging before
its first output. `esp32c3-sample-hung.py`'s `reset_into_the_app()` holds DTR
deasserted across the sequence and reads the ROM's own `boot:` line back,
retrying if it says DOWNLOAD.

**`README.md` is stale.** It still says nothing runs on real silicon.
