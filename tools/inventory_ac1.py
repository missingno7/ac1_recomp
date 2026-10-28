#!/usr/bin/env python3
"""Inventory installed AC1 executables/DLLs and focused DX9 startup metadata.

The installation is read-only. Output goes only into the ignored work folder.
Requires pefile and capstone (the pinned DukeMP Python environment works).
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(r"C:\Program Files (x86)\GOG.com\Assassins Creed")
EXPECTED_DX9_SHA1 = "a9ea476e70f076709cca59a802e26ec894432120"


def sha(path: Path, algorithm="sha256"):
    h = hashlib.new(algorithm)
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def entry_disassembly(pe, count=48):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    va = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_data(rva, 256)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    return [{"va": f"0x{i.address:08X}", "bytes": i.bytes.hex(" "),
             "mnemonic": i.mnemonic, "operands": i.op_str}
            for i in list(md.disasm(data, va))[:count]]


def imports(pe):
    rows = []
    for descriptor in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = descriptor.dll.decode("ascii", "replace")
        names = []
        for item in descriptor.imports:
            names.append(item.name.decode("ascii", "replace") if item.name
                         else f"ordinal:{item.ordinal}")
        rows.append({"dll": dll, "count": len(names), "names": names})
    return rows


def tls_info(pe):
    d = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    if not d:
        return None
    s = d.struct
    start, end, callbacks = (s.StartAddressOfRawData, s.EndAddressOfRawData,
                             s.AddressOfCallBacks)
    callback_vas = []
    if callbacks:
        try:
            rva = callbacks - pe.OPTIONAL_HEADER.ImageBase
            for index in range(128):
                va = pe.get_dword_at_rva(rva + index * 4)
                if not va:
                    break
                callback_vas.append(f"0x{va:08X}")
        except (pefile.PEFormatError, AttributeError):
            pass
    return {"start_raw_data_va": f"0x{start:08X}",
            "end_raw_data_va": f"0x{end:08X}",
            "index_va": f"0x{s.AddressOfIndex:08X}",
            "callbacks_va": f"0x{callbacks:08X}",
            "callbacks": callback_vas, "zero_fill": s.SizeOfZeroFill,
            "characteristics": s.Characteristics}


def load_config(pe):
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[10]
    if not directory.VirtualAddress:
        return None
    try:
        pe.parse_data_directories(directories=[10])
        lc = pe.DIRECTORY_ENTRY_LOAD_CONFIG.struct
        names = lc.dump_dict()
        # Preserve only SEH and mitigations fields that matter to x86 startup.
        wanted = ("Size", "SEHandlerTable", "SEHandlerCount", "SecurityCookie",
                  "GuardCFCheckFunctionPointer", "GuardCFDispatchFunctionPointer",
                  "GuardCFFunctionTable", "GuardCFFunctionCount", "GuardFlags")
        return {n: names[n]["Value"] for n in wanted if n in names}
    except (AttributeError, KeyError, pefile.PEFormatError, TypeError):
        return {"parse_error": "pefile did not decode load-config directory"}


def rtti_candidates(pe):
    # MSVC type descriptors contain a pointer pair followed by a decorated
    # name such as .?AVClass@@. Report candidates, not asserted type objects.
    results = []
    for section in pe.sections:
        if section.Characteristics & 0x20000000:
            continue
        raw = section.get_data()
        pos = 0
        while len(results) < 250:
            pos = raw.find(b".?A", pos)
            if pos < 0:
                break
            end = raw.find(b"\0", pos, min(len(raw), pos + 256))
            if end > pos:
                value = raw[pos:end]
                if value.startswith((b".?AV", b".?AU", b".?AW4")):
                    results.append({"va": f"0x{pe.OPTIONAL_HEADER.ImageBase + section.VirtualAddress + pos - 8:08X}",
                                    "name": value.decode("ascii", "replace")})
            pos += 3
    return results


def inspect(path: Path, detailed=False):
    import pefile

    raw_sha256, raw_sha1 = sha(path), sha(path, "sha1")
    try:
        pe = pefile.PE(str(path), fast_load=False)
    except pefile.PEFormatError:
        return {"name": path.name, "size": path.stat().st_size,
                "sha256": raw_sha256, "sha1": raw_sha1,
                "pe": False}
    opt = pe.OPTIONAL_HEADER
    item = {"name": path.name, "size": path.stat().st_size,
            "sha256": raw_sha256, "sha1": raw_sha1, "pe": True,
            "machine": pe.FILE_HEADER.Machine,
            "machine_name": {0x14C: "I386", 0x8664: "AMD64"}.get(pe.FILE_HEADER.Machine, "other"),
            "magic": opt.Magic, "image_base": f"0x{opt.ImageBase:08X}",
            "size_of_image": opt.SizeOfImage,
            "entry_point_va": f"0x{opt.ImageBase + opt.AddressOfEntryPoint:08X}",
            "timestamp": pe.FILE_HEADER.TimeDateStamp,
            "sections": [{"name": s.Name.rstrip(b"\0").decode("ascii", "replace"),
                          "rva": s.VirtualAddress, "virtual_size": s.Misc_VirtualSize,
                          "raw_size": s.SizeOfRawData,
                          "executable": bool(s.Characteristics & 0x20000000)}
                         for s in pe.sections]}
    if detailed:
        item.update({"imports": imports(pe), "tls": tls_info(pe),
                     "load_config": load_config(pe),
                     "entry_disassembly": entry_disassembly(pe),
                     "rtti_candidates": rtti_candidates(pe)})
    pe.close()
    return item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=GAME)
    parser.add_argument("--output", type=Path, default=ROOT / "work/ac1-binary-inventory.json")
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to((ROOT / "work").resolve()):
        parser.error("output must be below work/")
    paths = sorted((p for p in args.game.iterdir()
                    if p.is_file() and p.suffix.casefold() in (".exe", ".dll")),
                   key=lambda p: p.name.casefold())
    rows = [inspect(p, p.name.casefold() == "assassinscreed_dx9.exe") for p in paths]
    dx9 = next((p for p in rows if p["name"].casefold() == "assassinscreed_dx9.exe"), None)
    if not dx9 or dx9["sha1"] != EXPECTED_DX9_SHA1:
        raise SystemExit(f"DX9 fingerprint mismatch: {dx9 and dx9['sha1']}")
    report = {"game_directory": str(args.game), "images": rows,
              "fingerprint_check": {"expected_dx9_sha1": EXPECTED_DX9_SHA1,
                                    "observed_dx9_sha1": dx9["sha1"], "matched": True}}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Inventoried {len(rows)} EXE/DLL files; DX9 SHA-1 matched; report: {out}")


if __name__ == "__main__":
    main()
