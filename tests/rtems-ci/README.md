# The RTEMS lane

The B side of the A/B. `tests/esp-idf-ci/` is the A side, and the two configs
are deliberately the same configuration on the same part — so a difference
between the lanes is a difference between RTEMS and ESP-IDF, not between two
configurations that happen to look similar.

**Keep them in step.** A change to one that is not made to the other quietly
turns the comparison into a comparison of something else.

```sh
../esp-idf-ci/venv/bin/esphome compile reference-node.yaml
../../tools/rtems-ci-run.sh \
    .esphome/build/reference-node/reference-node.bin
```

Needs an RTEMS 7 RISC-V toolchain and the BSP **installed**, not just built —
`./waf install` in the RTEMS tree is what creates the pkg-config file the build
backend reads. See `docs/esp32c3-bsp.md`.

## What differs from the ESP-IDF lane, and why

**The image.** ESP-IDF builds a four-part boot chain — bootloader, partition
table, OTA data, app — that the harness assembles at the offsets
`flasher_args.json` records. RTEMS builds for the ROM's direct boot mode, so
the build backend's `objcopy` output *is* the flash contents and the harness
only pads it. That is why these are two scripts rather than one with a flag.

**No `hardware_uart:`.** The ESP-IDF config must select UART0 explicitly,
because ESPHome defaults the C3 to USB Serial/JTAG and QEMU models that as a
stub. On RTEMS the console is the BSP's — which device and what baud are fixed
when the BSP is built — so there is nothing here to select. The same choice
still has to be made, one layer down, in `config_esp32c3db.ini`.

**Failure signatures.** RTEMS reports a fatal through `bsp_fatal_extension`,
which prints `*** FATAL ***` and the source and code. Note that `RTEMS
shutdown` on its own is *not* a failure — a clean `rtems_shutdown_executive()`
prints it too — so the harness matches the fatal header rather than the banner.
Verified against a real crash log, not just written to look right.

## What a pass means

Two `CI-MARKER` lines the firmware prints itself: one from the boot hook, one
from a periodic component **on its third update**.

The second is the one that matters. A firmware that reaches `setup()` and then
wedges looks identical, on a serial line, to one idling correctly — so a
verdict built on "nothing crashed" would happily pass a dead scheduler.

## `-icount` and what timing numbers mean

On by default: it ties the guest clock to executed instructions rather than
host scheduling, which makes runs reproducible and lets QEMU skip guest idle,
so a node sleeping a second between updates costs milliseconds of wall clock.

It also makes wall-clock measurement meaningless, because the guest runs faster
than real time while idle. Pass `-I` to measure an interval. Measured that way,
the 1 s `update_interval` above produces 20 updates in 20 seconds.

## Recorded result

```
verdict: PASS
  seen    CI-MARKER boot ok
  seen    CI-MARKER scheduler ok
```
