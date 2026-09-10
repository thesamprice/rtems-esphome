# The ESP-IDF reference lane

The A side of the A/B. It exists to notice when a change to common ESPHome code
breaks the backend that already works, which `CONTRIBUTING.md` rule 7 requires
of every refactor commit but cannot enforce on its own.

```sh
python3.12 -m venv venv && venv/bin/pip install -e ../../src/esphome
venv/bin/esphome compile reference-node.yaml

../../tools/esp-idf-ci-run.sh \
    .esphome/build/reference-node/build
```

ESPHome is installed from `src/esphome`, not from PyPI, so the lane tests the
revision this repository pins rather than whatever happens to be released.
ESPHome brings its own ESP-IDF (5.5.5 at the time of writing) and RISC-V
toolchain; the first compile downloads them and is slow, later ones are not.

The target is the ESP32-C3 rather than the original ESP32 because the C3 is
RISC-V, so an A/B against the RTEMS lane is about the runtime rather than about
the instruction set.

## What a pass means

The firmware prints `CI-MARKER` lines from its own boot hook and from a
periodic component on its third update. Both must appear.

That second one is the point. A firmware that reaches `setup()` and then wedges
looks identical, on a serial line, to one that is idling correctly — so a
verdict built on "nothing crashed" would pass a dead scheduler. Requiring the
third update means time advanced and the main loop ran.

The harness also stops early on panic signatures rather than waiting out the
timeout, because a panic is a result and spending 90 seconds discovering it is
waste.

## The console trap

`hardware_uart: UART0` in `reference-node.yaml` is not a preference.

ESPHome defaults the C3 to the USB Serial/JTAG console, and QEMU models that
peripheral as a stub: reads return 0, writes are discarded, and it is never
attached to a chardev, so there is no `-serial` index that reaches it. Left at
the default you get the ROM banner, the second stage bootloader entry, and then
silence forever.

Setting it through `logger:` rather than through `sdkconfig_options` matters.
ESPHome's logger chooses the console itself and generates the matching calls,
so overriding `CONFIG_ESP_CONSOLE_*` underneath it produces a firmware that
does not link — `logger_esp32.cpp` still references
`usb_serial_jtag_vfs_set_rx_line_endings`, which a UART build does not provide.

The RTEMS BSP hits this same trap from the other direction; see
`ESPRESSIF_USE_USB_CONSOLE` in `config_esp32c3db.ini`. It is a property of
QEMU's ESP32-C3 model rather than of either operating system, which is why it
catches both.

## Recorded result

```
verdict: PASS
  seen    CI-MARKER boot ok
  seen    CI-MARKER scheduler ok
```

ESPHome `2026.10.0-dev` from `58ca345`, ESP-IDF 5.5.5, QEMU `69094f5`.
