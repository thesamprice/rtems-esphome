#!/usr/bin/env python3
"""Check that the node advertised itself and that the stack survived it.

Read from a capture of the netdev rather than from the guest: the guest cannot
say whether a packet reached the wire, and the failure this guards against --
a fault on lwIP's thread during the announcement -- kills the stack silently.
"""
import os
import re
import socket
import struct
import sys
import time

port = int(sys.argv[1])
capture = os.environ.get("ZYNQ_CAPTURE", "capture.pcap")

failures = []


def check(what, ok):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures.append(what)


# The API port answering after the announcement is what proves the stack is
# still alive.  Before #78 this connect failed, because the lwIP thread had
# died building the announcement.
alive = False
deadline = time.time() + 20
while time.time() < deadline and not alive:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.close()
        alive = True
    except OSError:
        time.sleep(1)
check("the API port still answers after advertising", alive)

# Give the announcement sequence time to finish before reading the capture.
time.sleep(3)

probes = announces = 0
strings = set()
try:
    data = open(capture, "rb").read()
except OSError as exc:
    check(f"the capture exists ({exc})", False)
    sys.exit(1)

off = 24
while off + 16 <= len(data):
    _, _, caplen, _ = struct.unpack("<IIII", data[off:off + 16])
    off += 16
    pkt = data[off:off + caplen]
    off += caplen
    if len(pkt) < 42 or struct.unpack(">H", pkt[12:14])[0] != 0x0800 or pkt[23] != 17:
        continue
    ihl = (pkt[14] & 0xF) * 4
    sport, dport = struct.unpack(">HH", pkt[14 + ihl:14 + ihl + 4])
    if 5353 not in (sport, dport):
        continue
    dns = pkt[14 + ihl + 8:]
    if len(dns) < 12:
        continue
    _, flags, _, _, _, _ = struct.unpack(">HHHHHH", dns[:12])
    if flags & 0x8000:
        announces += 1
        for m in re.finditer(rb"[ -~]{4,}", dns):
            strings.add(m.group().decode("ascii", "ignore"))
    else:
        probes += 1

print(f"     {probes} probe(s), {announces} announcement(s)")

# RFC 6762: three probes, then announce.  Asserting "at least" rather than
# exactly, because a retransmission is legal and not a regression.
check("it probed before claiming the name", probes >= 3)
check("it announced", announces >= 1)
check("the announcement names the esphome service", any("_esphomelib" in s for s in strings))
check("and carries the configuration's TXT records", any("board=" in s for s in strings))

if failures:
    print(f"FAIL {len(failures)} check(s)")
    sys.exit(1)
print("ALL OK")
