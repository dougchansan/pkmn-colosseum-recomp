"""Read dolrecomp_hook_aware_addresses out of a built module and compare it to a hook list.

A hook-aware module bakes the addresses it was built to hook as a
null-terminated u32 array, which the runtime compares against the hooks a mod
registers. The shipped module predates the current hook list by three days
and carries nothing saying which list produced it, so read the array and
compare directly.

Parses the PE export table by hand rather than loading the library: the module
imports host symbols that only the runtime provides.

    python read_baked_hooks.py <module.dll> [hook-list.txt]
"""
import pathlib
import struct
import sys


def rva_to_offset(data, sections, rva):
    for virt, vsize, raw, rsize in sections:
        if virt <= rva < virt + max(vsize, rsize):
            return raw + (rva - virt)
    raise ValueError(f"rva {rva:#x} is in no section")


def read_export(path, wanted):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    # The data directories sit after the optional header's fixed part, which
    # differs between PE32 and PE32+.
    dir_off = opt + (112 if magic == 0x20B else 96)
    export_rva, _export_size = struct.unpack_from("<II", data, dir_off)
    if not export_rva:
        return None

    sec = pe + 24 + opt_size
    sections = []
    for i in range(n_sections):
        s = sec + i * 40
        vsize, virt, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((virt, vsize, raw, rsize))

    exp = rva_to_offset(data, sections, export_rva)
    n_names, addr_rva, names_rva, ords_rva = struct.unpack_from("<IIII", data, exp + 24)
    names_off = rva_to_offset(data, sections, names_rva)
    ords_off = rva_to_offset(data, sections, ords_rva)
    addr_off = rva_to_offset(data, sections, addr_rva)

    for i in range(n_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        start = rva_to_offset(data, sections, name_rva)
        end = data.index(b"\0", start)
        if data[start:end].decode("ascii", "replace") != wanted:
            continue
        ordinal = struct.unpack_from("<H", data, ords_off + i * 2)[0]
        sym_rva = struct.unpack_from("<I", data, addr_off + ordinal * 4)[0]
        off = rva_to_offset(data, sections, sym_rva)
        out = []
        while True:
            value = struct.unpack_from("<I", data, off)[0]
            if value == 0:
                return out
            out.append(value)
            off += 4
    return None


module = pathlib.Path(sys.argv[1])
baked = read_export(module, "dolrecomp_hook_aware_addresses")
if baked is None:
    print(f"{module.name}: no dolrecomp_hook_aware_addresses export "
          "(not a hook-aware module)")
    raise SystemExit(0)
print(f"{module.name}: {len(baked)} baked hook addresses")

if len(sys.argv) > 2:
    listed = []
    for line in pathlib.Path(sys.argv[2]).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        listed.append(int(line.split()[0], 16))
    print(f"hook list: {len(listed)} addresses")
    missing = [a for a in listed if a not in baked]
    extra = [a for a in baked if a not in listed]
    print(f"in the list but NOT baked: {len(missing)}"
          + ("  " + ", ".join(f"{a:08X}" for a in missing[:8]) if missing else ""))
    print(f"baked but not in the list: {len(extra)}"
          + ("  " + ", ".join(f"{a:08X}" for a in extra[:8]) if extra else ""))
    print("VERDICT: " + ("current" if not missing and not extra
                         else "STALE - rebuild the module"))
