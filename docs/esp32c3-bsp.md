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

`scripts/build_esp_qemu.sh` fetches `src/esp-qemu` if it is not there, applies
`patches/esp-qemu/`, configures and builds. The binary is left runnable in the
build tree at `src/esp-qemu/build/qemu-system-riscv32`, which is where the test
runner looks; installing it is optional and only worth doing to put it on a
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

`patches/esp-qemu/intmatrix-status.patch` is that plus the two register
indices. It is a patch like any other here, applied by
`scripts/apply_patches.sh` to the `src/esp-qemu` submodule. Still needed as of
`esp-develop-9.2.2-20260417`: the released binaries and the `esp-develop` tip
carry byte-identical unpatched files.

`src/esp-qemu` is declared `update = none`, so on a default clone there is
nothing there to patch. That is not a failure — the tree is meant to work
without it — so `apply_patches.sh` now skips an unfetched opt-in submodule and
stays green, unless you name the module on the command line, which is what
`build_esp_qemu.sh` does after fetching it.

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

`patches/esp-qemu/systimer-counter-remainder.patch`. `sp69`'s 600 ms period
goes from 599996499 ns to 600000375 ns, from 5.8 ppm short to 0.6 ppm long, and
five tests move from fail to pass.

Note that this is not RTEMS-specific and not `-icount`-specific. Any guest that
reads the systimer counter more often than once per 62.5 ns sees a slow clock;
instruction counting only makes the reads land closer together and so exposes it
hardest.

## Memory

`ESP32C_CODE_REGION_SIZE` is 1 MiB of the 4 MiB flash, and RAM is 320 KiB at
`0x3fc80000`. The BSP pulls in `tstsmallmem`, so the testsuite is already
configured for a small part. The whole suite links: 632 executables, no
overflow.

## Test results

459 tests, the whole suite less the performance families the runner defers by
default (benchmarks, tmtests, psxtmtests, rhealstone, spintrcritical\*).
`-j 2`, `-icount shift=0,sleep=off`, patched QEMU.

| | before the counter fix | after |
|---|---|---|
| PASS | 414 | **419** |
| XFAIL | 14 | 14 |
| FAIL | 31 | **26** |

`patches/esp-qemu/systimer-counter-remainder.patch` moved five tests from fail
to pass — `sp69`, `spcpucounter01`, `sptimecounter02`, `record04` and `ttest02`
— and moved nothing the other way. The remaining 26 fall into three groups.

**23 are the part being small.** 20 filesystem tests never get a RAM disk
(`ramdisk_support.c: 55 rc == 0`), `fsdosfssync01` and `fsdosfsformat01` fail
opening and formatting one, `fsrofs01` reports `buffer open failed: 6`, and
`capture01` dies on `INTERNAL_ERROR_TOO_LITTLE_WORKSPACE`. 320 KiB is not
enough for what those tests want to allocate. Nothing to fix in the BSP.

**2 are the timecounter**, `sp69` and `sptimecounter02`, described above.

**24 are the part being small.** 20 filesystem tests never get a RAM disk
(`ramdisk_support.c: 55 rc == 0`), `fsdosfssync01` and `fsdosfsformat01` fail
opening and formatting one, `fsrofs01` reports `buffer open failed: 6`, and
`capture01` dies on `INTERNAL_ERROR_TOO_LITTLE_WORKSPACE`. 320 KiB is not
enough for what those tests want to allocate. Nothing to fix in the BSP.

**1 is upstream-known.** `ttest01` fails `test-malloc.c:75 *ctx->c == c`, and it
fails on every architecture; it is on the mbv BSP's known-failure list too.

**1 is not explained.** `psxstat` fails `test.c:790 status == -1` — a mkdir that
should have returned EACCES did not. It runs a long way, 48 KB of output, before
that, and it is not a RAM disk failure, so it is not simply the part being
small. This is the one left worth someone's time.

### What the earlier reading of these numbers got wrong

Two things, both worth keeping because both were confident and both were wrong.

`record04` and `ttest02` were recorded as `-icount` artifacts on the grounds
that they failed with instruction counting and passed without it, at the same
timeout in both directions. That correlation was real. The conclusion drawn
from it — that icount is "not uniformly good here", unlike on mbv where it is
what makes those same tests pass — was not. They were failing on the QEMU
counter defect described above, and icount is simply the configuration that
exposes it hardest: with virtual time advancing per instruction, consecutive
counter reads land closest together, which is exactly where the truncation lost
the most. Both pass with icount once the counter is fixed.

`spcpucounter01` was recorded as a hang. It was a truncated log from a run on a
host that had run out of memory, and QEMU was killed before the rest reached the
serial file. A truncated log and a hang look identical.

The general lesson is the same one twice: a correlation between a knob and a
failure is not the mechanism, and the state of the machine is part of the
evidence.

## State

| | |
|---|---|
| Builds | yes, 632 test executables, `riscv-rtems7-gcc` 15.2.0, no `rv32imc` multilib so `rv32im/ilp32` is selected |
| `hello` | passes, on stock Espressif QEMU too |
| `ticker` and interrupts | pass on the patched QEMU; fatal spurious interrupt without it |
| `-icount shift=0,sleep=off` | works, on by default in the runner, and no longer implicated in any failure |
| Evidence pipeline | not wired up. There is no `config_esp32c3db_fanalyzer.ini` or `_coverage.ini`, so `make fanalyzer` and `make coverage` will refuse; `make CONFIG=configs/config_esp32c3db.ini tests` also still calls `tools/mbv-run-tests.sh`, not the runner here |
