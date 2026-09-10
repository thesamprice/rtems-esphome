# The lwip lane

Networking on RTEMS, on `arm/xilinx_zynq_a9_qemu` with the Cadence GEM. This is
M3's foundation: the stack ESPHome's socket backend will sit on.

`../rtems-ci/` is the ESP32-C3 lane and shares nothing with this but the idea.

## Building and running

Needs the ARM toolchain, the BSP **installed**, and rtems-lwip built and
installed against it.

```sh
# The BSP
cd src/rtems
./waf configure --out=build-zynq --prefix=$HOME/rtems/7 \
    --rtems-config=<a config.ini naming arm/xilinx_zynq_a9_qemu>
./waf build --out=build-zynq && ./waf install --out=build-zynq

# The stack
cd ../rtems-lwip
git submodule update --init --recursive          # lwip-upstream, rtems_waf
./waf configure --prefix=$HOME/rtems/7 --rtems-bsps=arm/xilinx_zynq_a9_qemu
./waf build && ./waf install

# This test
export PKG_CONFIG_PATH=$HOME/rtems/7/lib/pkgconfig
INC=$HOME/rtems/7/arm-rtems7/xilinx_zynq_a9_qemu/lib/include
arm-rtems7-gcc $(pkg-config --cflags arm-rtems7-xilinx_zynq_a9_qemu) \
    -I$INC -I$INC/lwip -O2 -g -o nettest.exe init.c \
    -llwip $(pkg-config --libs arm-rtems7-xilinx_zynq_a9_qemu)

../../tools/zynq-lwip-run.sh nettest.exe
```

rtems-lwip needed **no patching** to build for this BSP.

## What a pass means, and why it is not a ping

The guest listens; the **host connects to it** and exchanges bytes in both
directions.

A ping would be weaker to the point of being misleading. QEMU's user-mode
networking answers ICMP for its own gateway address itself, so a guest that can
transmit but never receive still gets replies. An accepted TCP connection
carrying bytes each way cannot be faked that way — it needs the GEM, the
driver, lwip, and the host stack all working.

```
Start PHY autonegotiation
autonegotiation complete
link speed for phy address 7: 100
start_networking                               ok
the interface is up                            ok
it has the address it was given                ok
socket() / bind() / listen()                   ok
a connection arrived from outside the guest    ok
bytes arrived over it                          ok
they are what the host sent                    ok
a reply could be sent                          ok

host side: guest replied: b'RTEMS'
```

## Two details that cost time

**`-device cadence_gem` is refused.** The machine already instantiates two, so
the netdev attaches to the first with `-net nic -net user,...` rather than
being plugged in. QEMU warns that the second GEM has no peer; that is expected.

**`IP_ADDR4`, not `IP4_ADDR`.** `LWIP_IPV6` is 1 in this build, so `ip_addr_t`
is the dual-stack union and the address needs its type tag set as well as its
bytes. `IP4_ADDR` fails to compile against it, which is the good case; the
failure mode worth avoiding is code that compiles and leaves the tag unset.

## What this does not yet do

* **No DHCP.** The address is static. `LWIP_DHCP` is 1, so it should work, and
  a DHCP test would largely be testing QEMU's DHCP server.
* **No mDNS.** rtems-lwip imports the headers and not the implementation —
  see rtems-esphome#63. ESPHome needs it for discovery.
* **Not wired to ESPHome.** The socket backend selection is #12; this only
  establishes that the stack underneath it works.
