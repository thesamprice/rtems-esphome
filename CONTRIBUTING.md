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
