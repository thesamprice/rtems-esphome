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
| `uart.yaml` | bytes leave the port and the reply comes back |
| `zynq-scheduler.yaml` | the same node runs on a second BSP, on a different architecture |

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

### `uart.yaml`

Needs something on the other end of UART1. `rtems_builder/tests/esp32c3/uart/echo.py`
is that: it echoes what it receives **in upper case**, so a chip looping TX
straight back to RX — a real fault — cannot pass, which a verbatim echo would.

```sh
python3 ../../../rtems_builder/tests/esp32c3/uart/echo.py /tmp/uart1.sock &
../esp-idf-ci/venv/bin/esphome compile uart.yaml
../../tools/rtems-ci-run.sh -M "CI-MARKER uart ok" \
    .esphome/build/uarttest/uarttest.bin \
    -- -serial unix:/tmp/uart1.sock
```

The second `-serial` is UART1; the console stays on the first, so the log is
not travelling over the port under test.

`number: 1`, not pins. Which pads a port reaches is fixed when the BSP is
built, so there is nothing here to choose — the same decision still exists, one
layer down.

The checks worth pointing at are the last four: `peek_byte()` must return the
first byte and **not consume it**, which is the whole of its contract and the
easiest part to get wrong.

```
nothing is waiting before we send            ok
the reply arrives                            ok
read it                                      ok
it is what the far end sent, not what we sent ok
and nothing is left over                     ok
two more arrive                              ok
peek                                         ok
peek returns the first byte                  ok
peek does not consume it                     ok
read both                                    ok
the peeked byte is still first               ok
now nothing is left                          ok
```

**`rx_buffer_size` matters here more than on other platforms.** Termios, not
the driver, holds received bytes, and its default is 256. A reply longer than
that, arriving faster than the main loop reads it, is silently truncated — 300
bytes became 255 before this was wired up. Raise it for any protocol with long
frames.

### `zynq-scheduler.yaml`

`scheduler.yaml` with one line changed — `bsp: arm/xilinx_zynq_a9_qemu` instead
of `riscv/esp32c3db`. Different architecture, different board, different
emulator binary, no `#ifdef` anywhere.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-scheduler.yaml
qemu-system-arm -no-reboot -nographic -M xilinx-zynq-a9 -m 256M \
    -serial null -serial mon:stdio -net none \
    -kernel .esphome/build/schedzynq/schedzynq.elf
```

Note it boots an **ELF through `-kernel`**, not a raw flash image. The build
backend produces both; the ESP32-C3's direct-boot mode wants the binary and the
Zynq wants the ELF, which is why the objcopy step is a named ninja rule rather
than part of the link.

This lane exists to make rule 2 falsifiable rather than aspirational. It has
already earned that: the first attempt failed at the preprocessor, because
`<bsp/gpio.h>` `#error`s unless the BSP defines `BSP_GPIO_PIN_COUNT`, and most
BSPs — the Zynq among them — do not. Nothing on the ESP32-C3 could have found
that.

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
