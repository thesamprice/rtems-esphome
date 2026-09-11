#!/usr/bin/env python3
"""Report what an RTEMS image costs in flash and in RAM.

Why this exists rather than a call to <arch>-rtems7-size: that tool's three
numbers are misleading on both BSPs this port targets, in opposite directions.

  * Its "bss" includes .work, the RTEMS workspace and heap, which confdefs
    sizes to absorb whatever RAM is left over rather than to any measured
    demand.  On arm/xilinx_zynq_a9_qemu that is 250 MiB, and reporting it as
    the application's memory use is not a small error.
  * Its "text" and "data" together are the bytes that reach the part, which is
    the number worth tracking -- but the .bin beside the .elf is not, because
    objcopy pads across the gap between the flash and RAM address windows.  The
    ESP32-C3 image's .bin is 1 MiB and 74% zero.

So this classifies sections rather than summing them, and says which numbers
are demand and which are whatever was left over.

Reads the ELF directly, so it needs no cross toolchain -- which matters because
CI should be able to report a footprint without the RSB image.

Usage:
    scripts/footprint.py <file.elf> [...]      one table per image
    scripts/footprint.py --json <file.elf>     machine-readable
    scripts/footprint.py --find                every built image under tests/
"""

import argparse
import json
import pathlib
import struct
import sys

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHT_NOBITS = 8

# Sections the BSP or confdefs sizes to fill what is left, not to demand.
# Counting these as the application's cost is the error this tool exists to
# avoid; they are reported, separately, because they are not free either.
RESIDUAL = (".work", ".nocache", ".rwbarrier")

EM = {0x28: "arm", 0xF3: "riscv", 0x3E: "x86-64", 0x08: "mips"}


def sections(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    if data[4] != 1:
        raise ValueError(f"{path}: only ELF32 is handled; this port has no 64-bit BSP yet")
    (e_machine,) = struct.unpack_from("<H", data, 18)
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)

    def entry(i):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, _off, size = struct.unpack_from("<IIIIII", data, off)
        return name, typ, flags, addr, size


    # The section-header string table's own sh_offset, at +16 in its entry.
    stroff, = struct.unpack_from("<I", data, e_shoff + e_shstrndx * e_shentsize + 16)

    out = []
    for i in range(e_shnum):
        nameoff, typ, flags, addr, size = entry(i)
        end = data.index(b"\0", stroff + nameoff)
        name = data[stroff + nameoff:end].decode()
        if flags & SHF_ALLOC and size:
            out.append({"name": name, "size": size, "addr": addr,
                        "nobits": typ == SHT_NOBITS,
                        "write": bool(flags & SHF_WRITE)})
    return EM.get(e_machine, hex(e_machine)), out


def classify(secs):
    """flash = bytes in the image; ram = bytes claimed at boot."""
    flash = [s for s in secs if not s["nobits"]]
    ram = [s for s in secs if s["nobits"] or s["write"]]
    demand = [s for s in ram if s["name"] not in RESIDUAL]
    residual = [s for s in ram if s["name"] in RESIDUAL]
    return flash, demand, residual


def total(secs):
    return sum(s["size"] for s in secs)


def kib(n):
    return f"{n / 1024:.1f} KiB"


def report(path, arch, secs, verbose):
    flash, demand, residual = classify(secs)
    text = total([s for s in flash if s["name"] in (".text", ".start", ".init", ".fini")])

    print(f"{path.name}  ({arch})")
    print(f"  flash, in the image        {kib(total(flash)):>12}"
          f"   of which code {kib(text)}")
    print(f"  RAM, application           {kib(total(demand)):>12}")
    if residual:
        names = ", ".join(s["name"] for s in residual)
        print(f"  RAM, sized to fit          {kib(total(residual)):>12}   {names}")

    # A section counted in both columns is ordinary for initialized data: the
    # initial values live in the image and the variables live in RAM.  It is
    # not ordinary for a region that is only ever scratch, and on this port
    # .nocache is exactly that -- a megabyte of DMA-visible memory that the
    # Zynq BSP marks LOAD, so a megabyte of zeros is carried in the image and
    # copied at boot.  Worth naming rather than leaving in the arithmetic.
    loaded_residual = [s for s in residual if not s["nobits"]]
    if loaded_residual:
        for s in loaded_residual:
            print(f"    note: {s['name']} is {kib(s['size'])} of image bytes for a "
                  f"region that holds no initial state")
    if verbose:
        for s in sorted(flash, key=lambda s: -s["size"]):
            print(f"      flash {s['name']:<20}{kib(s['size']):>12}")
        for s in sorted(demand + residual, key=lambda s: -s["size"]):
            print(f"      ram   {s['name']:<20}{kib(s['size']):>12}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="*", type=pathlib.Path)
    ap.add_argument("--json", action="store_true", help="machine-readable, for CI")
    ap.add_argument("-v", "--verbose", action="store_true", help="per-section detail")
    ap.add_argument("--find", action="store_true",
                    help="every .elf under tests/*/.esphome/build")
    args = ap.parse_args()

    paths = list(args.elf)
    if args.find:
        top = pathlib.Path(__file__).resolve().parent.parent
        paths += sorted(top.glob("tests/*/.esphome/build/*/*.elf"))
    if not paths:
        ap.error("name an ELF, or pass --find")

    out = {}
    for p in paths:
        try:
            arch, secs = sections(p)
        except ValueError as exc:
            print(exc, file=sys.stderr)
            continue
        flash, demand, residual = classify(secs)
        if args.json:
            out[p.name] = {
                "arch": arch,
                "flash_bytes": total(flash),
                "text_bytes": total([s for s in flash
                                     if s["name"] in (".text", ".start", ".init", ".fini")]),
                "ram_application_bytes": total(demand),
                "ram_residual_bytes": total(residual),
                "sections": {s["name"]: s["size"] for s in secs},
            }
        else:
            report(p, arch, secs, args.verbose)

    if args.json:
        print(json.dumps(out, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
