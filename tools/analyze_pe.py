#!/usr/bin/env python3
"""Fingerprint and report the locally installed AC1 DX9 executable.

Reads the configured original executable without modifying or copying it.
Reports are written only below work/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config/ac1_dx9.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def analyze(binary: Path) -> dict:
    import pefile

    raw_hash = sha256(binary)
    pe = pefile.PE(str(binary), fast_load=False)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    entry_rva = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    sections = []
    for section in pe.sections:
        characteristics = section.Characteristics
        sections.append({
            "name": section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
            "rva": section.VirtualAddress,
            "virtual_size": section.Misc_VirtualSize,
            "raw_offset": section.PointerToRawData,
            "raw_size": section.SizeOfRawData,
            "characteristics": characteristics,
            "executable": bool(characteristics & 0x20000000),
            "readable": bool(characteristics & 0x40000000),
            "writable": bool(characteristics & 0x80000000),
        })
    imports = []
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
            dll = descriptor.dll.decode("ascii", errors="replace")
            for symbol in descriptor.imports:
                imports.append({
                    "dll": dll,
                    "name": symbol.name.decode("ascii", errors="replace") if symbol.name else None,
                    "ordinal": symbol.ordinal,
                    "iat_va": symbol.address,
                })
    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            exports.append({
                "name": symbol.name.decode("ascii", errors="replace") if symbol.name else None,
                "ordinal": symbol.ordinal,
                "rva": symbol.address,
                "va": image_base + symbol.address,
                "forwarder": symbol.forwarder.decode("ascii", errors="replace") if symbol.forwarder else None,
            })
    directories = []
    for index, directory in enumerate(pe.OPTIONAL_HEADER.DATA_DIRECTORY):
        if directory.VirtualAddress or directory.Size:
            directories.append({
                "index": index,
                "name": directory.name,
                "rva": directory.VirtualAddress,
                "size": directory.Size,
            })
    relocs = []
    if pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].VirtualAddress:
        try:
            for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
                relocs.extend({
                    "rva": entry.rva,
                    "type": entry.type,
                } for entry in block.entries)
        except pefile.PEFormatError:
            relocs = [{"error": "malformed base relocation directory"}]
    tls = None
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        item = pe.DIRECTORY_ENTRY_TLS.struct
        tls = {
            "start_raw_data_va": item.StartAddressOfRawData,
            "end_raw_data_va": item.EndAddressOfRawData,
            "index_va": item.AddressOfIndex,
            "callbacks_va": item.AddressOfCallBacks,
            "zero_fill": item.SizeOfZeroFill,
            "characteristics": item.Characteristics,
        }
    exceptions = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXCEPTION"):
        exceptions = [entry.struct.dump_dict() for entry in pe.DIRECTORY_ENTRY_EXCEPTION]
    report = {
        "path": str(binary),
        "size": binary.stat().st_size,
        "sha256": raw_hash,
        "machine": pe.FILE_HEADER.Machine,
        "machine_name": "I386" if pe.FILE_HEADER.Machine == 0x14C else "other",
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "characteristics": pe.FILE_HEADER.Characteristics,
        "optional_magic": pe.OPTIONAL_HEADER.Magic,
        "image_base": image_base,
        "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage,
        "entry_point_rva": entry_rva,
        "entry_point_va": image_base + entry_rva,
        "dll_characteristics": pe.OPTIONAL_HEADER.DllCharacteristics,
        "sections": sections,
        "data_directories": directories,
        "base_relocation_count": len(relocs),
        "base_relocations_sample": relocs[:64],
        "tls": tls,
        "exception_directory_entries": exceptions[:128],
        "imports": imports,
        "imported_dlls": sorted({item["dll"] for item in imports}, key=str.casefold),
        "exports": exports,
        "rtti_strings": [],
        "indirect_branch_count": None,
        "notes": [
            "Exception-directory details are recorded only if pefile exposes entries; x86 MSVC EH also lives in image metadata.",
            "RTTI and indirect branches need code/data-aware follow-up; these fields are intentionally not guessed.",
        ],
    }
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        indirect = 0
        for section in pe.sections:
            if not section.Characteristics & 0x20000000:
                continue
            for insn in md.disasm(section.get_data(), image_base + section.VirtualAddress):
                if CS_GRP_JUMP in insn.groups and (
                    not insn.operands or insn.operands[0].type != 2  # X86_OP_IMM
                ):
                    indirect += 1
        report["indirect_branch_count"] = indirect
    except ImportError:
        pass
    finally:
        pe.close()
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "work/ac1-dx9-pe.json")
    args = parser.parse_args()
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    binary = (args.binary or Path(config["binary"])).resolve()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output must be under {ROOT / 'work'}")
    report = analyze(binary)
    expected_hash = config["sha256"]
    if report["sha256"] != expected_hash:
        parser.error(f"binary fingerprint changed: expected {expected_hash}, got {report['sha256']}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['machine_name']} PE32 image 0x{report['image_base']:08X}, "
          f"entry 0x{report['entry_point_va']:08X}, SHA-256 {report['sha256']}")
    print(f"{len(report['sections'])} sections, {len(report['imports'])} imports, "
          f"{len(report['exports'])} exports; report: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
