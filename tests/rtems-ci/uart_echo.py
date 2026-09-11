#!/usr/bin/env python3
"""Echo whatever arrives on a UNIX socket, upper-cased.

The far end of the UART lane's second serial port.  Vendored here from the
builder tree rather than referenced across repositories, because CI for this
repository cannot reach that one.

Upper-cased rather than verbatim so that the guest reading its own bytes back
cannot be confused with a UART that loops TX to RX internally -- a real fault
that a verbatim echo would pass.
"""
import os, socket, sys, threading, time

path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)
srv.settimeout(60)
print(f"listening on {path}", flush=True)
try:
    conn, _ = srv.accept()
except socket.timeout:
    print("no connection", flush=True)
    sys.exit(1)
conn.settimeout(60)
deadline = time.time() + 55
while time.time() < deadline:
    try:
        data = conn.recv(256)
    except socket.timeout:
        break
    if not data:
        break
    print(f"echo {data!r}", flush=True)
    conn.sendall(bytes(data).upper())
conn.close()
