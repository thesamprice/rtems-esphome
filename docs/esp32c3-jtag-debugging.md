# Halting the C3 while it is stuck

Coverage says which lines ran. It cannot say where execution is *sitting*, and
for a fault that presents as "the console stops printing" that is the only
question worth asking. The devkit's built-in USB-Serial-JTAG answers it with no
extra hardware: the same cable carries the console and the debug port.

Three things get in the way, none of them obvious, and each looks like an
unsupported adapter rather than what it is.

## A killed OpenOCD wedges the port until it is drained

The chip buffers its JTAG responses. An OpenOCD that dies without releasing the
interface -- `timeout` sending SIGKILL is enough -- leaves its last replies
queued, and every later session reads those stale bytes first:

```
Error: esp_usb_jtag: IN buffer overflow! (0, size 160)
Error: missing data from bitq interface
Error: [esp32c3] Unsupported DTM version: -1
```

and, once past that, an IDCODE shifted by a byte:

```
Info : JTAG tap: esp32c3.tap0 tap/device found: 0xffffffff
Warn : Unexpected idcode after end of chain: 160 0x005c2565
```

This survives a chip reset, is identical on mainline 0.12 and on Espressif's
fork, and does not depend on `adapter speed` -- sweeping 40000 down to 1000
changes nothing. It reads exactly like a driver that does not support the part.

`tools/esp32c3-usbjtag-drain.py` clears it: it reads the vendor interface's IN
endpoint dry and resets the device. After that the TAP reads `0x00005c25`
first try. Note that the reset re-enumerates, so `/dev/cu.usbmodem*` disappears
for a few seconds; wait for the node before opening it.

Kill OpenOCD with SIGTERM (`timeout -15`, not the default SIGKILL) and this
does not arise. Note that MacPorts' `timeout` spells the signal `-15`, and
rejects both `-TERM` and `-s TERM` with a usage message -- silently running
nothing at all, if the exit status is not checked.

## Mainline OpenOCD is not enough

Homebrew's 0.12.0 carries the `esp_usb_jtag` driver but fails against chip
revision v0.4 even with a clean endpoint. Espressif's build works:

```
gh release download v0.12.0-esp32-20260831 --repo espressif/openocd-esp32 \
  -p 'openocd-esp32-macos-arm64-*.tar.gz'
tar xzf openocd-esp32-macos-arm64-*.tar.gz
openocd-esp32/bin/openocd -s openocd-esp32/share/openocd/scripts \
  -f board/esp32c3-builtin.cfg
```

There is no need for a hand-written config; `board/esp32c3-builtin.cfg` is
correct for the devkit.

## `reset halt` is not a neutral way to start

It issues an `RTC_SW_SYS_RST`, and our PHY bring-up does not survive one. Under
a software core reset the radio comes up dead -- `esp_wifi_scan` reports zero
networks and `esp_wifi_set_config()` answers `ESP_ERR_NO_MEM` -- where the same
image reset over EN (`rst:0x15`) scans and associates normally.

So a debugger that resets the target is measuring a boot the fault never takes.
`tools/esp32c3-sample-hung.py` resets over the EN pin the way esptool does,
waits for the console to go quiet, and only then attaches, with OpenOCD given
`init` alone and no reset of any kind. Examining a *running* core works fine
once the endpoint is clean.

That our init depends on power-on state a core reset does not restore is a real
gap -- stock ESP-IDF's bootloader clears the modem domain on every path -- and
is worth fixing rather than working around.

## A reset over EN lands in the ROM about a quarter of the time

DTR drives GPIO9 and RTS drives EN, and the part samples both together. A
transient where DTR is asserted as EN is released leaves the chip in the ROM
loader rather than booting the image:

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x5 (DOWNLOAD(USB/UART0/1))
waiting for download
```

This matters more than it sounds, because of how it presents when the console
is not being read carefully: nothing is printed at all, and a halt puts the PC
in ROM around `0x40046000` with `sp` above the linker's RAM region. That looks
exactly like the firmware hanging before its first output -- and it produced
two false "stalls" here before the banner was checked.

`reset_into_the_app()` holds DTR deasserted across the whole sequence and then
reads the ROM's own `boot:` line back, retrying if it says DOWNLOAD. Any run
that still fails to finish after that is the firmware's doing.

## Sampling

```sh
tools/esp32c3-usbjtag-drain.py
C3_ELF=path/to/image.exe \
C3_OPENOCD=.../openocd-esp32/bin/openocd \
C3_OPENOCD_SCRIPTS=.../openocd-esp32/share/openocd/scripts \
C3_TRIGGER="asking for a DHCP lease" C3_QUIET=12 \
  tools/esp32c3-sample-hung.py
```

It prints the console with timestamps, then N halts with `pc`, `ra`, `sp`,
`mie`, `mip` and `mstatus`, symbolized through the cross `addr2line` with an
`nm` fallback for the blob and anything else built without DWARF, and a
histogram of where the PC landed.

Read `mie` and the symbol together before concluding anything. A PC parked in
`bsp_reset` with `ra` in `bsp_fatal_extension` and `mie=0` is not a hang: it is
the halt loop every RTEMS image ends in, including after a clean `exit(0)`. It
means the run *finished* while the harness was waiting, not that it stopped
there.
