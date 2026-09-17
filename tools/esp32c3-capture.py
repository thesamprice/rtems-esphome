"""Reset over EN and print the console with timestamps relative to the reset.

Two things make this less trivial than it looks, and both produce a silent
empty capture that reads as a firmware failure:

  * The console on this board IS the chip's native USB-Serial-JTAG, so
    resetting the chip re-enumerates the USB device and invalidates the open
    handle.  The port has to be closed, waited for, and reopened.

  * DTR drives GPIO9 and RTS drives EN and the part samples both together, so
    roughly one reset in four leaves the ROM in "waiting for download" instead
    of booting.

So: reset, reopen, and if the application's banner does not appear, treat the
reset as failed and do it again rather than reporting a board that never ran.
"""
import os, serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem2101"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
BANNER = os.environ.get("CAP_BANNER", "***")


def wait_for_port(timeout=30):
    t = time.time()
    while time.time() - t < timeout:
        if os.path.exists(PORT):
            time.sleep(0.6)
            return True
        time.sleep(0.2)
    return False


def pulse_reset():
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = False; s.rts = False; time.sleep(0.05)
    s.dtr = False; s.rts = True;  time.sleep(0.2)
    s.dtr = False; s.rts = False
    try:
        s.close()
    except Exception:
        pass


def capture(attempts=6):
    for attempt in range(attempts):
        if not wait_for_port():
            sys.exit("%s never appeared" % PORT)
        pulse_reset()
        if not wait_for_port():
            sys.exit("%s did not come back after reset" % PORT)
        t0 = time.time()
        try:
            s = serial.Serial(PORT, 115200, timeout=0.2)
        except serial.SerialException:
            continue

        out, line, ok = [], b"", False
        while time.time() - t0 < SECS:
            try:
                d = s.read(256)
            except serial.SerialException:
                break
            for b in d:
                if b == 0x0A:
                    txt = line.decode("utf8", "replace").rstrip()
                    out.append((time.time() - t0, txt))
                    if BANNER in txt:
                        ok = True
                    line = b""
                else:
                    line += bytes([b])
            # Give up early on a reset that landed in the ROM loader.
            if not ok and time.time() - t0 > 6.0:
                if any("DOWNLOAD" in t or "waiting for download" in t
                       for _, t in out):
                    break
        try:
            s.close()
        except Exception:
            pass

        if ok:
            for ts, txt in out:
                print("%7.3f  %s" % (ts, txt))
            return
        sys.stderr.write("  (reset did not reach the application, retrying)\n")
    sys.exit("the board never reached the application")


capture()
