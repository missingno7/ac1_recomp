#!/usr/bin/env python3
"""Compile DXVK's built-in shaders with NDK glslc and validate Vulkan 1.1 SPIR-V."""
# Adapted from codepdbh/nfsu2-recompiled ports/dxvk_android/shaders.py.
# Copyright (c) codepdbh; MIT per the upstream repository README.
import argparse
import hashlib
import pathlib
import re
import struct
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("source", type=pathlib.Path)
parser.add_argument("--glslc", required=True, type=pathlib.Path)
parser.add_argument("--spirv-val", required=True, type=pathlib.Path)
args = parser.parse_args()
root = args.source.resolve()
output = root / "generated"
output.mkdir(exist_ok=True)
shaders = []
for directory in ("src/dxvk/shaders", "src/dxvk/hud/shaders", "src/d3d9/shaders"):
    shader_dir = root / directory
    if shader_dir.exists():
        shaders.extend(shader_dir / item for item in sorted(shader_dir.iterdir())
                       if item.suffix in (".comp", ".frag", ".vert", ".geom"))
signature = hashlib.sha256(pathlib.Path(__file__).read_bytes())
for shader in shaders:
    signature.update(shader.relative_to(root).as_posix().encode("utf-8"))
    signature.update(shader.read_bytes())
signature = signature.hexdigest()
marker = output / "SHADERS_SHA256"
if marker.exists() and marker.read_text(encoding="ascii").strip() == signature and all(
        (output / (shader.stem + suffix)).is_file() for shader in shaders for suffix in (".h", ".spv")):
    print(f"DXVK shaders are current ({len(shaders)} Vulkan 1.1 shaders)")
    sys.exit(0)
count = 0
for shader in shaders:
    binary = output / (shader.stem + ".spv")
    header = output / (shader.stem + ".h")
    subprocess.run([str(args.glslc), "--target-env=vulkan1.1", str(shader), "-o", str(binary)], check=True)
    subprocess.run([str(args.spirv_val), "--target-env", "vulkan1.1", str(binary)], check=True)
    data = binary.read_bytes()
    if len(data) % 4:
        raise SystemExit(f"Invalid SPIR-V byte count: {shader}")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    symbol = re.sub(r"\W", "_", shader.stem)
    with header.open("w", encoding="ascii", newline="\n") as stream:
        stream.write("// Generated from " + shader.name + " for Vulkan 1.1.\n")
        stream.write("#include <stdint.h>\n")
        stream.write(f"static const uint32_t {symbol}[] = {{\n")
        for offset in range(0, len(words), 8):
            stream.write("  " + ", ".join(f"0x{word:08x}u" for word in words[offset:offset + 8]) + ",\n")
        stream.write("};\n")
    count += 1
marker.write_text(signature + "\n", encoding="ascii")
print(f"{count} built-in shaders generated and validated for Vulkan 1.1")
