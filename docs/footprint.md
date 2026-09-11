# Footprint

What an RTEMS ESPHome image costs, measured rather than estimated. Regenerate
with `scripts/footprint.py --find`.

## Reading these numbers

`<arch>-rtems7-size` is misleading on both BSPs here, in opposite directions,
which is why there is a tool rather than a shell alias.

Its **bss** includes `.work`, the RTEMS workspace and heap. `confdefs` sizes
that to absorb whatever RAM is left rather than to any measured demand, so it
is not a cost the application controls. On the Zynq it is 250 MiB. Reporting
that as memory use is not a small error.

Its **text + data** is the right number for flash, and `footprint.py` agrees
with it byte for byte on every ESP32-C3 image. But the `.bin` beside the `.elf`
is *not* that number: `objcopy` pads across the gap between the flash and RAM
address windows, so the ESP32-C3 `.bin` is 1 MiB and 74% zero, and the Zynq
ones are 267 MB. Neither is a footprint. The Zynq lanes boot from the ELF via
`-kernel`, so those `.bin` files are an artefact nothing reads.

So: **flash** is bytes in the image, **RAM (application)** is what the program
asks for, and what is left over is reported separately as sized-to-fit.

## riscv/esp32c3db

Part has 1 MiB of mapped code flash and 320 KiB of SRAM. Flash is the cheap
resource here and RAM is the constraint, which is the trade the
`ESPHOME_THREAD_MULTI_ATOMICS` decision rested on.

| image | flash KiB | of which code | RAM (application) KiB | since 803b46c |
|---|---:|---:|---:|---:|
| `scheduler` | 319.6 | 283.7 | 30.8 | +17.1 |
| `primitives` | 321.7 | 284.9 | 30.7 | +16.9 |
| `reference-node` | 329.5 | 291.4 | 31.0 | +18.1 |
| `gpio` | 334.0 | 294.9 | 31.4 | +17.1 |
| `i2ctest` | 330.3 | 292.2 | 30.8 | +10.1 |
| `uarttest` | 335.1 | 296.6 | 31.1 | +10.4 |

Re-measured 2026-09-11 against esphome `7e96d19`, RTEMS pin `b03d4c0119` with
`patches/rtems/` applied, `riscv-rtems7-gcc 15.2.0` (RSB `105f43d299`).

**The growth is features, not bloat.** The first baseline was taken 29 esphome
commits earlier, before file-backed preferences, the network interface, lwIP
address types, the mDNS responder, the GPIO, I2C and UART backends, the 1 kHz
clock and third-party library support. +13.6 KiB of flash on average for that
list is cheap, and RAM did not move at all — every image still asks for ~31 KiB
and still leaves ~289 KiB.

RAM staying flat across all six is the number worth watching, because RAM is
the constraint on this part and flash is not: these images use a third of the
1 MiB mapped code window.

`.work` takes the remaining ~289 KiB of SRAM in every one of these.

## arm/xilinx_zynq_a9_qemu

| image | flash KiB | of which code | RAM (application) KiB |
|---|---:|---:|---:|
| `zynqprobe` | 185.6 | 164.2 | 1054.3 |
| `schedzynq` | 192.2 | 169.9 | 1054.4 |
| `ipzynq` | 197.7 | 174.4 | 1054.4 |
| `prefszynq` | 204.2 | 179.5 | 1054.4 |
| `thrzynq` | 212.6 | 186.0 | 1055.3 |
| `dnszynq` | 1345.3 | 296.0 | 3006.7 |
| `sockzynq` | 1363.4 | 314.7 | 3007.4 |
| `apizynq` | 1412.3 | 353.5 | 3007.5 |

Two things here are the BSP rather than ESPHome, and both are worth knowing
before quoting a Zynq number:

* The lwIP images carry **1 MiB of `.nocache`** in the image. The Zynq BSP
  marks that region `LOAD`, so a megabyte of zeros is stored and copied at
  boot for memory that only ever holds DMA descriptors. It is the difference
  between `schedzynq` at 192 KiB and `apizynq` at 1412 KiB — the code
  difference between them is only ~184 KiB.
* **RAM (application) starts at ~1 MiB** even for a bare scheduler test, and
  reaches ~3 MiB with lwIP. That is `.bss`, and on a board with 256 MiB of
  DRAM nothing has needed to account for it. It would matter on a smaller
  part, so it is recorded rather than dismissed.

## What is not measured yet

Optimisation level is whatever the build backend passes today; nothing has
been built `-Os` for comparison. `.eh_frame` is 6.8 KiB on the ESP32-C3 images
and would go with `-fno-exceptions`, which ESPHome does not currently build
with. Neither is a conclusion, only an unmeasured lead.
