#!/usr/bin/env python3
"""Validate and summarize fingerprint-pinned AC1 initializer pointer tables."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_CONFIG = ROOT / "config/ac1_dx9.json"
TABLE_CONFIG = ROOT / "config/startup_initializers.json"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def analyze(binary: Path, table_config: dict) -> dict:
    import pefile

    actual_hash = sha256(binary)
    expected_hash = table_config["binary_sha256"].lower()
    if actual_hash.lower() != expected_hash:
        raise ValueError(f"binary SHA-256 mismatch: expected {expected_hash}, got {actual_hash}")
    pe = pefile.PE(str(binary), fast_load=True)
    try:
        base = pe.OPTIONAL_HEADER.ImageBase
        width = int(table_config.get("pointer_width", 4))
        if width != 4:
            raise ValueError("initializer tables are expected to contain 32-bit x86 pointers")
        sections = []
        for section in pe.sections:
            sections.append({
                "name": section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
                "start": base + section.VirtualAddress,
                "end": base + section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData),
                "executable": bool(section.Characteristics & 0x20000000),
                "readable": bool(section.Characteristics & 0x40000000),
                "writable": bool(section.Characteristics & 0x80000000),
            })
        executable = [s for s in sections if s["executable"]]
        table_results = []
        all_valid = []
        for table in table_config["tables"]:
            start = int(table["start_va"], 0)
            end = int(table["end_va_exclusive"], 0)
            size = end - start
            if size <= 0 or size % width:
                raise ValueError(f"invalid bounds/width for table {table['name']}")
            containing = [s for s in sections if s["start"] <= start and end <= s["end"]]
            if len(containing) != 1 or not containing[0]["readable"]:
                raise ValueError(f"table {table['name']} does not fit one readable PE section")
            data = pe.get_data(start - base, size)
            if len(data) != size:
                raise ValueError(f"table {table['name']} data is truncated")
            pointers = list(struct.unpack("<" + "I" * (size // width), data))
            null_count = sum(pointer == 0 for pointer in pointers)
            valid = sorted({p for p in pointers if p and any(s["start"] <= p < s["end"] for s in executable)})
            nontext = sorted({p for p in pointers if p and not any(s["start"] <= p < s["end"] for s in executable)})
            all_valid.extend(valid)
            table_results.append({
                "name": table["name"],
                "description": table.get("description", ""),
                "start_va": start,
                "end_va_exclusive": end,
                "section": containing[0]["name"],
                "entry_count": len(pointers),
                "null_count": null_count,
                "nonnull_count": len(pointers) - null_count,
                "unique_executable_pointer_count": len(valid),
                "unique_non_executable_pointer_count": len(nontext),
                "non_executable_pointers": nontext,
                "executable_pointers": valid,
            })
        return {
            "binary": str(binary),
            "binary_sha256": actual_hash,
            "image_base": base,
            "executable_sections": executable,
            "tables": table_results,
            "unique_executable_pointers_across_tables": sorted(set(all_valid)),
            "unique_executable_pointer_count_across_tables": len(set(all_valid)),
        }
    finally:
        pe.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--tables", type=Path, default=TABLE_CONFIG)
    parser.add_argument("--output", type=Path, default=ROOT / "work/startup-initializers-report.json")
    args = parser.parse_args()
    base_config = json.loads(BASE_CONFIG.read_text(encoding="utf-8"))
    table_config = json.loads(args.tables.read_text(encoding="utf-8"))
    binary = (args.binary or Path(base_config["binary"])).resolve()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output must be under {ROOT / 'work'}")
    report = analyze(binary, table_config)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for table in report["tables"]:
        print(f"{table['name']}: {table['entry_count']} entries, {table['null_count']} null, "
              f"{table['unique_executable_pointer_count']} unique executable pointers, "
              f"{table['unique_non_executable_pointer_count']} non-executable pointers")
    print(f"unique executable initializer functions: "
          f"{report['unique_executable_pointer_count_across_tables']}; report: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
