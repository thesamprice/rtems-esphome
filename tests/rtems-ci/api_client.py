#!/usr/bin/env python3
"""Connect to an ESPHome node over the native API and report what it sends.

A socket poke would prove a listener exists.  This proves the node speaks the
protocol: the handshake completes, device info parses, entity metadata arrives,
and states keep arriving at the cadence the configuration asked for.
"""
import asyncio, sys
from aioesphomeapi import APIClient

HOST, PORT = "127.0.0.1", int(sys.argv[1])
WANT_STATES = 4          # at 500 ms, comfortably inside the window
WINDOW = 25.0


async def main() -> int:
    cli = APIClient(HOST, PORT, None)
    try:
        await asyncio.wait_for(cli.connect(login=True), timeout=20)
    except Exception as e:
        print(f"FAIL connect: {e!r}")
        return 1
    print("ok   handshake completed")

    try:
        info = await asyncio.wait_for(cli.device_info(), timeout=10)
        print(f"ok   device info: name={info.name!r} model={info.model!r} "
              f"manufacturer={info.manufacturer!r} esphome={info.esphome_version!r}")
    except Exception as e:
        print(f"FAIL device_info: {e!r}")
        return 1

    try:
        entities, _services = await asyncio.wait_for(cli.list_entities_services(), timeout=10)
    except Exception as e:
        print(f"FAIL list_entities: {e!r}")
        return 1
    names = sorted(getattr(e, "name", "?") for e in entities)
    print(f"ok   entity metadata: {names}")
    if not any("Counter" in n for n in names):
        print("FAIL the template sensor is not among the entities")
        return 1

    seen: list[float] = []
    done = asyncio.Event()

    def on_state(state):
        val = getattr(state, "state", None)
        if val is not None:
            seen.append(val)
            if len(seen) >= WANT_STATES:
                done.set()

    cli.subscribe_states(on_state)
    try:
        await asyncio.wait_for(done.wait(), timeout=WINDOW)
    except asyncio.TimeoutError:
        print(f"FAIL only {len(seen)} states in {WINDOW}s: {seen}")
        await cli.disconnect()
        return 1
    print(f"ok   states arrive and advance: {seen[:WANT_STATES]}")
    if seen[:WANT_STATES] != sorted(seen[:WANT_STATES]):
        print("FAIL the counter did not increase monotonically")
        return 1

    await cli.disconnect()
    print("ok   disconnected")

    # Reconnect, which is the case a node that leaks its listening socket fails.
    cli2 = APIClient(HOST, PORT, None)
    try:
        await asyncio.wait_for(cli2.connect(login=True), timeout=20)
    except Exception as e:
        print(f"FAIL reconnect: {e!r}")
        return 1
    print("ok   reconnected after a disconnect")
    await cli2.disconnect()
    print("ALL OK")
    return 0


sys.exit(asyncio.run(main()))
