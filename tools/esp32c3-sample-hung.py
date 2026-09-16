"""Poor man's profiler for the ESP32-C3 over its built-in USB-JTAG.

Runs the image, watches the console, and when it stops making progress halts
the core repeatedly and records where it is parked.  Coverage can say which
lines ran; only this can say where execution is *sitting*.

The stall is defined as: the console printed the line we are waiting for and
then went quiet for QUIET seconds.  That is what "stalls out" means from the
outside, so it is what the harness triggers on.
"""
import os, re, socket, subprocess, sys, threading, time
import serial

PORT      = os.environ.get("C3_PORT", "/dev/cu.usbmodem2101")
ELF       = os.environ.get("C3_ELF")            # required
OOCD      = os.environ.get("C3_OPENOCD", "openocd")
OOCD_S    = os.environ.get("C3_OPENOCD_SCRIPTS")
TOOLCHAIN = os.environ.get("C3_TOOLCHAIN_PREFIX",
                           os.path.expanduser("~/rtems/7/bin/riscv-rtems7-"))
ADDR2LINE = TOOLCHAIN + "addr2line"
NM        = TOOLCHAIN + "nm"
SP        = os.environ.get("C3_OUTDIR", ".")

TRIGGER  = os.environ.get("C3_TRIGGER", "connecting to")
QUIET    = float(os.environ.get("C3_QUIET", "8"))
SAMPLES  = int(os.environ.get("C3_SAMPLES", "12"))
DEADLINE = float(os.environ.get("C3_DEADLINE", "75"))


class Tcl:
    """OpenOCD's TCL RPC on 6666.  Commands and replies end with 0x1a."""
    def __init__(self, port=6666):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\x1a")
        buf = b""
        while not buf.endswith(b"\x1a"):
            chunk = self.s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf[:-1].decode("utf8", "replace")

    def close(self):
        self.s.close()


def symbolize(addrs):
    if not addrs:
        return {}
    p = subprocess.run([ADDR2LINE, "-f", "-C", "-e", ELF] + ["0x%08x" % a for a in addrs],
                       capture_output=True, text=True)
    out = p.stdout.strip().split("\n")
    res = {}
    for i, a in enumerate(addrs):
        fn  = out[2 * i]     if 2 * i     < len(out) else "??"
        loc = out[2 * i + 1] if 2 * i + 1 < len(out) else "??"
        res[a] = (fn, loc)
    return res


def load_symtab():
    """Fall back for addresses addr2line cannot place (no DWARF: blob, libmp)."""
    syms = []
    p = subprocess.run([NM, "-n", ELF], capture_output=True, text=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            try:
                syms.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    return syms


def nearest(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return "??"
    return "%s+0x%x" % (best[1], addr - best[0])


console = []
lock = threading.Lock()
stop = threading.Event()


def reader(ser):
    line = b""
    while not stop.is_set():
        try:
            d = ser.read(256)
        except Exception:
            break
        if not d:
            continue
        for b in d:
            if b == 0x0A:
                with lock:
                    console.append((time.time(), line.decode("utf8", "replace").rstrip()))
                line = b""
            else:
                line += bytes([b])


def wait_for_port(timeout=30):
    """A USB reset re-enumerates the device; the tty node lags by seconds."""
    t = time.time()
    while time.time() - t < timeout:
        if os.path.exists(PORT):
            time.sleep(1.0)      # settle, or the first open still races
            return
        time.sleep(0.5)
    sys.exit("%s never came back" % PORT)


def reset_into_the_app(ser, attempts=5):
    """Reset over EN, and make sure the chip came up running the image.

    DTR drives GPIO9 and RTS drives EN, and on this part the two are sampled
    together: a transient where DTR is asserted as EN is released leaves the
    ROM in "waiting for download" instead of booting.  It happens on perhaps a
    quarter of resets here, and the symptom -- a console that prints nothing
    and a PC parked in ROM around 0x40046000 -- reads exactly like the firmware
    hanging early, which is the one thing this harness must not confuse.

    So: hold DTR deasserted across the whole sequence, then read the ROM's own
    banner back and retry if it says DOWNLOAD.
    """
    for attempt in range(attempts):
        with lock:
            del console[:]

        ser.dtr = False
        ser.rts = False
        time.sleep(0.05)
        ser.dtr = False
        ser.rts = True          # EN low
        time.sleep(0.2)
        ser.dtr = False
        ser.rts = False         # EN high, and GPIO9 was never pulled down
        t0 = time.time()

        deadline = time.time() + 3.0
        verdict = None
        while time.time() < deadline:
            time.sleep(0.1)
            with lock:
                snap = list(console)
            for _, ln in snap:
                if "DOWNLOAD" in ln or "waiting for download" in ln:
                    verdict = "download"
                    break
                if "boot:" in ln:
                    verdict = "app"
                    break
            if verdict:
                break

        if verdict != "download":
            return t0
        print("== reset landed in ROM download mode; retrying (%d)" % (attempt + 1))

    sys.exit("the board kept coming up in download mode")


def main():
    if not ELF:
        sys.exit("set C3_ELF to the .exe that is on the board")
    wait_for_port()
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    th = threading.Thread(target=reader, args=(ser,), daemon=True)
    th.start()

    t0 = reset_into_the_app(ser)

    # Wait for the trigger line, then for the console to go quiet.
    triggered = None
    while time.time() - t0 < DEADLINE:
        time.sleep(0.25)
        with lock:
            snap = list(console)
        if triggered is None:
            for ts, ln in snap:
                if TRIGGER in ln:
                    triggered = ts
                    print("== trigger seen: %r" % ln)
                    break
        else:
            last = snap[-1][0] if snap else triggered
            if time.time() - last >= QUIET:
                print("== quiet for %.1fs -> attaching" % (time.time() - last))
                break
    else:
        print("== deadline hit; attaching anyway")

    print("== starting openocd (no reset)")
    oo = subprocess.Popen(
        [OOCD] + (["-s", OOCD_S] if OOCD_S else []) +
        ["-f", "board/esp32c3-builtin.cfg",
         "-c", "riscv set_command_timeout_sec 20",
         "-c", "init"],
        stdout=open(SP + "/pmp-openocd.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(6)
    if oo.poll() is not None:
        sys.exit("openocd died; see pmp-openocd.log")

    tcl = Tcl()
    print("== target:", tcl.cmd("targets").strip().splitlines()[-1].strip())

    syms = load_symtab()
    samples = []
    for i in range(SAMPLES):
        tcl.cmd("halt")
        regs = {}
        for r in ("pc", "ra", "sp", "mcause", "mepc", "mstatus", "mie", "mip"):
            out = tcl.cmd("reg %s" % r)
            m = re.search(r"0x([0-9a-fA-F]+)", out)
            regs[r] = int(m.group(1), 16) if m else 0
        samples.append(regs)
        tcl.cmd("resume")
        time.sleep(0.35)

    stop.set()
    time.sleep(0.3)
    ser.close()

    print("\n===== CONSOLE =====")
    with lock:
        for ts, ln in console:
            print("  %7.3f  %s" % (ts - t0, ln))

    print("\n===== SAMPLES =====")
    info = symbolize([s["pc"] for s in samples] + [s["ra"] for s in samples])
    for i, s in enumerate(samples):
        fn, loc = info.get(s["pc"], ("??", "??"))
        if fn == "??" or fn.startswith("??"):
            fn = nearest(syms, s["pc"])
            loc = ""
        rfn, _ = info.get(s["ra"], ("??", "??"))
        if rfn == "??" or rfn.startswith("??"):
            rfn = nearest(syms, s["ra"])
        print("  %2d  pc=0x%08x  %-42s %s" % (i, s["pc"], fn, loc))
        print("      ra=0x%08x  %-42s sp=0x%08x mie=0x%08x mip=0x%08x mstatus=0x%08x"
              % (s["ra"], rfn, s["sp"], s["mie"], s["mip"], s["mstatus"]))

    print("\n===== PC HISTOGRAM =====")
    hist = {}
    for s in samples:
        fn, _ = info.get(s["pc"], ("??", "??"))
        if fn == "??" or fn.startswith("??"):
            fn = nearest(syms, s["pc"]).split("+")[0]
        hist[fn] = hist.get(fn, 0) + 1
    for fn, n in sorted(hist.items(), key=lambda kv: -kv[1]):
        print("  %3d/%d  %s" % (n, len(samples), fn))

    tcl.cmd("shutdown")
    tcl.close()
    oo.wait(timeout=10)


main()
