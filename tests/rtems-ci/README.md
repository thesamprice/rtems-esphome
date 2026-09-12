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

A lane that exists but is not enforced reads as coverage and is not, so the
third column says which are enforced and the ones that are not say why.

| config | what it proves | in CI |
|---|---|---|
| `reference-node.yaml` | the same node builds and runs on both stacks | yes |
| `primitives.yaml` | time, mutexes, ISR wake, priority inheritance | yes |
| `scheduler.yaml` | the scheduler's ordering and drift, both icount modes | yes |
| `gpio.yaml` | a pin declared in YAML is really driven, and its interrupt really arrives | yes |
| `i2c.yaml` | a real device on the bus answers, and one that is not there does not | yes |
| `uart.yaml` | bytes leave the port and the reply comes back | yes |
| `sensor.yaml` | an upstream ESPHome component, unmodified, reads a modelled device | yes |
| `spi.yaml` | ESPHome's SPI stack reads an SSI flash on GPSPI2 | yes |
| `zynq-scheduler.yaml` | the same node runs on a second BSP, on a different architecture | **no** — 1 |
| `zynq-socket.yaml` | ESPHome's own socket layer accepts a connection over lwip | **no** — 1, 2 |
| `zynq-ip.yaml` | addresses parse, format and compare, both families | **no** — 1, 2 |
| `zynq-api.yaml` | **a Home Assistant client talks to the node over the native API** | **no** — 1, 2, 3 |
| `zynq-dns.yaml` | names resolve asynchronously without stalling the main loop | **no** — 1, 2 |
| `zynq-prefs.yaml` | preferences reach a file, come back off it, and a damaged one is refused | **no** — 1 |
| `zynq-threads.yaml` | the core helpers are correct when a second RTEMS task uses them | **no** — 1 |
| `zynq-mdns.yaml` | the node advertises itself, and the stack survives advertising | **no** — 1, 2, 3 |
| `zynq-mqtt.yaml` | *(untracked, not finished)* | **no** — 4 |

The BSP tests under `tests/bsp-*` all run in CI. They have no ESPHome in them
— each drives a driver or a BSP facility directly — so a failure is the BSP's
rather than the platform backend's: `bsp-pin`, `bsp-tick`, `bsp-opendrain`,
`bsp-i2c`, `bsp-gpspi`, `bsp-spidrv`.

**Why the Zynq lanes are not enforced**

1. The toolchain image has no ARM toolchain and no `arm/xilinx_zynq_a9_qemu`
   BSP. Adding them means a second RSB build, roughly doubling the image, which
   is a real change and wants its own review. See #72.
2. Also needs rtems-lwip built and installed against that BSP —
   `tests/zynq-lwip/README.md` has the recipe.
3. Also needs `aioesphomeapi` on the runner and a forwarded port; the verdict
   comes from the client's exit status rather than a marker in the guest log.
4. `zynq-mqtt.yaml` is not finished and is not tracked. It is listed so that
   its absence is deliberate rather than an oversight.

All eight tracked Zynq lanes have been run by hand against a BSP and an
rtems-lwip built from **this tree's pin** (`b03d4c0119`, confirmed in the
binaries rather than assumed), and all eight pass. What is missing is only the
image change that would make CI do it. `config_zynq_a9_qemu.ini` is the BSP
config; `tests/zynq-lwip/README.md` has the rtems-lwip recipe.

Nothing has to be installed over the toolchain prefix to reproduce that. Build
into a scratch prefix whose `bin` is a symlink to the real one, and point
ESPHome at it:

```sh
PFX=/tmp/zynq-prefix
mkdir -p $PFX && ln -s $HOME/rtems/7/bin $PFX/bin
( cd src/rtems && ./waf configure --rtems-config=../../config_zynq_a9_qemu.ini \
      --out=build-zynq --prefix=$PFX && ./waf --out=build-zynq && \
      ./waf --out=build-zynq install )
( cd src/rtems-lwip && PKG_CONFIG_PATH=$PFX/lib/pkgconfig \
      ./waf configure --prefix=$PFX --rtems-bsps=arm/xilinx_zynq_a9_qemu && \
      ./waf build && ./waf install )
RTEMS_TOOLS_PREFIX=$PFX ../../.venv/bin/esphome compile zynq-api.yaml
```

That matters because the prefix may already hold a BSP from a different RTEMS
checkout, and linking against one silently is how a lane passes while testing
something other than what is in the tree.

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

### `zynq-socket.yaml`

`esphome::socket` — the layer `api`, `web_server` and `mqtt` all reach the
network through — accepting a TCP connection from outside the guest.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-socket.yaml
../../tools/zynq-lwip-run.sh -M "CI-MARKER socket ok" \
    .esphome/build/sockzynq/sockzynq.elf
```

`tests/zynq-lwip/` proves the stack underneath by talking to lwip directly;
this proves the seam above it. Both are needed: the first would pass with an
ESPHome layer that did not work, and the second cannot run without the first.

The interface is brought up in the test rather than by a `network` component,
because there is not one yet — that is #13 and #14. Nothing below that line
knows it.

**Not on the ESP32-C3**, because that machine has no NIC QEMU can give it
(#30). This lane is the reason the second BSP exists.

### `zynq-ip.yaml`

`network/ip_address.h` on RTEMS. Its POSIX arm is *reused* rather than a fourth
representation added — the arm is `<arpa/inet.h>`, `struct in_addr` and
`inet_pton`, with nothing host-specific in it — and this is what checks that
the reuse actually holds. An arm that compiles but parses wrongly would be
worse than a new one.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-ip.yaml
../../tools/zynq-lwip-run.sh -n -M "CI-MARKER ip ok" \
    .esphome/build/ipzynq/ipzynq.elf
```

`-n` because this one never listens; without it the harness waits out its
timeout for a connection that is not coming.

The check that earns its place is the octet constructor agreeing with the
parser — that is what catches a byte-order mistake, and a byte-order mistake is
exactly what reusing another platform's arm risks.

**One result worth knowing rather than being surprised by:** `192.168.7` parses,
as `192.168.0.7`. That is `inet_aton` behaving as it always has — `a.b.c` is
`a.b` plus a 16-bit `c` — and it means the same string in the same YAML is a
valid address here and a rejected one on a platform whose arm uses lwip's
`ipaddr_aton`. Asserted rather than left implicit.

`is_connected()` is also checked, and expected to be **false**: no interface is
brought up in this configuration, and the platform asks the stack rather than
assuming a link the way host does.

### `zynq-api.yaml`

The one that means something. A real `aioesphomeapi` client — the same library
Home Assistant uses — connects to the node and completes the protocol.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-api.yaml
../../tools/zynq-lwip-run.sh -p 6053 \
    -c "$PWD/../esp-idf-ci/venv/bin/python $PWD/api_client.py" \
    .esphome/build/apizynq/apizynq.elf
```

`-c` hands the verdict to the client's exit status, because the guest's own log
cannot say whether a protocol exchange it is only one end of actually worked.

```
ok   handshake completed
ok   device info: name='apizynq' model='xilinx_zynq_a9_qemu' manufacturer='RTEMS'
ok   entity metadata: ['RTEMS Counter']
ok   states arrive and advance: [223.0, 224.0, 225.0, 226.0]
ok   disconnected
ok   reconnected after a disconnect
```

The reconnect is not decoration: a node that leaks its listening socket, or
leaves a half-closed connection in the table, passes everything above it and
fails there.

**`rtems: network:` brings the interface up**, because there is no `wifi:` or
`ethernet:` for this platform — which controller a board has is decided when
the BSP is built, so all a configuration can say is the address.

### `zynq-dns.yaml`

Asynchronous DNS, and — the part that matters — that it does not stall the
main loop.

```sh
../../tools/zynq-lwip-run.sh -n -M "CI-MARKER dns ok" \
    .esphome/build/dnszynq/dnszynq.elf
```

Three cases, each pumping a loop and reporting the **worst gap between
iterations**, because "does not block" is the claim an implementation can
quietly fail:

```
a numeric address resolves: immediate
a real hostname resolves: resolved after pumping, worst loop gap 13ms
a name that does not exist fails: failed after pumping, worst loop gap 13ms
```

The failure case is the one worth having. A resolver that blocks shows up there
as a single gap the length of the timeout, and nowhere else.

QEMU's user-mode networking answers DNS at `10.0.2.3`, which a static
configuration has to be told about — `dns_setserver()` in the test.

### `zynq-prefs.yaml`

The preferences store, on a filesystem.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-prefs.yaml
../../tools/zynq-lwip-run.sh -n -M "CI-MARKER prefs ok" \
    .esphome/build/prefszynq/prefszynq.elf
```

`rtems: preferences_path: /prefs.dat` is what makes the store file-backed;
without it preferences stay in memory, which is all a BSP with no filesystem
can offer.

Every read-back calls `load_store()` first, which replaces the in-memory map
with whatever the file says. That is what makes this testable without a reboot
— and it is also what the first version of this lane got wrong, so it is worth
saying why the checks are shaped the way they are:

* **The file is checked to exist before anything is read back.** Without that
  check the read-backs pass out of the in-memory map on a store that never
  wrote a byte. That is not hypothetical: `save()` did not set the dirty flag,
  so `sync()` returned success without writing, and the lane reported thirteen
  passes against a store that did not exist.
* **Two corruptions, not one.** The store has two independent defences and a
  single flipped byte only exercises whichever it lands in. A byte in a
  record's length field is caught by the length check before the checksum is
  ever computed — so with the checksum comparison deleted, a test that flips
  only that byte still passes. The value-byte corruption is the one only the
  checksum can catch.

```
a damaged record length loads nothing at all       ok
a damaged value loads nothing at all               ok
```

**What this lane does not prove is surviving a reboot**, because this board's
filesystem is RAM. That half of #16 waits on #42. A RAM disk called persistence
would be worse than saying so.

It also records a RTEMS deviation worth knowing: `rename()` does not replace an
existing destination. `_rename_r()` evaluates the new path with
`RTEMS_FS_EXCLUSIVE` and fails with `EEXIST` whatever the filesystem
underneath, so the write-beside-and-rename that would make a replacement atomic
has to unlink first.

### `zynq-threads.yaml`

Cross-task use of ESPHome's core task and queue helpers (#34).

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-threads.yaml
../../tools/zynq-lwip-run.sh -n -M "CI-MARKER threads ok" \
    .esphome/build/thrzynq/thrzynq.elf
```

#34 was opened against an audit that counted 27 core blockers across six
task/queue files. Most of them are answered by the thread model rather than by
porting: this platform selects `ESPHOME_THREAD_MULTI_ATOMICS`, under which
`freertos_queue.h`, `static_task.*` and `main_task.*` compile out entirely.
What that leaves is the part an audit cannot see — this port really does have
other tasks, since lwIP runs its own — so the helpers that *do* compile have to
be right when a second task uses them. Every check here runs against a real
second RTEMS task.

**Two tasks from one entry point with different arguments** is the concrete
form of the gap that made NASA OSAL unsuitable: `rtems_task_start()` takes a
per-task argument, so the classic API is already the helper and no
task-creation shim is needed.

**The queue** is checked twice. Once deterministically with no second task, for
the capacity claim — a ring of 4 holds 3, because full is `tail+1 == head` and
that is how it tells full from empty — and once with a producer task pushing
200 items through a ring of 8:

```
pushes refused while the ring was full: 28
the ring really filled during the run              ok
```

The refusal count is the evidence, not decoration. Without it the test passes
on a consumer that simply kept up, never crossing the full path at all.

**The wake is the interesting one.** ESPHome's contract under `MULTI_ATOMICS`
is that the scheduler is safe to call from any thread but does *not* wake the
loop by itself — a background producer calls `App.wake_loop_threadsafe()`. Both
halves fail silently: an unsafe scheduler corrupts rarely, and a missing wake
only ever shows up as latency. So the lane defers the same work twice:

```
deferred without a wake: 59646us
deferred with a wake:    448us
ratio: 133x
```

Stubbing `wake_loop_threadsafe()` to do nothing moves the second number to
19636us and fails both wake checks, which is what makes the 133x mean something.

**One divergence is recorded rather than asserted:** `try_lock()` fails while
*another* task holds the mutex, and succeeds from the owning task, because
RTEMS binary semaphores under priority inheritance permit nested access.
FreeRTOS's do not. See #71 — a same-task contention check would have proved
nothing, which is how that was found.

### `zynq-mdns.yaml`

Discovery: is the node advertised over mDNS, and is the network stack still
alive afterwards.

```sh
../esp-idf-ci/venv/bin/esphome compile zynq-mdns.yaml
ZYNQ_CAPTURE=$PWD/out/capture.pcap ../../tools/zynq-lwip-run.sh -D -p 6053 \
    -c "$PWD/../esp-idf-ci/venv/bin/python $PWD/mdns_client.py" \
    -o out .esphome/build/mdnszynq/mdnszynq.elf
```

**The verdict is read from a capture of the netdev**, not from the guest, which
is what `-D` is for. Nothing the guest can print establishes that a packet
reached the wire — and the interesting failures here are all failures to
transmit.

```
ok   the API port still answers after advertising
     3 probe(s), 2 announcement(s)
ok   it probed before claiming the name
ok   it announced
ok   the announcement names the esphome service
ok   and carries the configuration's TXT records
```

**The API check is not padding.** Advertising happens on lwIP's thread, and a
fault there does not merely lose the announcement — it kills the thread and
every protocol goes quiet at once. That is exactly how this failed before #78,
and from outside it looked like an mDNS problem rather than a dead stack. A
lane that only counted mDNS packets would have reported the same "no
announcement" for a node that was fine and for one that was dead.

Counts are `>=`, not `==`: a retransmission is legal and is not a regression.

**What this lane does not check** is the pace. The announcements are about five
times faster than RFC 6762 intends, because guest time under this QEMU machine
runs 5.6–6.9x fast (#77). The sequence is correct; the rate is a property of the
emulation, so asserting on it would be asserting on the host's speed.

Five defects across three layers had to be fixed before this passed — #64 and
#74 in the Xilinx driver, #75 in lwIP's responder and its timeout pool, #78 in
this platform's `mdns` implementation. The lane exists so they stay fixed.

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
