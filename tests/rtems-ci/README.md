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

## The three configs

`reference-node.yaml` is the one that must stay identical to the ESP-IDF
lane's. The other two are RTEMS-only on purpose — adding their checks to the
reference node would turn the A/B into a comparison of something else.

| config | what it proves |
|---|---|
| `reference-node.yaml` | the same node builds and runs on both stacks |
| `primitives.yaml` | time, mutexes, ISR wake, priority inheritance |
| `gpio.yaml` | a pin declared in YAML is really driven, and its interrupt really arrives |
| `i2c.yaml` | a real device on the bus answers, and one that is not there does not |

### `gpio.yaml`

An `output:` on GPIO4 and a `binary_sensor:` on GPIO5, declared the ordinary
way, so what runs is the whole path a real configuration takes: `gpio.py`'s
codegen, `RTEMSGPIOPin`, the BSP driver, the registers.

The checks are made *at the pad*, through the BSP hook, not by asking the pin
object what it last wrote. A pin that reported its own last value would pass
with every register write removed.

The interrupt check needs a stimulus, and nothing is plugged into an emulated
board. It uses the one thing that can move an undriven pad: its pull resistor.
Swapping GPIO5 from pull-up to pull-down produces a real falling edge, and
`GPIOBinarySensor` in interrupt mode publishes *only* when its ISR sets the
changed flag — so the sensor's state following the resistor is an end-to-end
proof that the interrupt reached ESPHome's handler.

```
turn_on drives the pad high                  ok
turn_off drives the pad low                  ok
the pad follows the output component         ok
a pulled-up input reads high                 ok
driving one pin leaves the other alone       ok
the pulled-up sensor reads on                ok
switching to pull-down is seen               ok
and switching back is seen                   ok
```

Requires the QEMU fork at `59917ba` or later; before that, `hw/gpio/esp32c3_gpio.c`
modelled nothing but the strapping register and every one of these would have
failed.

### `i2c.yaml`

Needs a slave on the command line:

```sh
../esp-idf-ci/venv/bin/esphome compile i2c.yaml
../../tools/rtems-ci-run.sh -M "CI-MARKER i2c ok" \
    .esphome/build/i2ctest/i2ctest.bin \
    -- -device tmp105,address=0x48
```

Add `-device at24c-eeprom,address=0x50,rom-size=256` to see the scan report two
devices rather than one.

The TMP105 is a real device model, not a stub, so a value read from it came off
a modelled bus rather than out of the driver.

It reads the two **alarm limit** registers, not the temperature. They reset to
75 °C and 80 °C: known, non-zero, and different from each other, so a bus stuck
at zero fails and so does one returning the previous transfer's bytes. The
temperature register is unusable here — `tmp105_reset()` zeroes it, and reset
runs after `-device` applies its properties, so a command-line `temperature=`
never survives to be read.

The last check is the one that keeps the others honest: an address nothing
answers on must come back `ERROR_NOT_ACKNOWLEDGED`. A bus that reported success
there would report success whatever happened on the wire.

```
read T_LOW                                   ok
T_LOW is 75 C, its reset value               ok
read T_HIGH                                  ok
T_HIGH is 80 C, so it differs from T_LOW     ok
write the config register                    ok
read the config register back                ok
it holds what was written                    ok
an address nothing answers on is NACKed      ok
```

With `scan: true` and an `at24c-eeprom` added at `0x50`, the scan reports both
and nothing else out of the 112 addresses it probes.

Requires the QEMU fork at `2546b01` or later. Before that the RISC-V machine had
no I2C controller at all — accesses were absorbed by the catch-all IO region
with a warning, not faulted, so a driver would have looked like it worked.

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
