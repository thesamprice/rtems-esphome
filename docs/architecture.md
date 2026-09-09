# Architecture decisions

These are the standing decisions for the port. They come from the project's
feasibility study; the issues in this repository implement them. Where a
decision is contested by evidence found during implementation, change the
decision here in the same commit that acts on it.

## The shape of the port

Add RTEMS as a native ESPHome runtime platform. Do not port ESP-IDF to RTEMS,
and do not emulate FreeRTOS on RTEMS.

ESPHome already has the seams this needs — a platform-selected core HAL, abstract
`GPIOPin`/`InternalGPIOPin`, a virtual `UARTComponent`, an `I2CBus` reduced to
`write_readv()`, platform-specific preferences behind a contract, a socket
abstraction with a BSD implementation, and a per-platform main-loop wake
dispatcher. Host and Zephyr backends already exist and already replace FreeRTOS
semantics rather than emulating them. The port is mostly an exercise in
extending those seams, not in building a new umbrella HAL.

```
                 ESPHome
                    │
            portable existing APIs
                    │
         ┌──────────┴──────────┐
         │                     │
      ESP-IDF                RTEMS
         │             ┌───────┼────────┐
     FreeRTOS/lwIP   NASA OSAL POSIX  libbsd
         │                     │
       ESP32              RTEMS BSPs
```

## NASA OSAL is a building block, not the foundation

Use OSAL where the semantics map cleanly: monotonic time, delays,
task-context binary and counting semaphores, and queues where a component's
semantics fit.

Do not route everything through it. Three specific gaps drive this:

- ESPHome's `Mutex` contract has `try_lock()`. OSAL's mutex API exposes
  take/give with no non-blocking operation. Use POSIX/RTEMS synchronization
  rather than weakening the ESPHome interface.
- OSAL's task API cannot pass an instance argument to a task entry point, which
  ESPHome needs. Prefer `pthread_create()` or the native RTEMS task API.
- OSAL calls should not be assumed ISR-safe. ESPHome's ISR-safe wake path needs
  documented RTEMS primitives.

**The ESPHome-facing API encodes ESPHome's semantics. OSAL is one
implementation source, not the definition of those semantics.**

## Networking goes straight to BSD sockets

```
ESPHome socket abstraction  →  existing BSD backend  →  RTEMS libbsd
```

Not through OSAL sockets. ESPHome already wraps `connect()`, `accept()`,
`setsockopt()`, `getsockopt()`, vectored I/O, UDP send/receive, shutdown and
socket monitoring. OSAL's socket layer is an abstraction over the same BSD
calls that exposes fewer socket-option semantics than ESPHome already uses, so
inserting it would cost capability and gain nothing.

DNS is the one real networking blocker: MQTT currently calls lwIP's
`dns_gethostbyname_addrtype()` directly and handles asynchronous completion. It
needs a platform-neutral resolver boundary so the existing async lwIP path and
an RTEMS `getaddrinfo()` worker can coexist. A blocking `getaddrinfo()` on the
main loop is not acceptable — the ESPHome event loop is latency-sensitive.

## Keep the ESPHome scheduler

ESPHome owns its scheduler and builds timeouts and intervals on `millis_64()`.
It does not fundamentally depend on FreeRTOS software timers. The right
abstraction is **clock below scheduler**, not **OSAL timer replacing
scheduler**.

## A FreeRTOS OSAL backend is not on the critical path

There is no current upstream FreeRTOS backend in NASA OSAL. The best-known
public baseline, METECS `OSAL-PC-FreeRTOS`, targets the FreeRTOS Windows
simulator, is written against OSAL 5.0.0, has no networking or select, and was
last pushed in March 2021.

Requiring it first would turn one port into two simultaneous ports. Treat it as
reference material. A modern ESP-IDF FreeRTOS OSAL backend is a separate,
optional deliverable *after* the RTEMS architecture is proven.

## Target BSPs

**First: `arm/xilinx_zynq_a9_qemu`.** RTEMS documents both running the BSP under
QEMU and a working libbsd Cadence GEM driver, including the QEMU NIC
invocation. That is exactly the combination the first networking milestone
needs.

**Second: an AArch64 QEMU BSP (`qemu_a53`/`qemu_a72`)** to prove the backend is
portable rather than Zynq-shaped. A second unrelated BSP is the definition of
architectural success.

**Later, for HAL work: Raspberry Pi 4B**, which RTEMS documents with GPIO, UART,
SPI, I²C and watchdog, and which also runs under QEMU.

**ESP32-C3 (`riscv/esp32c3db`): the feasibility spike is answered — it boots.**
The charter listed this as an open question. This repository is that spike: see
`docs/esp32c3-bsp.md`. RTEMS runs on Espressif's ESP32-C3 QEMU model and passes
most of its own testsuite, after one four-line QEMU fix. What is *not*
established is networking on it — QEMU's esp32c3 machine models OPENCORES_ETH
and does not emulate the radio, and the part has 320 KiB of RAM, so libbsd's
footprint is an open question there. Zynq A9 remains the first committed
networking target.

## Emulator lanes

Two, side by side:

```
Reference lane                     New backend lane

ESPHome YAML                       ESPHome YAML
     ▼                                  ▼
generated C++                      generated C++
     ▼                                  ▼
ESP-IDF                            RTEMS platform
FreeRTOS + lwIP                    OSAL + POSIX/libbsd + BSP
     ▼                                  ▼
Espressif QEMU                     QEMU Zynq A9
ESP32 / ESP32-C3                   Cadence GEM
```

The reference lane is not optional. Every common-code change must keep building
and running the existing ESP-IDF backend, and Espressif's QEMU plus
`pytest-embedded` is how that gets checked without hardware.

## Definition of success

**First milestone that means anything:** an ESPHome-generated RTEMS image
running under QEMU, with scheduler, logging and wake behaviour intact, and the
native ESPHome API reachable across RTEMS libbsd.

Hardware buses, OTA, TLS-heavy components, audio, cameras, BLE, Zigbee and
OpenThread follow later. Unsupported components must fail validation clearly
rather than compile into stubs that appear to work.
