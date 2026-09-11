#!/usr/bin/env python3
"""Send QMP commands to a paused QEMU, then release the guest.

Used by tools/rtems-ci-run.sh -Q.  Some device state cannot be set from the
QEMU command line: QEMU's tmp105 registers its "temperature" property in
instance_init, so -device tmp105,temperature=25000 does set it, and then
realize() calls tmp105_reset(), which puts it back to 0.  Setting it over QMP
after realize is the only way to give the device a value -- and it matters,
because a sensor reading 0 C cannot be told apart from a sensor that was never
read at all.

Usage:  qmp-preboot.py <socket> <commands-file>

The commands file holds one JSON object per line; blank lines and lines
starting with # are skipped.  qmp_capabilities is sent first and cont last, so
the file contains only the commands that are its own business.

Exits non-zero if the socket never appears, if any command returns an error,
or if a reply is not valid JSON.  Failing here rather than letting the guest
run is deliberate: a test whose fixture did not take would otherwise report
whatever the device's reset value happens to be.
"""

import json
import os
import socket
import sys
import time

CONNECT_TIMEOUT = 15.0
REPLY_TIMEOUT = 10.0


def connect(path):
    deadline = time.monotonic() + CONNECT_TIMEOUT
    while time.monotonic() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except (ConnectionRefusedError, FileNotFoundError):
                pass
        time.sleep(0.1)
    sys.exit(f"qmp: socket did not accept a connection within {CONNECT_TIMEOUT}s: {path}")


class Qmp:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(REPLY_TIMEOUT)
        self.buf = b""
        self._read_object()          # the greeting

    def _read_object(self):
        """One JSON object per line, so read until a line completes."""
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                sys.exit("qmp: timed out waiting for a reply")
            if not chunk:
                sys.exit("qmp: connection closed while waiting for a reply")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            sys.exit(f"qmp: reply is not JSON: {line!r} ({exc})")

    def command(self, obj):
        self.sock.send((json.dumps(obj) + "\n").encode())
        while True:
            reply = self._read_object()
            # Events arrive interleaved with replies and are not answers.
            if "event" in reply:
                continue
            if "error" in reply:
                sys.exit(f"qmp: {obj.get('execute')} failed: {reply['error']}")
            return reply


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sock_path, cmd_path = sys.argv[1], sys.argv[2]

    with open(cmd_path) as fh:
        commands = []
        for n, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                commands.append(json.loads(line))
            except json.JSONDecodeError as exc:
                sys.exit(f"qmp: {cmd_path}:{n}: not valid JSON: {exc}")

    q = Qmp(connect(sock_path))
    q.command({"execute": "qmp_capabilities"})
    for cmd in commands:
        q.command(cmd)
        print(f"ok: {json.dumps(cmd)}")
    q.command({"execute": "cont"})
    print("ok: guest released")


if __name__ == "__main__":
    main()
