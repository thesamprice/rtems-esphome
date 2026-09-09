# rtems-esphome

Running [ESPHome](https://esphome.io) on [RTEMS](https://www.rtems.org),
starting from the ESP32-C3.

This is early. What exists today is a working, reproducible foundation — RTEMS
boots on the ESP32-C3 under emulation and passes most of its own testsuite —
plus the fixes it took to get there. ESPHome itself is not running yet.

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
| Testsuite | 414 pass, 14 xfail, 31 fail of 459 |

The failures are mostly the part being small: 320 KiB of RAM is not enough for
the filesystem tests to allocate a RAM disk. `docs/esp32c3-bsp.md` has the full
breakdown, and the open ones are issues here.

Nothing is running on real silicon yet. Everything above is QEMU.

## What it took

Two fixes, both written up in `docs/esp32c3-bsp.md`, both candidates for their
upstreams:

**`patches/esp-qemu/intmatrix-status.patch`** — QEMU's ESP32-C3 interrupt
matrix does not implement `INTERRUPT_CORE0_INTR_STATUS_0` and `_1`. Those are
how software finds out which peripheral raised a CPU interrupt line, since the
matrix maps 62 sources onto 31 lines. Without them every interrupt dispatches
as the invalid vector and the first clock tick is a fatal spurious interrupt,
so nothing past `hello` runs. Four lines; the model already keeps the value.

**`patches/rtems/esp32c3-systimer-frequency.patch`** — the BSP declared the
system timer at `16 * 1024 * 1024` Hz under a comment saying 16 MHz. It is 16
MHz exactly. The error cancels inside the OS, because the same constant sets
the tick period and converts it back, which is why the testsuite barely
notices — but it made every RTEMS second 4.86% off a real one.

## Reproducing it

Needs an RTEMS 7 RISC-V toolchain (`riscv-rtems7-gcc`), and for QEMU: ninja,
python3, pkg-config, glib and libgcrypt >= 1.8.

```sh
git clone --recursive https://github.com/thesamprice/rtems-esphome
cd rtems-esphome

# QEMU, fetched and patched for you.  ~10 minutes.
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
config_esp32c3db.ini    RTEMS BSP config for riscv/esp32c3db
docs/esp32c3-bsp.md     how the BSP boots, what QEMU gets wrong, test results
patches/esp-qemu/       fixes to Espressif's QEMU
patches/rtems/          fixes to RTEMS
scripts/build_esp_qemu.sh
tools/esp32c3-run-tests.sh
src/esp-qemu            Espressif QEMU, submodule, fetched on demand
```

## Licence

BSD-2-Clause, see `LICENSE.md`. The patches are against RTEMS (BSD-2-Clause)
and QEMU (GPL-2.0-or-later) and carry their projects' terms where they apply.
