# Clear the ESP32-C3 built-in USB-JTAG endpoint.
#
# A SIGKILLed OpenOCD leaves its last responses queued in the chip's JTAG IN
# FIFO.  The next session's first scan then reads those stale bytes, which
# OpenOCD reports as "IN buffer overflow! (0, size 160)" and cannot recover
# from -- a chip reset does not help, because the USB block is not reset with
# the core.  Draining the endpoint (and resetting the device) does.
import sys, usb.core, usb.util

dev = usb.core.find(idVendor=0x303A, idProduct=0x1001)
if dev is None:
    sys.exit("no ESP32-C3 USB-JTAG device (303a:1001) found")

# The JTAG function is a vendor-specific interface; the CDC ones belong to the
# kernel's serial driver and must be left alone.
cfg = dev.get_active_configuration()
jtag = None
for intf in cfg:
    if intf.bInterfaceClass == 0xFF:
        jtag = intf
        break
if jtag is None:
    sys.exit("no vendor-specific (JTAG) interface on the device")

print("JTAG interface %d, endpoints: %s" % (
    jtag.bInterfaceNumber,
    ", ".join("0x%02x" % e.bEndpointAddress for e in jtag)))

ep_in = usb.util.find_descriptor(
    jtag, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
    == usb.util.ENDPOINT_IN)

drained = 0
for _ in range(64):
    try:
        data = dev.read(ep_in.bEndpointAddress, ep_in.wMaxPacketSize, timeout=50)
    except usb.core.USBTimeoutError:
        break
    except usb.core.USBError:
        break
    if not len(data):
        break
    drained += len(data)
print("drained %d stale bytes" % drained)

try:
    dev.reset()
    print("device reset")
except usb.core.USBError as e:
    print("reset failed (harmless if the drain sufficed): %s" % e)
