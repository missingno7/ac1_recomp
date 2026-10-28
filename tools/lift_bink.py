#!/usr/bin/env python3
"""Generate an isolated CPU-struct static lift of the pinned AC1 Bink DLL.

This experiment uses the AC1-owned PE/CFG driver and generated CPU ABI. It
reads the installed DLL only; catalogs and translated C remain under work/.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.metadata
import json
import re
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PCRECOMP = Path(r"D:\Prog\dukemp_recomp\.deps\pcrecomp")
EXPECTED_SHA256 = "603d0a96d1f27422f88d78b20ca525975ebe5e0d7667e62b98a3db80766702b4"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cached_outputs_match(output: Path, previous: dict) -> bool:
    """Accept a cache hit only for the exact CMake source glob and hashed outputs."""
    expected_tus = set(previous.get("translation_units", []))
    output_hashes = previous.get("output_sha256")
    if not expected_tus or not isinstance(output_hashes, dict) or not output_hashes:
        return False
    actual_tus = {path.name for path in output.glob("initial_*.c")}
    if actual_tus != expected_tus:
        return False
    output_root = output.resolve()
    for relative, expected in output_hashes.items():
        path = (output / relative).resolve()
        if not path.is_relative_to(output_root) or not path.is_file():
            return False
        if sha256(path) != expected:
            return False
    return True


def remove_stale_initial_sources(output: Path, active_sources: set[str]) -> None:
    """Keep the exact CMake `initial_*.c` glob equal to the active shard list."""
    for stale in output.glob("initial_*.c"):
        if stale.name not in active_sources:
            stale.unlink()


def run_cache_selftest() -> None:
    """Exercise empty-stamp rejection and exact source-glob matching."""
    root = ROOT / "work"
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bink-cache-selftest-", dir=root) as temporary:
        output = Path(temporary)
        shard = output / "initial_0000.c"
        shard.write_text("static int bink_test;\n", encoding="utf-8")
        report = {
            "translation_units": [shard.name],
            "output_sha256": {shard.name: sha256(shard)},
        }
        assert cached_outputs_match(output, report)
        assert not cached_outputs_match(output, {**report, "output_sha256": {}})
        stray = output / "initial_dispatch.c"
        stray.write_text("/* must not enter CMake's wildcard */\n", encoding="utf-8")
        assert not cached_outputs_match(output, report)
        remove_stale_initial_sources(output, {shard.name})
        assert not stray.exists()
        assert cached_outputs_match(output, report)


def executable_va(va: int, base: int, sections) -> bool:
    return any(
        section.Characteristics & 0x20000000 and
        base + section.VirtualAddress <= va <
        base + section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData)
        for section in sections
    )


def adapt_module_addresses(source_dir: Path, output_dir: Path,
                           translation_units: list[str], base: int,
                           image_size: int, call_returns_by_site: dict[int, int]) -> dict[str, int]:
    """Rebase internal transfers/return slots and namespace this DLL's symbols."""
    from lift_ac1 import write_text_if_changed

    end = base + image_size
    counts = {"internal_dispatch": 0, "call_return": 0}
    for name in translation_units:
        source = (source_dir / name).read_text(encoding="utf-8")
        adapted_lines = []
        for line in source.splitlines():
            is_call = re.search(r"/\*\s*([0-9A-Fa-f]{8}):\s*call\b", line)
            if is_call:
                site = int(is_call.group(1), 16)
                # All original direct calls have a return slot at next EIP;
                # alter only that exact literal on the source-mapped call.
                # The size table is encoded in the source comment's address
                # and is supplied by the instruction scan below.
                return_va = call_returns_by_site.get(site)
                if return_va is not None:
                    old = f"push32(c, 0x{return_va:08X}u)"
                    if old in line:
                        line = line.replace(old, f"push32(c, GVA(0x{return_va:08X}u))")
                        counts["call_return"] += 1
            def rebase_dispatch(match):
                target = int(match.group(1), 16)
                if base <= target < end:
                    counts["internal_dispatch"] += 1
                    return f"dispatch(c, GVA(0x{target:08X}u))"
                return match.group(0)
            line = re.sub(r"dispatch\(c,\s*0x([0-9A-Fa-f]{8})u\)", rebase_dispatch, line)
            adapted_lines.append(line)
        source = "\n".join(adapted_lines) + "\n"
        source = re.sub(r"\bL_([0-9A-Fa-f]{8})\b", r"BINK_L_\1", source)
        write_text_if_changed(output_dir / name, source)

    remove_stale_initial_sources(output_dir, set(translation_units))

    dispatch = source_dir / "initial_dispatch.c"
    source = dispatch.read_text(encoding="utf-8")
    source = re.sub(r"\bL_([0-9A-Fa-f]{8})\b", r"BINK_L_\1", source)
    source = source.replace("initial_dispatch.h", "bink_dispatch.h")
    source = source.replace("ac1_lifted_fn", "bink_lifted_fn")
    source = source.replace("ac1_lifted_entry", "bink_lifted_entry")
    source = source.replace("ac1_lifted_functions", "bink_lifted_functions")
    source = source.replace("ac1_lifted_function_count", "bink_lifted_function_count")
    source = source.replace("ac1_lifted_lookup", "bink_lifted_lookup")
    write_text_if_changed(output_dir / "bink_dispatch.c", source)
    header = (source_dir / "initial_dispatch.h").read_text(encoding="utf-8")
    header = header.replace("ac1_lifted_fn", "bink_lifted_fn")
    header = header.replace("ac1_lifted_entry", "bink_lifted_entry")
    header = header.replace("ac1_lifted_functions", "bink_lifted_functions")
    header = header.replace("ac1_lifted_function_count", "bink_lifted_function_count")
    header = header.replace("ac1_lifted_lookup", "bink_lifted_lookup")
    write_text_if_changed(output_dir / "bink_dispatch.h", header)
    write_text_if_changed(
        output_dir / "bink_list.h",
        (source_dir / "initial_list.h").read_text(encoding="utf-8").replace(
            "LIFTED_FUNCS", "BINK_LIFTED_FUNCS"))
    return counts


def call_return_sites(binary: Path, pcrecomp: Path, catalog: dict) -> dict[int, int]:
    """Map decoded call sites to their actual next-instruction guest VA."""
    sys.path.insert(0, str(pcrecomp / "tools/pe"))
    import pefile
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    pe = pefile.PE(str(binary))
    base = int(pe.OPTIONAL_HEADER.ImageBase)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    result = {}
    for function in catalog:
        for block in function["blocks"]:
            for address, size in block["instructions"]:
                code = pe.get_data(address - base, size)
                instruction = next(decoder.disasm(code, address, count=1), None)
                if instruction and instruction.size == size and instruction.mnemonic == "call":
                    result[address] = address + size
    pe.close()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/binkw32.json")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--pcrecomp", type=Path, default=DEFAULT_PCRECOMP)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "work/lift_bink")
    parser.add_argument("--max-functions", type=int)
    parser.add_argument("--max-depth", type=int)
    parser.add_argument("--translation-unit-buckets", type=int)
    parser.add_argument("--root-set", choices=("exports", "entry", "attach"),
                        help="all executable exports plus DllMain, only DllMain, or DllMain + _BinkSetMemory@8")
    parser.add_argument("--if-needed", action="store_true",
                        help="skip unchanged analysis/lifting when pinned inputs and generated outputs match")
    parser.add_argument("--selftest", action="store_true",
                        help="run the focused generated-source cache check")
    args = parser.parse_args()
    if args.selftest:
        run_cache_selftest()
        print("Bink generated-source cache self-test: ok")
        return 0
    config_path = args.config.resolve()
    config = json.loads(config_path.read_text(encoding="utf-8"))
    binary = (args.binary or Path(config["binary"])).resolve()
    output = args.output_dir.resolve()
    if not output.is_relative_to((ROOT / "work").resolve()):
        parser.error(f"output-dir must be under {ROOT / 'work'}")
    if config.get("sha256", "").lower() != EXPECTED_SHA256 or sha256(binary) != EXPECTED_SHA256:
        parser.error("installed binkw32.dll SHA-256 does not match the pinned AC1 DLL")
    preferred_base = int(config.get("preferred_base", "0"), 0)
    configured_size = int(config.get("image_size", "0"), 0)
    configured_entry_rva = int(config.get("entry_rva", "0"), 0)
    root_set = args.root_set or config.get("root_set", "attach")
    max_functions = args.max_functions or int(config.get("max_functions", 10000))
    max_depth = args.max_depth if args.max_depth is not None else int(config.get("max_depth", 3))
    translation_unit_buckets = (args.translation_unit_buckets
                                if args.translation_unit_buckets is not None
                                else int(config.get("translation_unit_buckets", 16)))
    observed_seeds = [int(value, 0) if isinstance(value, str) else int(value)
                      for value in config.get("observed_seeds", [])]

    import pefile
    pe = pefile.PE(str(binary))
    base = int(pe.OPTIONAL_HEADER.ImageBase)
    image_size = int(pe.OPTIONAL_HEADER.SizeOfImage)
    entry_rva = int(pe.OPTIONAL_HEADER.AddressOfEntryPoint)
    if (pe.FILE_HEADER.Machine != pefile.MACHINE_TYPE["IMAGE_FILE_MACHINE_I386"] or
            base != preferred_base or entry_rva != configured_entry_rva or
            image_size != configured_size):
        parser.error("Bink PE machine/base/DllMain identity changed")
    tls = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"]]
    if tls.VirtualAddress or tls.Size:
        parser.error("Bink static TLS is unsupported by this first lift experiment")
    import_counts = collections.Counter()
    import_symbol_counts = collections.Counter()
    for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
        dll = descriptor.dll.decode("ascii", "replace").lower()
        import_counts[dll] += 1
        import_symbol_counts[dll] += len(descriptor.imports)
    export_roots = set()
    export_records = []
    export_count = 0
    for export in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        va = base + int(export.address)
        export_name = export.name.decode("ascii", "replace") if export.name else None
        export_records.append({"name": export_name, "ordinal": int(export.ordinal),
                               "guest_va": va, "executable": executable_va(va, base, pe.sections),
                               "forwarder": export.forwarder.decode("ascii", "replace")
                               if export.forwarder else None})
        if executable_va(va, base, pe.sections):
            export_roots.add(va)
            export_count += 1
    sections = [
        {"name": section.Name.rstrip(b"\0").decode("ascii", "replace"),
         "rva": int(section.VirtualAddress), "virtual_size": int(section.Misc_VirtualSize),
         "raw_size": int(section.SizeOfRawData),
         "executable": bool(section.Characteristics & 0x20000000)}
        for section in pe.sections
    ]
    relocation_types = collections.Counter(
        entry.type for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
        for entry in block.entries)
    pe.close()
    if translation_unit_buckets < 1:
        parser.error("translation-unit-buckets must be positive")

    import subprocess
    pcrecomp = args.pcrecomp.resolve()
    pcrecomp_revision = subprocess.check_output(
        ["git", "-C", str(pcrecomp), "rev-parse", "HEAD"], text=True).strip()
    from lift_ac1 import sha256 as driver_sha256
    input_identity = {
        "binary_sha256": EXPECTED_SHA256,
        "config_sha256": sha256(config_path),
        "lift_bink_sha256": sha256(Path(__file__).resolve()),
        "lift_ac1_sha256": driver_sha256(ROOT / "tools/lift_ac1.py"),
        "pcrecomp_revision": pcrecomp_revision,
        "pcrecomp_disasm_sha256": sha256(pcrecomp / "tools/disasm/disasm32.py"),
        "pcrecomp_lifter_sha256": sha256(pcrecomp / "tools/lift/lift32_cpu.py"),
        "python_version": sys.version.split()[0],
        "capstone_version": importlib.metadata.version("capstone"),
        "pefile_version": importlib.metadata.version("pefile"),
        "root_set": root_set,
        "observed_seeds": sorted(set(observed_seeds)),
        "max_functions": max_functions,
        "max_depth": max_depth,
        "translation_unit_buckets": translation_unit_buckets,
    }
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "bink-lift-report.json"
    if args.if_needed and report_path.is_file():
        try:
            previous = json.loads(report_path.read_text(encoding="utf-8"))
            if (previous.get("inputs") == input_identity and
                    cached_outputs_match(output, previous)):
                print(f"Bink lift unchanged: {previous['lifted_function_count']} functions; skipping")
                return 0
        except (OSError, ValueError, KeyError):
            pass

    sys.path.insert(0, str(ROOT / "tools"))
    import lift_ac1
    started = time.perf_counter()
    if root_set == "exports":
        extra_roots = sorted(export_roots)
    elif root_set == "attach":
        set_memory = next((item for item in export_records
                           if item["name"] == "_BinkSetMemory@8" and item["executable"]), None)
        if set_memory is None:
            parser.error("pinned _BinkSetMemory@8 export was not found")
        extra_roots = [set_memory["guest_va"]]
    elif root_set == "entry":
        extra_roots = []
    else:
        parser.error(f"unsupported root set in config: {root_set}")
    extra_roots.extend(observed_seeds)
    for seed in extra_roots:
        if not executable_va(seed, base, pe.sections):
            parser.error(f"configured Bink seed is not in executable image code: 0x{seed:08X}")
    info, dis, catalog, failures, queued, entry, iat, _callback_edges = lift_ac1.discover_entry_cone(
        binary, pcrecomp, max_functions, max_depth, sorted(set(extra_roots)))
    catalog_functions = sorted(catalog.values(), key=lambda item: item["address"])
    call_returns = call_return_sites(binary, pcrecomp, catalog_functions)

    raw_output = output / ".pcrecomp_raw"
    lift_result = lift_ac1.emit_lift(
        binary, pcrecomp, catalog_functions, raw_output,
        functions_per_tu=100, translation_unit_buckets=translation_unit_buckets)
    address_adaptations = adapt_module_addresses(
        raw_output, output, lift_result["translation_units"],
        base, image_size, call_returns)
    todo_count = sum((output / name).read_text(encoding="utf-8").count("RECOMP_TODO")
                     for name in lift_result["translation_units"])
    report = {
        "binary": str(binary), "binary_sha256": EXPECTED_SHA256,
        "machine": "PE32/i386", "preferred_base": base, "image_size": image_size,
        "entry_rva": entry_rva, "entry_va": entry,
        "root_set": root_set,
        "exports_seeded": export_count if root_set == "exports" else 0,
        "unique_executable_export_roots": len(export_roots),
        "root_addresses": ([f"0x{x:08X}" for x in extra_roots] + [f"0x{entry:08X}"]),
        "direct_call_depth": max_depth, "max_functions": max_functions,
        "import_dll_counts": dict(sorted(import_counts.items())),
        "import_symbol_counts": dict(sorted(import_symbol_counts.items())),
        "static_tls_directory": {"rva": tls.VirtualAddress, "size": tls.Size},
        "relocation_entries": sum(relocation_types.values()),
        "relocation_types": {str(key): value for key, value in sorted(relocation_types.items())},
        "iat_slots": len(iat),
        "discovered_function_count": len(catalog),
        "lifted_function_count": len(lift_result["lifted"]),
        "lift_failures": lift_result["lift_failures"],
        "discovery_failures": failures, "queue_remaining": queued,
        "translation_unit_buckets": translation_unit_buckets,
        "translation_units": lift_result["translation_units"],
        "unsupported_count": todo_count,
        "unsupported_sites": sorted({line.strip() for name in lift_result["translation_units"]
                                      for line in (output / name).read_text(encoding="utf-8").splitlines()
                                      if "RECOMP_TODO(" in line}),
        "address_adaptations": address_adaptations,
        "sections": sections,
        "elapsed_seconds": round(time.perf_counter() - started, 3),
        "runtime_note": "Internal direct transfers and call return slots use GVA; runtime must set g_image_delta to this DLL's load-base delta while dispatching Bink code.",
        "inputs": input_identity,
    }
    export_lines = [
        "/* Generated fingerprint and export map for the pinned AC1 Bink DLL. */",
        "#pragma once", "#include <stdint.h>",
        f'#define BINK_MODULE_SHA256 "{EXPECTED_SHA256}"',
        f"#define BINK_MODULE_PREFERRED_BASE 0x{base:08X}u",
        f"#define BINK_MODULE_IMAGE_SIZE 0x{image_size:08X}u",
        f"#define BINK_MODULE_ENTRY_RVA 0x{entry_rva:08X}u",
        "typedef struct { const char *name; uint16_t ordinal; uint32_t guest_va; } bink_export_record;",
        "static const bink_export_record bink_export_records[] = {",
    ]
    for record in export_records:
        export_lines.append(
            f"    {{{json.dumps(record['name']) if record['name'] is not None else '0'}, "
            f"{record['ordinal']}u, 0x{record['guest_va']:08X}u}},")
    export_lines.extend(("};", f"#define BINK_MODULE_EXPORT_COUNT {len(export_records)}u", ""))
    lift_ac1.write_text_if_changed(output / "bink_module.h", "\n".join(export_lines))
    generated_outputs = [output / name for name in lift_result["translation_units"]]
    generated_outputs.extend(output / name for name in (
        "bink_dispatch.c", "bink_dispatch.h", "bink_list.h", "bink_module.h"))
    report["output_sha256"] = {
        path.relative_to(output).as_posix(): sha256(path) for path in generated_outputs
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Bink export/entry lift: {len(catalog)} discovered, {len(lift_result['lifted'])} emitted, "
          f"{todo_count} TODO sites, {len(lift_result['lift_failures'])} lift failures, "
          f"{report['elapsed_seconds']:.2f}s")
    print(f"Address adaptations: {address_adaptations}; manifest: {output / 'bink-lift-report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
