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
            text = content.decode("utf-8").replace("\r\n", "\n")
            capacity = (
                "/* Original map/renderer allocation exceeds the initial 64-region probe cap. */\n"
                "#define GUEST_MEMORY_MAX_REGIONS 256u"
            )
            if text.count(capacity) != 1:
                raise SystemExit(f"unexpected guest-memory capacity definition: {source}")
            text = text.replace(
                capacity,
                "/* AC1 startup exceeds the donor's 256-region bookkeeping capacity. */\n"
                "#define GUEST_MEMORY_MAX_REGIONS 4096u",
            )
            marker = "void guest_memory_release(uint32_t base);"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory header shape: {source}")
            text = text.replace(marker, marker + "\nuint64_t guest_memory_region_size(uint32_t base);")
            marker = "int guest_memory_read(uint32_t address, void *output, size_t width);"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory read declaration: {source}")
            text = text.replace(
                marker,
                marker + "\n"
                "/* Checked fixed-width CPU accesses may cross adjacent mappings. */\n"
                "int guest_memory_read_scalar(uint32_t address, void *output, size_t width);\n"
                "int guest_memory_write_scalar(uint32_t address, const void *input, size_t width);\n"
                "void guest_execution_read(uint32_t address, void *output, size_t width);\n"
                "void guest_execution_write(uint32_t address, const void *input, size_t width);",
            )
            content = text.replace("\r\n", "\n").encode("utf-8")
        if destination == "src/runtime/guest_memory.c":
            text = content.decode("utf-8").replace("\r\n", "\n")
            checkpoint = (
                "    if (current_memory.count==65) {\n"
                '        printf("guest-memory mapping checkpoint: count=65 base=0x%08x size=%llu\\n",\n'
                "               guest_base,(unsigned long long)size);\n"
                "        fflush(stdout);\n"
                "    }\n"
            )
            if text.count(checkpoint) != 1:
                raise SystemExit(f"unexpected guest-memory checkpoint shape: {source}")
            text = text.replace(checkpoint, "")
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
            marker = "void *guest_ptr(uint32_t address, size_t width)\n"
            if text.count(marker) != 1:
                raise SystemExit(f"unexpected guest-memory pointer implementation: {source}")
            scalar_helpers = r'''/* CPU scalar accesses can cross adjacent guest mappings whose host backing
 * allocations are unrelated. Validate the full guest span first, then copy
 * it in per-region chunks. Raw guest_ptr intentionally remains one-region. */
static guest_memory_region_t *scalar_region_at(uint64_t address)
{
    if (!selected_memory || address >= UINT64_C(0x100000000)) return NULL;
    for (size_t i = 0; i < current_memory.count; ++i) {
        guest_memory_region_t *region = &current_memory.regions[i];
        uint64_t end = (uint64_t)region->base + region->size;
        if (address >= region->base && address < end) return region;
    }
    return NULL;
}

static int scalar_span_mapped(uint64_t begin, uint64_t end)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        if (!region) return 0;
        uint64_t region_end = (uint64_t)region->base + region->size;
        if (region_end <= at) return 0;
        at = region_end < end ? region_end : end;
    }
    return 1;
}

static void scalar_copy_from(uint64_t begin, uint64_t end, uint8_t *output)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(output, region->data + (size_t)(at - region->base), chunk);
        output += chunk;
        at += chunk;
    }
}

static void scalar_copy_to(uint64_t begin, uint64_t end, const uint8_t *input)
{
    uint64_t at = begin;
    while (at < end) {
        guest_memory_region_t *region = scalar_region_at(at);
        uint64_t region_end = (uint64_t)region->base + region->size;
        size_t chunk = (size_t)((region_end < end ? region_end : end) - at);
        memcpy(region->data + (size_t)(at - region->base), input, chunk);
        input += chunk;
        at += chunk;
    }
}

int guest_memory_read_scalar(uint32_t address, void *output, size_t width)
{
    if (!output || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(output, single, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_from(address, end, (uint8_t *)output);
    return 1;
}

int guest_memory_write_scalar(uint32_t address, const void *input, size_t width)
{
    if (!input || !width || width > 16u) return 0;
    uint64_t end = (uint64_t)address + width;
    if (end > UINT64_C(0x100000000)) return 0;
    void *single = guest_memory_try_ptr(address, width);
    if (single) {
        memcpy(single, input, width);
        return 1;
    }
    if (!scalar_span_mapped(address, end)) return 0;
    scalar_copy_to(address, end, (const uint8_t *)input);
    return 1;
}

void guest_execution_read(uint32_t address, void *output, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(output, guest_execution_base + address, width);
        return;
    }
#endif
    if (!guest_memory_read_scalar(address, output, width))
        fail("unmapped scalar read", address, width);
}

void guest_execution_write(uint32_t address, const void *input, size_t width)
{
#ifdef DUKEMP_DIRECT_MEMORY
    if (guest_execution_base) {
        memcpy(guest_execution_base + address, input, width);
        return;
    }
#endif
    if (!guest_memory_write_scalar(address, input, width))
        fail("unmapped scalar write", address, width);
}

'''
            text = text.replace(marker, scalar_helpers + marker)
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
            access_replacements = {
                "memcpy(&v, guest_execution_ptr(a, 1), 1);": "guest_execution_read(a, &v, 1);",
                "memcpy(&v, guest_execution_ptr(a, 2), 2);": "guest_execution_read(a, &v, 2);",
                "memcpy(&v, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &v, 4);",
                "memcpy(&v, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &v, 8);",
                "memcpy(guest_execution_ptr(a, 1), &v, 1);": "guest_execution_write(a, &v, 1);",
                "memcpy(guest_execution_ptr(a, 2), &v, 2);": "guest_execution_write(a, &v, 2);",
                "memcpy(guest_execution_ptr(a, 4), &v, 4);": "guest_execution_write(a, &v, 4);",
                "memcpy(guest_execution_ptr(a, 8), &v, 8);": "guest_execution_write(a, &v, 8);",
                "memcpy(&f, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &f, 4);",
                "memcpy(&d, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &d, 8);",
                "memcpy(&i, guest_execution_ptr(a, 2), 2);": "guest_execution_read(a, &i, 2);",
                "memcpy(&i, guest_execution_ptr(a, 4), 4);": "guest_execution_read(a, &i, 4);",
                "memcpy(&i, guest_execution_ptr(a, 8), 8);": "guest_execution_read(a, &i, 8);",
                "memcpy(guest_execution_ptr(a, 4), &f, 4);": "guest_execution_write(a, &f, 4);",
                "memcpy(guest_execution_ptr(a, 4), &i, 4);": "guest_execution_write(a, &i, 4);",
                "memcpy(guest_execution_ptr(a, 2), &i, 2);": "guest_execution_write(a, &i, 2);",
                "memcpy(guest_execution_ptr(a, 8), &i, 8);": "guest_execution_write(a, &i, 8);",
                "memcpy(&x, guest_execution_ptr(a, 16), 16);": "guest_execution_read(a, &x, 16);",
                "memcpy(guest_execution_ptr(a, 16), &v, 16);": "guest_execution_write(a, &v, 16);",
                "memcpy(b, guest_execution_ptr(a, 10), 10);": "guest_execution_read(a, b, 10);",
                "memcpy(guest_execution_ptr(a, 10), b, 10);": "guest_execution_write(a, b, 10);",
            }
            for old, new in access_replacements.items():
                if text.count(old) == 0:
                    raise SystemExit(f"expected CPU guest-memory operand not found: {old}")
                text = text.replace(old, new)
            if "guest_execution_ptr(a," in text:
                raise SystemExit("CPU header still has an unconverted fixed-width memory operand")
            content = text.replace("\r\n", "\n").encode("utf-8")
        if not output.exists() or output.read_bytes() != content:
            output.write_bytes(content)
        provenance["files"].append({
            "source": str(source),
            "destination": destination,
            "sha256": actual_hash,
            "prepared_sha256": hashlib.sha256(content).hexdigest(),
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
