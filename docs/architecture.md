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

**First: `riscv/esp32c3db`.** It boots, it passes 420 of 459 RTEMS tests under
Espressif's QEMU, and the toolchain and BSP already build — see
`docs/esp32c3-bsp.md`. It is also the actual target: ESPHome runs on ESP32
hardware, so porting to the real part is the point rather than a detour.

Two further reasons it is the right *first* BSP specifically:

- **M1 and M2 need no networking at all.** Boot, time, synchronisation, wake,
  logging and the scheduler are the whole of the first two milestones, and none
  of them care what NIC the board has. The argument for Zynq A9 is entirely an
  M3 argument, so spending it in M1 buys nothing.
- **It is RISC-V, like the ESP-IDF reference lane.** Both sides of the A/B on
  the same instruction set means a difference between them is a difference in
  the runtime, which is the comparison worth having.

**M3 networking: `arm/xilinx_zynq_a9_qemu`.** This is where the original
reasoning still holds, and it holds for two concrete reasons rather than
preference.

RTEMS documents both QEMU execution and a working libbsd Cadence GEM driver on
that BSP, including the QEMU NIC invocation — and notes that QEMU does not model
the Cadence checksum offload completely, which is the kind of detail that only
appears in a path someone actually tests.

The ESP32-C3 has no such path. QEMU gives it an OpenCores Ethernet MAC at
`0x600CD000`, an address the real chip has no MAC at — the C3 is WiFi and BLE
only. **RTEMS has no OpenCores Ethernet driver**, and neither does FreeBSD, so
libbsd does not inherit one; `ethoc` is a Linux driver. Networking on the C3
lane would therefore mean writing a driver from scratch, for a device that does
not exist on the silicon, to carry a stack whose footprint has to fit in 320
KiB. All three of those are avoidable by doing M3 somewhere else.

**Second BSP, for portability: whichever of the two is not first.** Doing the
C3 first makes Zynq A9 serve double duty — it proves the backend is not
C3-shaped *and* it carries the networking milestone. A second unrelated BSP is
the definition of architectural success either way; the order does not weaken
it.

**Later, for HAL work: Raspberry Pi 4B**, which RTEMS documents with GPIO, UART,
SPI, I²C and watchdog, and which also runs under QEMU.

### Why this is not what the feasibility study said

The study put Zynq A9 first and listed the ESP32-C3 as an explicit feasibility
spike, on the grounds that no primary source established that RTEMS'
`esp32c3db` BSP and Espressif's QEMU were compatible.

That spike is answered — they are, after two QEMU fixes. Once the C3 boots, the
study's own reason for preferring Zynq first no longer applies to M1 and M2,
because that reason was networking and those milestones have none. The
networking half of the recommendation is unchanged and is stronger now than
when it was written, because the OpenCores driver gap is a measured absence
rather than an assumption.

## Where the port code lives

Both forked, both for the same reason: the work ends up as commits shaped for a
pull request rather than as patch files that have to be turned into one later.

| | fork | branch |
|---|---|---|
| `src/esphome` | `thesamprice/esphome` | `rtems` |
| `src/esp-qemu` | `thesamprice/qemu` | `esp32c3-rtems-fixes` |

`patches/` stays for what it is good at: a handful of small changes against a
tree we do not intend to carry a branch of. Today that is two RTEMS patches —
one real BSP fix and one backport of an upstream commit our pin predates.

A platform backend is not that. It adds a platform component, a
`core/wake/wake_rtems.*`, a Python build target, a HAL header and `USE_RTEMS`
through `defines.h`. Applying a diff that size on every build, and rebasing it
by hand against a tree that moves daily, is the failure mode `patches/` exists
to avoid rather than an instance of it.

ESPHome takes new platforms upstream — Zephyr and LibreTiny both arrived that
way — so the end state is a pull request, and a fork is the form that ends in.
When a change lands upstream, point the submodule back and drop the branch.

Out-of-tree `external_components` cannot host this: a *platform* is not a
component. `core/hal.h` dispatches on `USE_<platform>` to
`components/<platform>/hal.h`, and `defines.h` has to know the platform exists.
Neither is reachable from outside the tree.

Upstream `dev` moves fast. Rebase deliberately and record the new pin, because
the pin is what makes a result attributable — `scripts/manifest.sh` is the check
that a number and a tree correspond.

## Emulator lanes

Two, side by side:

```
Reference lane                     New backend lane

ESPHome YAML                       ESPHome YAML
     ▼                                  ▼
generated C++                      generated C++
     ▼                                  ▼
ESP-IDF                            RTEMS platform
FreeRTOS + lwIP                    OSAL + POSIX + BSP
     ▼                                  ▼
Espressif QEMU                     Espressif QEMU
ESP32-C3                           ESP32-C3

                                   and from M3, for networking:
                                   QEMU Zynq A9 + libbsd + Cadence GEM
```

Both lanes on the same part through M1 and M2, which is the point: the same
YAML, the same instruction set, the same emulator, and the only difference is
the runtime underneath.

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
