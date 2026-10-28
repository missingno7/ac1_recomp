#!/usr/bin/env python3
"""Prepare the small pinned runtime subset reused from the local Duke project."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / "config/ac1_dx9.json").read_text(encoding="utf-8"))
DUKE_ROOT = Path(CONFIG["pcrecomp_root"]).resolve().parents[1]
FINGERPRINT_HEADER = ROOT / "src/runtime/ac1_fingerprint.h"
SOURCE_FILES = {
    "src/runtime/guest_memory.c": ("src/runtime/guest_memory.c", "6beb27ea60b92faa8b197554cc650cc50b57bdc31729a11d1f3599bebc4bc22c"),
    "src/runtime/guest_memory.h": ("src/runtime/guest_memory.h", "b2074c584a8f7566a1de98a651e1cb4567920b22d95b66f130c8a655cb4104a0"),
    "src/runtime/pe32_loader.c": ("src/runtime/pe32_loader.c", "0cdd8787c31898f98f5bfffeb2b91ae6892711e5eae9a6b61f7a9d0222f69402"),
    "src/runtime/pe32_loader.h": ("src/runtime/pe32_loader.h", "612c669eaa50ceef205b5989b6db18489f59c30da22cf8a1dc9b61562b6c270e"),
    "src/runtime/guest_clock.c": ("src/runtime/guest_clock.c", "e30a6a9444fb98928fe925c130376a96b1d10776d89eedddd2c5e4439334cf51"),
    "src/runtime/guest_clock.h": ("src/runtime/guest_clock.h", "717ee58822b319d0db590fb1d550f40fd83cdea8c8b203269af82867e58a246a"),
    "src/runtime/pcrecomp_heap.c": ("src/runtime/pcrecomp_heap.c", "63054a3cfc491af7de2d69df17b628fb714df0b34bd34197091ea1e89d5b9f1b"),
    "src/runtime/pcrecomp_heap.h": ("src/runtime/pcrecomp_heap.h", "885795647e5b396a6d2010824c8266beea3b49b45d22cfa376578fad621bda00"),
    "src/runtime/pcrecomp_heap.inc": ("work/generated/pcrecomp_heap.inc", "8cac9f155a4b5b8ac7668419d54044b4cfb072ac75a80e0515208b86b54e47fc"),
    "src/runtime/LICENSE.pcrecomp.txt": (".deps/pcrecomp/LICENSE", "969c26f4e5da359fea84127dfecd3057729cfed75211fb324bdb42af6c087614"),
    "src/runtime/cpu.h": ("work/generated/cpu.h", "ded33fb92194b50c42d80f03ec9142f3517bb62400545ca57f00e903e3383dfe"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    provenance = {
        "duke_reference": str(DUKE_ROOT),
        "pcrecomp_root": CONFIG["pcrecomp_root"],
        "license": "pcrecomp-derived allocator files retain the upstream MIT license below src/runtime",
        "files": [],
    }
    for destination, (source_relative, expected_hash) in SOURCE_FILES.items():
        source = DUKE_ROOT / source_relative
        actual_hash = sha256(source)
        if actual_hash != expected_hash:
            raise SystemExit(
                f"pinned runtime input changed: {source} sha256={actual_hash}; "
                f"expected {expected_hash}"
            )
        output = ROOT / destination
        output.parent.mkdir(parents=True, exist_ok=True)
        content = source.read_bytes()
        if destination == "src/runtime/guest_memory.h":
            text = content.decode("utf-8")
            marker = "void guest_memory_release(uint32_t base);"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory header shape: {source}")
            text = text.replace(marker, marker + "\nuint64_t guest_memory_region_size(uint32_t base);")
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/guest_memory.c":
            text = content.decode("utf-8")
            marker = "void guest_memory_reset(void)"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory source shape: {source}")
            text = text.replace(
                marker,
                "uint64_t guest_memory_region_size(uint32_t base)\n"
                "{\n"
                "    if (!selected_memory) return 0;\n"
                "    for (size_t i = 0; i < current_memory.count; ++i)\n"
                "        if (current_memory.regions[i].base == base)\n"
                "            return current_memory.regions[i].size;\n"
                "    return 0;\n"
                "}\n\n"
                + marker,
            )
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/cpu.h":
            text = content.decode("utf-8")
            marker = "} CPU;"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected pcrecomp CPU header shape: {source}")
            text = text.replace(
                marker,
                marker + "\n\n/* Project-owned native dispatcher, called by generated code. */\n"
                "void dispatch(CPU *cpu, uint32_t target);\n"
                "void dispatch_jmp(CPU *cpu, uint32_t target);\n"
                "void ac1_unsupported_instruction(uint32_t guest_va, const char *reason);",
            )
            todo = "#define RECOMP_TODO(va, text) abort()"
            if text.count(todo) != 1:
                raise SystemExit(f"unexpected pcrecomp RECOMP_TODO hook in {source}")
            text = text.replace(todo, "#define RECOMP_TODO(va, text) ac1_unsupported_instruction((va), (text))")
            content = text.replace("\r\n", "\n").encode("utf-8")
        if not output.exists() or output.read_bytes() != content:
            output.write_bytes(content)
        provenance["files"].append({
            "source": str(source),
            "destination": destination,
            "sha256": actual_hash,
        })
    expected_game_hash = str(CONFIG["sha256"]).lower()
    if len(expected_game_hash) != 64 or any(c not in "0123456789abcdef" for c in expected_game_hash):
        raise SystemExit("config/ac1_dx9.json contains an invalid executable SHA-256")
    fingerprint_text = (
        "/* Generated from config/ac1_dx9.json; do not edit. */\n"
        f'#define AC1_EXPECTED_EXE_SHA256 "{expected_game_hash}"\n'
    )
    if not FINGERPRINT_HEADER.exists() or FINGERPRINT_HEADER.read_text(encoding="utf-8") != fingerprint_text:
        FINGERPRINT_HEADER.write_text(fingerprint_text, encoding="utf-8")
    provenance["expected_game_executable_sha256"] = expected_game_hash
    generated_dir = ROOT / "work"
    generated_dir.mkdir(exist_ok=True)
    provenance_path = generated_dir / "runtime-provenance.json"
    provenance_text = json.dumps(provenance, indent=2) + "\n"
    if not provenance_path.exists() or provenance_path.read_text(encoding="utf-8") != provenance_text:
        provenance_path.write_text(provenance_text, encoding="utf-8")
    print(f"prepared {len(SOURCE_FILES)} pinned runtime files from {DUKE_ROOT}")


if __name__ == "__main__":
    main()
