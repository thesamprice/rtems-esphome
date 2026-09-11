#!/bin/bash
# Bring lwip up on arm/xilinx_zynq_a9_qemu and prove a packet crossed the
# boundary, by connecting to the guest from the host.
#
# Usage:
#   tools/zynq-lwip-run.sh [options] <image.elf>
#
# Options:
#   -q QEMU   qemu-system-arm      (default: $QEMU_ARM or qemu-system-arm)
#   -o DIR    output dir           (default: ./zynq-lwip-results.<timestamp>)
#   -t SECS   hard timeout         (default: 90)
#   -p PORT   host port to forward (default: 5555)
#   -M TEXT   the marker that means success (default: "CI-MARKER net ok")
#   -n        do not connect from the host: for a test that does not listen
#   -D        capture the netdev to <out>/capture.pcap.  Some claims can only
#             be made from outside the guest -- that a packet reached the wire,
#             or that one did not -- and the guest's own log cannot make them.
#   -c SCRIPT run this host-side script instead of the built-in exchange.  It
#             is given the forwarded port as its only argument and its exit
#             status decides the verdict, which is how a lane that speaks a
#             real protocol -- the native API, say -- reports.
#
# Why this is not rtems-ci-run.sh
#   That harness boots a raw flash image with "-drive if=mtd", which is the
#   ESP32-C3's direct boot mode.  The Zynq takes an ELF through -kernel.  The
#   two are different enough at the QEMU command line that one script with a
#   flag would be mostly flag.
#
# Why the host connects rather than the guest pinging out
#   A guest that can send but not receive would pass a ping test against the
#   emulator's own gateway, because QEMU's user-mode networking answers ICMP
#   itself without a packet ever reaching the outside.  An accepted TCP
#   connection carrying bytes in both directions cannot be faked that way: it
#   needs the GEM, the driver, lwip and the host stack all working.
#
# The address is QEMU's user-mode default, 10.0.2.15 behind a NAT at 10.0.2.2,
# set statically in the guest.  DHCP would work and would be testing QEMU's
# DHCP server rather than anything here.

set -u

QEMU=${QEMU_ARM:-qemu-system-arm}
OUT=""
TMO=90
PORT=5555
MARKER="CI-MARKER net ok"
CONNECT=1
CLIENT=""
CAPTURE=0

while getopts "q:o:t:p:M:nc:D" opt; do
  case $opt in
    q) QEMU=$OPTARG;;
    o) OUT=$OPTARG;;
    t) TMO=$OPTARG;;
    p) PORT=$OPTARG;;
    M) MARKER=$OPTARG;;
    n) CONNECT=0;;
    c) CLIENT=$OPTARG;;
    D) CAPTURE=1;;
    *) exit 2;;
  esac
done
shift $((OPTIND - 1))

IMAGE=${1:-}
[ -n "$IMAGE" ] || { echo "error: no image given" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "error: no such image: $IMAGE" >&2; exit 2; }
command -v "$QEMU" >/dev/null || { echo "error: $QEMU not found" >&2; exit 2; }

[ -n "$OUT" ] || OUT=zynq-lwip-results.$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
log="$OUT/serial.log"
rm -f "$log"

# The machine already has two Cadence GEMs, so the netdev attaches to the first
# rather than being plugged in: "-device cadence_gem" is refused as not
# pluggable.  The second GEM having no peer is expected and QEMU says so.
# filter-dump attaches to the hub port the legacy -net form creates.  Off
# unless asked for: it writes every frame, which is a lot of disk for a lane
# that only needs a verdict.
DUMP_ARGS=""
if [ "$CAPTURE" = 1 ]; then
  DUMP_ARGS="-object filter-dump,id=netdump,netdev=hub0port0,file=$OUT/capture.pcap"
fi

"$QEMU" -no-reboot -display none -monitor none -M xilinx-zynq-a9 -m 256M \
  -serial null -serial file:"$log" \
  -net nic -net user,hostfwd=tcp:127.0.0.1:"$PORT"-10.0.2.15:"$PORT" \
  $DUMP_ARGS \
  -kernel "$IMAGE" > "$OUT/qemu.out" 2> "$OUT/qemu.err" &
qpid=$!

verdict=TIMEOUT
if [ "$CONNECT" = 1 ]; then
  for _ in $(seq 1 $((TMO * 2))); do
    grep -q "listening on" "$log" 2>/dev/null && break
    kill -0 $qpid 2>/dev/null || break
    sleep 0.5
  done
fi

client_rc=0
if [ -n "$CLIENT" ]; then
  # A lane with its own client waits for the application to be up rather than
  # for a "listening on" line, because a real server prints its own banner.
  for _ in $(seq 1 $((TMO * 2))); do
    grep -qE "setup\(\) finished|listening on" "$log" 2>/dev/null && break
    kill -0 $qpid 2>/dev/null || break
    sleep 0.5
  done
  sleep 1
  $CLIENT "$PORT" > "$OUT/host.log" 2>&1
  client_rc=$?
elif [ "$CONNECT" = 1 ] && grep -q "listening on" "$log" 2>/dev/null; then
  python3 - "$PORT" <<'PY' > "$OUT/host.log" 2>&1
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=15)
s.sendall(b"hello")
print("guest replied:", s.recv(32))
s.close()
PY
fi

for _ in $(seq 1 $((TMO * 2))); do
  grep -qF "$MARKER" "$log" 2>/dev/null && break
  grep -q "failure(s)" "$log" 2>/dev/null && break
  kill -0 $qpid 2>/dev/null || break
  sleep 0.5
done
kill -9 $qpid 2>/dev/null
wait $qpid 2>/dev/null

if [ -n "$CLIENT" ]; then
  # The client's exit status is the verdict: the guest's own log cannot say
  # whether a protocol exchange it is only one end of actually worked.
  if [ "$client_rc" = 0 ]; then verdict=PASS; else verdict=FAIL; fi
elif grep -qF "$MARKER" "$log" 2>/dev/null; then
  verdict=PASS
elif grep -q "failure(s)" "$log" 2>/dev/null; then
  verdict=FAIL
fi

{
  echo "verdict: $verdict"
  grep -E "^(start_networking|the interface|it has|socket|bind|listen|a connection|bytes|they are|a reply)" "$log" 2>/dev/null
  echo "host side:"; sed 's/^/  /' "$OUT/host.log" 2>/dev/null
} | tee "$OUT/result.txt"

echo
echo "serial log: $log"
[ "$verdict" = PASS ] && exit 0
exit 1
