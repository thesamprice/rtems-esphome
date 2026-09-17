# rtems-esphome

Running [ESPHome](https://esphome.io) on [RTEMS](https://www.rtems.org),
starting from the ESP32-C3.

ESPHome runs on RTEMS on the ESP32-C3 under emulation, and the WiFi radio now
works on real hardware. The two have not been joined yet: no ESPHome image has
been run on a board with a network.

- [`docs/architecture.md`](docs/architecture.md) — the standing decisions: what
  NASA OSAL is and is not used for, why networking goes straight to BSD sockets,
  which BSP comes first, and what counts as success.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — the rules that keep commits from
  undoing those decisions.
- The issues and milestones describe the route.

## Why the ESP32-C3

RTEMS has no Xtensa port, so the ESP32, ESP32-S2 and ESP32-S3 are out of reach
no matter what the tooling does. Espressif's RISC-V parts are the whole of the
field, and RTEMS has one BSP for them: `riscv/esp32c3db`, upstream in
`bsps/riscv/esp32`. Espressif's QEMU fork ships an `esp32c3` machine, so there
is somewhere to run it without hardware in the loop.

## What works today

| | |
|---|---|
| RTEMS builds for `riscv/esp32c3db` | 632 test executables, `riscv-rtems7-gcc` 15.2.0 |
| Boots under QEMU | ROM direct boot, console on UART0 |
| Clock, interrupts | working — after the QEMU fix below |
| Testsuite | 420 pass, 14 xfail, 25 fail of 459 |
| **ESPHome under QEMU** | nine configurations in CI: reference-node, primitives, scheduler, gpio, i2c, spi, uart, opendrain, sensor |
| **WiFi on real hardware** | scan, WPA2-PSK association, DHCP lease, socket — driven from MicroPython |

632 executables build; 459 are run. The difference is tests the runner skips
by design -- interactive ones, and those needing a second processor.
`docs/esp32c3-bsp.md` reconciles the two numbers.

Nothing in the failures is unexplained. 24 are the part being small — 320 KiB
of RAM is not enough for the filesystem tests to allocate a RAM disk — and
`ttest01` fails on every architecture upstream. `docs/esp32c3-bsp.md` has the
full breakdown, including three readings of these numbers that were confidently
wrong before they were right.

The testsuite numbers above are QEMU. The WiFi row is a board: an
ESP32-C3-DevKitM associating with a WPA2-PSK access point and holding a DHCP
lease. `docs/handoff.md` is the route from a clean clone to that result, and
`docs/esp32c3-jtag-debugging.md` is how to halt the board when it stops.

What remains is the join: ESPHome on the board, over that radio.

## What it took

Four changes, all written up in `docs/esp32c3-bsp.md`. Two are QEMU defects,
carried as commits on the `esp32c3-rtems-fixes` branch of the `src/esp-qemu`
fork and both worth sending upstream. One is an RTEMS BSP defect. One is a
backport of a fix that is already upstream:

**`src/esp-qemu` commit `3d3909d`** — QEMU's ESP32-C3 interrupt
matrix does not implement `INTERRUPT_CORE0_INTR_STATUS_0` and `_1`. Those are
how software finds out which peripheral raised a CPU interrupt line, since the
matrix maps 62 sources onto 31 lines. Without them every interrupt dispatches
as the invalid vector and the first clock tick is a fatal spurious interrupt,
so nothing past `hello` runs. Four lines; the model already keeps the value.

**`src/esp-qemu` commit `69094f5`** — QEMU's systimer
converted elapsed nanoseconds to counter ticks with an integer division and then
discarded the remainder. A tick is 62.5 ns and every guest read is an update, so
the counter ran slow in proportion to how often software looked at it. Five
tests move from fail to pass.

**`src/rtems` — the `psxstat` statvfs expectation** — a backport, not a
local fix. IMFS gained a real `statvfs` handler upstream in 2025 and `psxstat`
was not updated until 2026-07-29; the RTEMS revision used here is pinned between
those two commits, so the test fails on every BSP. Drop it when the pin advances.

**`src/rtems` commit `faf01c5`** — the BSP declared the
system timer at `16 * 1024 * 1024` Hz under a comment saying 16 MHz. It is 16
MHz exactly. The error cancels inside the OS, because the same constant sets
the tick period and converts it back, which is why the testsuite barely
notices — but it made every RTEMS second 4.86% off a real one.

## Reproducing it

Needs an RTEMS 7 RISC-V toolchain (`riscv-rtems7-gcc`), and for QEMU: ninja,
python3, pkg-config, glib and libgcrypt >= 1.8.

```sh
git clone https://github.com/thesamprice/rtems-esphome
cd rtems-esphome

# QEMU.  src/esp-qemu is a fork carrying both fixes as commits, so there is
# nothing to patch.  ~10 minutes.
scripts/build_esp_qemu.sh

# RTEMS, wherever your checkout is
cd /path/to/rtems
./waf configure -o build-esp32c3db \
    --rtems-config=/path/to/rtems-esphome/config_esp32c3db.ini \
    --rtems-tools=$HOME/rtems/7 --prefix=$HOME/rtems/7
./waf build

# and the suite
cd /path/to/rtems-esphome
RTEMS_BUILD=/path/to/rtems/build-esp32c3db/riscv/esp32c3db \
    tools/esp32c3-run-tests.sh
```

`config_esp32c3db.ini` turns the USB Serial/JTAG console off, which is not
optional under QEMU — that peripheral is modelled as a stub whose reads return
0, and the BSP's output routine spins waiting for a bit the stub never sets, so
the first character printed would hang the run.

## What ESPHome needs that is not here

ESPHome compiles a YAML device description into C++ and builds it against
ESP-IDF or Arduino. Retargeting that at RTEMS is not one job, and none of it is
started. The good news, and the reason this is tractable at all, is that
ESPHome already carries most of the seams — a platform-selected core HAL,
abstract GPIO, a virtual UART interface, an `I2CBus` reduced to one virtual
transaction, preferences behind a contract, a BSD socket backend, and a
per-platform main-loop wake dispatcher. Host and Zephyr backends already
replace FreeRTOS semantics rather than emulating them.

What is missing:

- **A network stack.** ESPHome's API, OTA and MQTT are all TCP. RTEMS has two
  options, `rtems-libbsd` and `rtems-lwip`; on a part with 320 KiB, lwIP is the
  realistic one. Espressif maintain their own lwIP fork that ESP-IDF builds
  against, so there are two vendors' forks of one stack to reconcile.
- **Wi-Fi.** Espressif's driver is a binary blob with an ESP-IDF-shaped
  interface around it, and QEMU does not emulate the radio at all. Whether the
  first milestone is wired networking under emulation or real hardware is an
  open question, and probably the most important one here.
- **The ESP-IDF surface ESPHome uses** — GPIO, I2C, SPI, timers, NVS. Some maps
  onto RTEMS drivers, some does not exist for this BSP yet.
- **A build path.** ESPHome generates a PlatformIO project. RTEMS builds with
  waf. Something has to bridge those.
- **Memory.** 320 KiB of RAM, and the testsuite already shows where that bites.

## Layout

```
config_esp32c3db.ini      RTEMS BSP config for riscv/esp32c3db
docs/handoff.md           clean clone to a running board, in order
docs/architecture.md      standing decisions for the port
docs/esp32c3-bsp.md       how the BSP boots, what QEMU gets wrong, test results
docs/esp32c3-jtag-debugging.md  halting a wedged C3 over its built-in USB-JTAG
scripts/build_esp_qemu.sh build the emulator
scripts/manifest.sh       what is checked out vs what is recorded
tests/rtems-ci/           ESPHome configs run on RTEMS, and their assertions
tests/esp-idf-ci/         the same configs on ESP-IDF, as the A side of an A/B
tools/esp32c3-run-tests.sh        the RTEMS testsuite under QEMU
tools/rtems-ci-run.sh             one ESPHome/RTEMS image under QEMU
tools/stage-esp32c3-workdir.sh    fill a fresh work directory
tools/build-esp32c3-micropython.sh  build the MicroPython WiFi image
tools/esp32c3-sample-hung.py      halt a stuck board and say where it is
src/esp-qemu              Espressif QEMU + our two fixes, as commits
src/esphome               the thing being ported
src/rtems                 RTEMS, forked: the ESP32-C3 BSP work
src/rtems-esp-wifi        RTEMS/Espressif WiFi glue, netif and examples
src/rtems-lwip            the network stack, sized for a 272 KiB part
src/micropython           the RTEMS port, and the WiFi example
src/osal                  NASA OSAL
src/rtems-libbsd          kept for comparison, not used
```

Each submodule is a fork whose branch carries the changes this port needs, so
a correct checkout is a correct tree and there is nothing to apply.
`scripts/manifest.sh` reports what is checked out against what is recorded and
fails on drift. Most are `update = none` and are not fetched by a plain clone;
`docs/handoff.md` says which, and which need an ssh key.

## Licence

BSD-2-Clause, see `LICENSE.md`. The forked submodules carry their own
projects' terms where they apply -- RTEMS (BSD-2-Clause), QEMU
(GPL-2.0-or-later), MicroPython (MIT).
