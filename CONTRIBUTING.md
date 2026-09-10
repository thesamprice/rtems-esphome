# Contributing

Read `docs/architecture.md` first. It holds the standing decisions; this file
holds the rules that keep commits from quietly undoing them.

## Hard rules

These are acceptance criteria, not suggestions. A change that breaks one of
them does not get merged, however convenient it is.

1. **Never emulate FreeRTOS on RTEMS.**
2. **No `#ifdef RTEMS_BSP_*` in ordinary ESPHome components.** BSP differences
   live under the RTEMS platform/HAL.
3. **Do not change an existing ESPHome interface because OSAL lacks an
   operation.** `Mutex::try_lock()` in particular stays.
4. **Do not replace the ESPHome scheduler with OSAL timers.**
5. **Do not route RTEMS networking through OSAL sockets** unless a prototype
   demonstrates the existing BSD backend is insufficient.
6. **No new abstraction without two implementations**, or a concrete design for
   the second one.
7. **Every refactor commit still compiles the ESP-IDF target.**
8. **Build-system changes, core OS changes and component ports are separate
   commits.**
9. **Upstream source modifications** — RTEMS, NASA OSAL, QEMU — are isolated,
   justified by an identified upstream deficiency, and shaped so they can be
   submitted upstream. See `patches/`.
10. **Every newly supported facility gets a non-interactive CI test.**
11. **Unsupported components fail validation or compilation clearly.** Never a
    partial stub that appears to work.
12. **Every QEMU test has a hard timeout and a machine-readable PASS/FAIL
    marker.**

## Commit messages

Say what the platform dependency was and what replaced it. The template:

```
Problem:
  Which platform dependency is being removed or implemented?

Existing behavior:
  Exact ESP-IDF/Host/Zephyr semantics being preserved.

Implementation:
  ESPHome-facing interface.
  RTEMS implementation.
  NASA OSAL calls, if any.

Non-goals:
  Explicitly unsupported facilities/components.

Tests:
  ESP-IDF regression.
  RTEMS unit/integration/QEMU test.

Dependency delta:
  Direct FreeRTOS/lwIP/esp_* references added/removed.

Portability:
  Why no BSP-specific behavior leaked into common code.
```

Not every commit needs every heading. A commit that adds a QEMU fix needs
Problem, Implementation and Tests; one that moves an interface needs all of
them.

## Evidence

Claims about behaviour need a run behind them. "The path now executes" and "the
defect no longer reproduces" and "this is fixed" are three different claims —
say which one you have. When a test result comes from a machine under load or a
run that was interrupted, say so rather than quoting the number as if it were
clean.

### Show the test can fail

**Before believing a pass, break the thing under test and watch the test go
red.** Then say so in the commit message, so nobody has to redo it.

This is not defensive box-ticking. Every timing, concurrency and persistence
test in this repository has had a first version that passed while measuring
nothing, and none of them looked wrong:

* Priority inheritance recorded a 0 ms block and passed, because the windows
  were shorter than the tick the tasks polled on. Caught by asking why the
  number was zero.
* Wake latency read `0us` on every sample — the path is well under a
  microsecond — and the ISR variant measured three logging calls.
* `delay()` passed under `-icount`, where deterministic execution made it land
  on exactly 50 ms every run. The real 43 ms return only appeared with icount
  off.
* The preferences lane reported thirteen passing checks against a store file
  that was never written: `save()` did not set the dirty flag, `sync()`
  returned success without writing, and every read-back came out of the
  in-memory map. The two checks that touched the file were the only honest ones
  in it.
* The same lane's corrupt-store check passed with the checksum comparison
  deleted. It flipped a byte that landed in a record's length field, which a
  cheaper check rejects first, so the defence it claimed to test was never
  reached.

The shape is always the same: **a satisfied assertion and a meaningless
measurement**. Four habits catch it.

1. **Verify the test can fail.** Remove `RTEMS_INHERIT_PRIORITY`, delete the
   checksum comparison, stub out the driver — then run. A test that still
   passes is not testing what its name says.
2. **Treat a suspiciously clean number as a defect until explained.** `0ms`,
   `0us`, min == mean == max. The wake stress test legitimately reports
   min == mean == max under icount, but that had to be worked out, not assumed.
3. **Check the preconditions of the assertion, not only the assertion.** That
   the file exists before reading it back; that the waiter blocked at all, not
   only that it did not block too long. A read-back that can be satisfied from
   a cache is not evidence about storage.
4. **Aim each check at one defence.** Where code rejects bad input two ways, a
   single malformed input only exercises whichever check runs first. Give each
   one an input the others let through.

Run timing assertions both with and without `-icount` (#47): each hides a
different class of bug.
