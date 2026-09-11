# riscv/esp32c3db — the Espressif ESP32-C3 on QEMU

The BSP is upstream RTEMS (`bsps/riscv/esp32`, `spec/build/bsps/riscv/esp32`,
added by Kinsey Moore in 2026). What is here is the config, a test runner, one
one-line fix to the BSP, and two fixes to QEMU — one without which the BSP
cannot take an interrupt at all, and one without which its clock runs slow in
proportion to how often software looks at it.

## Why the C3 and not the ESP32

RTEMS has no Xtensa port, so the ESP32, ESP32-S2 and ESP32-S3 are out of reach
whatever the emulator does. The RISC-V C-series parts are the whole of the
field, and `esp32c3db` is the one BSP variant RTEMS currently defines.
Espressif's QEMU ships an `esp32c3` machine, so the pair lines up.

## Quick start

```sh
scripts/build_esp_qemu.sh                         # patched QEMU, ~10 min
cd src/rtems && ./waf configure -o build-esp32c3db \
    --rtems-config=../../configs/config_esp32c3db.ini \
    --rtems-tools=$HOME/rtems/7 --prefix=$HOME/rtems/7 && ./waf build

tools/esp32c3-run-tests.sh                        # finds the QEMU it built
```

`scripts/build_esp_qemu.sh` fetches `src/esp-qemu` if it is not there, checks it
is on its recorded commit, configures and builds. There is nothing to apply —
the two QEMU fixes are commits on the pinned branch. The binary is left runnable
in the build tree at `src/esp-qemu/build/qemu-system-riscv32`, which is where the
test runner looks; installing it is optional and only worth doing to put it on a
PATH.

`-o build-esp32c3db` keeps this out of `build/`, so an mbv build configured in
the same tree survives. Waf's top level lock file moves either way; see the
note in `scripts/fanalyzer.sh`, which handles it for the analyser build.

## The image is not an ELF

The C3 boots from flash. There is no loader device to hand QEMU an ELF with,
the way `-device loader,file=...` does for the MicroBlaze V machine, so the
runnable artifact has to be a flash image.

The BSP builds for the ROM's **direct boot** mode, selected by
`ESPRESSIF_DIRECT_BOOT`. `bsps/riscv/shared/start/start.S` emits two
`0xaedb041d` words at the start of `.bsp_start_text`; the ROM finds them at
flash offset 0 and jumps straight to `0x42000008`. No `esp_image` header, no
second stage boot loader, no `esptool` — which also means no partition table
and no OTA data.

The link is arranged so this falls out of `objcopy`. The load addresses are
already flash offsets: `.text` at 0, and the rodata and data load images at
`ESP32C_CODE_REGION_SIZE`, while their virtual addresses are the mapped
windows at `0x42000000` and `0x3c000000`.

```
LOAD  0x42000000  paddr 0x00000000   .start .text
LOAD  0x3c100000  paddr 0x00100000   .rodata .init_array .rtemsroset
LOAD  0x3fc80000  paddr 0x00101f60   .data .sdata
LOAD  0x3fc80564  paddr 0x3fc80564   .bss .rtemsstack .work   (no contents)
```

So the whole of image preparation is

```sh
riscv-rtems7-objcopy -O binary test.exe test.raw   # LMAs place it
dd if=/dev/zero of=flash.bin bs=1m count=4         # pad to the flash size
dd if=test.raw of=flash.bin conv=notrunc
qemu-system-riscv32 -M esp32c3 -display none -no-reboot \
    -serial file:test.log -drive file=flash.bin,if=mtd,format=raw
```

`tools/esp32c3-run-tests.sh` does exactly that per test, into a temporary
directory it deletes as it goes, so the peak cost is `-j` times the flash size
rather than 632 times it.

`bsp_start_copy_sections()` copies `.data` and the fast text and data sections
out of the mapped flash window at run time. Nothing else is unpacked.

Both QEMU fixes live as commits on `esp32c3-rtems-fixes` in the `src/esp-qemu`
fork rather than as patch files, so fetching the submodule is all it takes to
get a working emulator. Both are candidates for `espressif/qemu` and neither is
RTEMS-specific; when they land upstream, the submodule points back at
`espressif/qemu` and the branch goes away.

## Two things the stock QEMU gets wrong

### 1. The USB Serial/JTAG console is a stub — set it to False

The BSP defaults `ESPRESSIF_USE_USB_CONSOLE` to true, and on real silicon that
is the right default. Under QEMU it is fatal. `hw/misc/esp32c3_jtag.c` is a
stub in the most literal sense:

```c
static uint64_t esp32c3_jtag_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(opaque);
    (void) s;
    return 0;
}
```

Reads return 0, writes are discarded, and the machine never attaches it to a
chardev, so there is no `-serial` index that reaches it. Output would not
merely be lost: `esp_uart_output_char()` spins waiting for
`USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE`, a bit a stub returning 0 never sets,
so the first character printed hangs the run.

`configs/config_esp32c3db.ini` therefore sets it False, which selects the UART0
path. That path calls the ROM's `uart_tx_one_char` at `0x40000068`, and QEMU
runs the real ROM (`pc-bios/esp32c3-rom.bin`), so it works and lands on
`-serial`.

### 2. The interrupt matrix has no status registers — QEMU needs a patch

This is the one that matters. `hello` passes without it; everything that takes
an interrupt fails identically.

The C3 maps 62 peripheral interrupt sources onto 31 CPU lines, and the BSP
deliberately shares lines — the four software interrupts all sit on line 1,
`TG_WDT` and `TG1_WDT` both on 14. `mcause` gives the *line*, so
`_RISCV_Interrupt_dispatch()` in `bsps/riscv/esp32/irq/irq_c3.c` reads
`INTERRUPT_CORE0_INTR_STATUS_0` and `_1` at `0xf8` and `0xfc` to find out which
*peripheral* raised it.

`hw/riscv/esp32c3_intmatrix.c` implements the map, priority, threshold, enable
and type registers, and returns 0 for everything else — those two included.
Instrumenting the dispatch shows it exactly, on the first clock tick of
`ticker`:

```
DISPATCH mcause=80000010 cpu_vector=16 status0=00000000 status1=00000000 active=0
```

The line is right: 16 is what the BSP maps `SYSTIMER_TARGET0_INTR` to, so QEMU
delivered the systimer interrupt correctly. But the status reads back 0, so no
peripheral matches, `active` is 0, and vector 0 is the invalid vector:

```
*** FATAL ***
fatal source: 14 (RTEMS_FATAL_SOURCE_SPURIOUS_INTERRUPT)
```

The BSP is not at fault and neither is the shared-line scheme — on silicon
those registers read back the input levels. The model already holds the value
they should return: `irq_levels`, the 64-bit mirror of the input levels it
maintains anyway in order to decide whether to assert a line. So the fix is to
return it:

```c
} else if (index == ESP32C3_INTMATRIX_IO_STATUS_0_REG) {
    r = (uint32_t) s->irq_levels;
} else if (index == ESP32C3_INTMATRIX_IO_STATUS_1_REG) {
    r = (uint32_t) (s->irq_levels >> 32);
}
```

That is commit `3d3909d` on the `esp32c3-rtems-fixes` branch of the
`src/esp-qemu` submodule. Still needed as of `esp-develop-9.2.2-20260417`: the
released binaries and the `esp-develop` tip carry byte-identical unpatched
files.

`tools/esp32c3-run-tests.sh` probes for the fix before running a suite.
Finding out 632 times that QEMU cannot deliver an interrupt is not a test
result.

### Two things about building QEMU itself

Neither is about this BSP, and both cost more time to diagnose than to fix, so
`scripts/build_esp_qemu.sh` passes them and says why.

`--disable-containers`. QEMU's `configure` probes for a container engine to
find cross compilers with, and runs `docker probe` whenever a `docker` binary
is on PATH. With Docker installed but not running, that call blocks instead of
failing, and configure hangs there producing no output at all.

`--enable-gcrypt --disable-gnutls`. The esp32c3 machine always instantiates its
AES, RSA, DS and XTS-AES devices, and `hw/misc/meson.build` only compiles those
`when: [gcrypt, 'CONFIG_RISCV_ESP32C3']`. QEMU's meson does not look for gcrypt
at all when gnutls provides crypto, which it does on a normal Homebrew machine,
so the build succeeds and the machine then dies at init with

```
qemu-system-riscv32: unknown type 'misc.esp32c3.aes'
```

Asking for gcrypt explicitly makes meson ignore gnutls for crypto and go find
it. gnutls then has to come out of the build as well: with it in, and out of
the crypto path, `crypto/tlscredspriv.h` is compiled without gnutls's include
path and everything that includes it fails on `gnutls/gnutls.h: file not
found`. Nothing here needs TLS.

## The systimer frequency was wrong

`bsps/riscv/esp32/clock/clockdrv_systimer.c` declared

```c
/* The SYSTIMER always operates at 16MHz */
#define SYSTIMER_FREQUENCY ( 16ULL * 1024 * 1024 )
```

The comment is right and the code is not: that is 16777216, and the C3's
system timer runs at 16 MHz exactly. ESP-IDF puts 16 ticks in a microsecond,
and QEMU models it the same way — `ESP_SYSTIMER_CNT_CLK 16000000` in
`include/hw/timer/esp_systimer.h`. So the constant was 4.86% high, and it is
used for three things at once: the tick interval programmed into TARGET0, the
timecounter's `tc_frequency`, and `_CPU_Counter_frequency()`. Everything the
system believes about elapsed time was off by that factor.

`patches/rtems/esp32c3-systimer-frequency.patch` is the one line.

What it does **not** do is move the testsuite much, and the reason is worth
knowing before reading anything into that. The constant is used for both ends
of the same measurement: it sets the number of counts programmed into TARGET0
for one clock tick, and it is the `tc_frequency` the timecounter converts those
counts back to nanoseconds with. Get it 4.86% high and the tick becomes 4.86%
longer in real time while the timecounter under-reads by 4.86%, so anything
that measures RTEMS time against RTEMS time cancels the error exactly. `sp69`,
which times a 600 ms rate monotonic period, barely notices:

| | measured wall time for 600 ms | short by |
|---|---|---|
| before, `-icount` | 599953174 ns | 78 ppm |
| after, `-icount` | 599996499 ns | 5.8 ppm |
| after, no `-icount` | 596925999 ns | 0.5% |

The error that does not cancel is against the outside world. A 10 ms tick is
really 10.49 ms, `_CPU_Counter_frequency()` is wrong by the same factor, and so
is every busy-wait or performance figure derived from it. A self-consistent
clock that is 4.86% off real time is exactly the kind of defect a testsuite is
bad at catching, which is why the evidence for it is the datasheet and the two
independent implementations — ESP-IDF's 16 ticks per microsecond and QEMU's
`ESP_SYSTIMER_CNT_CLK` — rather than a test that goes from red to green.

The last row is the argument for keeping `-icount shift=0,sleep=off` on:
without it the guest clock follows host scheduling and the error is three
orders of magnitude worse than the defect being discussed.

`sp69` still fails, before and after, because it asserts `>=` on the nominal
period and the period is a few parts per million short. Whether that last bit
belongs to the TARGET0 period programming or to QEMU's comparator reload has
not been run down.

## The QEMU counter loses a tick on every read

The frequency fix above left `sp69` failing by 5.8 ppm and `spcpucounter01`
failing outright. Both were QEMU, in one line.

`hw/timer/esp_systimer.c`:

```c
static void esp_systimer_update_counter(ESPSysTimerCounter *counter) {
    const int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    const int64_t elapsed_ns = now - counter->base;
    const int64_t ticks = (elapsed_ns * (ESP_SYSTIMER_CNT_CLK / 1000000)) / 1000;
    counter->value = (counter->value + ticks) & ESP_SYSTIMER_52BIT_MASK;
    counter->base = now;
}
```

One systimer tick is 62.5 ns. The conversion truncates, and then `base = now`
throws the remainder away rather than carrying it, so **each update loses up to
one tick**. It is not a fixed error: the counter runs slow in proportion to how
often the guest reads it, because every read is an update. `_CPU_Counter_read()`
writes `UNIT0_OP` with `UPDATE`, which reaches `esp_systimer_flush_counter()`
and so `esp_systimer_update_counter()`, on every single call.

The arithmetic matches what `sp69` measured. It lost about 56 counts across 60
clock ticks, ≈0.93 counts per tick, and RTEMS's timecounter reads the counter
about once per tick. Sub-tick remainder, discarded once per read.

`spcpucounter01` shows the read spacing directly, in its own overhead figures:

```
overhead read: 0 ticks, 0ns
overhead read: 1 ticks, 62ns
```

Consecutive reads land 0 to 62.5 ns apart — at or below one tick period. That is
the worst case for this code: when successive reads are closer together than a
tick, every conversion truncates to zero and every remainder is discarded, so
the counter can fail to advance at all for as long as the guest keeps reading
it. A busy-wait on the counter is precisely that shape.

The fix carries the remainder in the counter state:

```c
const int64_t scaled = elapsed_ns * (ESP_SYSTIMER_CNT_CLK / 1000000)
                     + counter->frac;
const int64_t ticks = scaled / 1000;
counter->frac = scaled % 1000;
```

Commit `69094f5` on the same branch. `sp69`'s 600 ms period goes from
599996499 ns to 600000375 ns, from 5.8 ppm short to 0.6 ppm long, and five
tests move from fail to pass.

Note that this is not RTEMS-specific and not `-icount`-specific. Any guest that
reads the systimer counter more often than once per 62.5 ns sees a slow clock;
instruction counting only makes the reads land closer together and so exposes it
hardest.

## Memory

`ESP32C_CODE_REGION_SIZE` is 1 MiB of the 4 MiB flash, and RAM is 320 KiB at
`0x3fc80000`. The BSP pulls in `tstsmallmem`, so the testsuite is already
configured for a small part. The whole suite links: 632 executables, no
overflow.

## Which pads are free, and who arbitrates them

Three drivers in this BSP point a pad at their peripheral, and none of them
can see what the others took:

| pins | taken by |
|---|---|
| 2, 8, 9 | strapping |
| 5, 6 | I2C — SDA and SCL |
| 7, 10 | UART1 — TX and RX |
| 12–17 | SPI flash on every module |
| 18, 19 | USB D− / D+ |
| 20, 21 | UART0, which is the console |

Leaving **0, 1, 3, 4, 11** for an application.

That table has been wrong once already, which is the point of this section.
It previously listed 7 and 10 as free, because it was written before the UART
driver existed and nothing failed when UART1 took them.

`<bsp/pin.h>` is what stops the next one. `bsp_pin_claim(pin, owner)` records
a pad's owner and refuses a second claimant, and all three drivers call it
before touching IO_MUX. A refusal names the holder:

```
esp32c3 gpio: GPIO5 belongs to esp32c3 i2c
```

It is a 22 bit mask and one pointer per pad, deliberately not
`rtems_gpio_request_pin()`: that lives in `gpio-support.c`, a little over
2000 lines with a mutex per bank and an optional interrupt server task, and
linking it into every application that merely has an I2C bus is a poor trade
on a part with 320 KiB of RAM.

One asymmetry worth knowing. A GPIO claim is never released, because
`rtems_gpio_release_pin()` keeps its bookkeeping in the shared layer and
there is no `rtems_gpio_bsp_release()` for it to call. Re-requesting the pad
as GPIO works — a re-claim by the same owner succeeds — but I2C or UART1
taking a pad GPIO used earlier and has since released is refused. That is a
false refusal, and it is the safe direction to be wrong in.

`tests/bsp-pin/run.sh` builds and runs the test for this.

## Open drain, and what it costs

`<bsp/gpio.h>` has `DIGITAL_INPUT`, `DIGITAL_OUTPUT` and `BSP_SPECIFIC`, and
no open-drain mode. The ESP32-C3 has the bit — `GPIO_PINn` bit 2,
`PAD_DRIVER` — and the I2C driver sets it directly, because it is inside the
BSP and may.

Above the BSP there is no way to ask for it, so `components/rtems` builds it
from the two halves the API does have: `digital_write(false)` selects output
and drives low, `digital_write(true)` selects input and lets the pull-up take
the line. That is what open drain is, and it needs no API change and no QEMU
model change — which matters, because the model does not implement
`PAD_DRIVER` either.

The cost, measured by `tests/bsp-opendrain`:

```
open-drain release costs 6.33x a push-pull write
```

**Read the ratio, not the nanoseconds.** Both halves are measured with the
same counter in the same run, so whatever the clock is really doing cancels
out of the ratio. The absolute figures are not silicon figures, for two
independent reasons: the counter is the 16 MHz systimer and guest time runs
several times fast under this QEMU (see "The QEMU counter loses a tick on
every read" above), and `-icount shift=0` charges one cycle per instruction
with no memory or peripheral-bus stall — which is exactly where writing
IO_MUX costs on real hardware.

So the emulated figure, 76 ns for a release against 1-Wire's 15 us deadline,
is a floor and a loose one. What it supports is a negative claim: emulated
open drain is **not obviously too slow** for 1-Wire, which is what the
question was. Two orders of magnitude of headroom would have to evaporate
before it failed. Settling it needs hardware.

A released open-drain line with no pull-up floats and reads back whatever it
last was, so `pin_mode()` warns when `open_drain` is asked for without
`pullup`.

## Test results

459 tests, the whole suite less the performance families the runner defers by
default (benchmarks, tmtests, psxtmtests, rhealstone, spintrcritical\*).
`-j 4`, `-icount shift=0,sleep=off`, patched QEMU.

| | first measured | after the QEMU counter fix | after the psxstat backport |
|---|---|---|---|
| PASS | 414 | 419 | **420** |
| XFAIL | 14 | 14 | 14 |
| FAIL | 31 | 26 | **25** |

Nothing moved backwards at any step. What is left divides cleanly, and none of
it is unexplained:

**24 are the part being small.** 20 filesystem tests never get a RAM disk
(`ramdisk_support.c: 55 rc == 0`), `fsdosfssync01` and `fsdosfsformat01` fail
opening and formatting one, `fsrofs01` reports `buffer open failed: 6`, and
`capture01` dies on `INTERNAL_ERROR_TOO_LITTLE_WORKSPACE`. 320 KiB is not
enough for what those tests want to allocate. There is nothing to fix in the
BSP; this is the support manifest, not a defect list.

**1 is upstream-known.** `ttest01` fails `test-malloc.c:75 *ctx->c == c` on
every architecture, and is on the mbv BSP's known-failure list too.

### psxstat was the pin, not the BSP

Worth its own heading because the conclusion is about this repository's
configuration rather than about the ESP32-C3.

`psxstat` failed `test.c:790 status == -1`, asserting that `statvfs()` on a
valid path returns `-1` with `ENOSYS`. Two upstream commits have to travel
together for that to be right:

| | | |
|---|---|---|
| `e618b20215` | 2025-06-11 | gave IMFS a real `.statvfs_h`; `IMFS_statvfs()` unconditionally returns 0 |
| `4645e241a8` | 2026-07-29 | updated `psxstat`, which still expected `ENOSYS` |

The pin sits between them, so `psxstat` fails — and, as the second commit's own
message says, it fails **on every BSP**, not just this one.
`patches/rtems/psxstat-statvfs-expect-success.patch` backports the test change.
The real fix is advancing `src/rtems`, which is 139 commits behind
`origin/main`; drop the patch when the pin moves past `4645e241a8`.

### Three readings of these numbers that were wrong

All three were confident, all three are worth keeping, and they fail the same
way: a symptom was characterised instead of the failing thing being read.

**`record04` and `ttest02` were called `-icount` artifacts.** They failed with
instruction counting and passed without it, at the same timeout in both
directions. The correlation was real; the conclusion that icount is "not
uniformly good here", unlike on mbv where it is what makes those same tests
pass, was not. They were failing on the QEMU counter defect above, and icount is
simply the configuration that exposes it hardest — virtual time advancing per
instruction puts consecutive counter reads closest together. Both pass with
icount on now, and icount is no longer implicated in any failure here.

**`spcpucounter01` was called a hang.** It was a truncated log from a run on a
host that had run out of memory, where QEMU was killed before the rest reached
the serial file. A truncated log and a hang look identical. It was a clean
assertion failure, and gdb found it already sitting in `bsp_reset`.

**`psxstat` was called a permissions failure** — "a mkdir that should have
returned EACCES did not". That text was the last thing printed before the
assertion, from an earlier section of the test; line 790 is in `test_statvfs()`
and has nothing to do with permissions. Reading the line the assertion is on
would have cost less than reading the log tail did.

A fourth near-miss belongs with them: cross-checking `psxstat` against the mbv
BSP showed `PASS`, which looked like evidence the defect was ESP32-C3 specific.
That mbv build is from RTEMS `dbef4aefc2`, a different revision that already has
the fix. A same-tree comparison is only evidence if it is actually the same
tree; check the version string before drawing the conclusion.

## State

| | |
|---|---|
| Builds | yes, 632 test executables, `riscv-rtems7-gcc` 15.2.0, no `rv32imc` multilib so `rv32im/ilp32` is selected |
| `hello` | passes, on stock Espressif QEMU too |
| `ticker` and interrupts | pass on the patched QEMU; fatal spurious interrupt without it |
| `-icount shift=0,sleep=off` | works, on by default in the runner, and no longer implicated in any failure |
| Unexplained failures | none. 24 are the part being small, 1 is upstream-known |
| Evidence pipeline | not wired up. There is no `config_esp32c3db_fanalyzer.ini` or `_coverage.ini`, so `make fanalyzer` and `make coverage` will refuse; `make CONFIG=configs/config_esp32c3db.ini tests` also still calls `tools/mbv-run-tests.sh`, not the runner here |
