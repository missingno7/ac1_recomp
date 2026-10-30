"""Stage only the ICD, removing Meson's build-tree stub RPATH."""
import json
from pathlib import Path
import shutil
import sys
from mesonbuild.scripts.depfixer import Elf, fix_elf

build, package = (Path(p).resolve() for p in sys.argv[1:])
library = package / 'arm64-v8a/libvulkan_freedreno.so'
shutil.copyfile(build / 'src/freedreno/vulkan/libvulkan_freedreno.so', library)
with Elf(str(library), False) as elf:
    remove = {p.encode() for paths in (elf.get_rpath(), elf.get_runpath()) if paths for p in paths.split(':')}
fix_elf(str(library), remove, b'', False)
with Elf(str(library), False) as elf:
    assert not elf.get_rpath() and not elf.get_runpath(), 'Build RPATH survived staging'
icd = json.loads((build / 'src/freedreno/vulkan/freedreno_icd.armv8-a.json').read_text())
# Android/adrenotools loads the HAL .so by name, not through an ICD manifest.
# Keep a relocatable manifest alongside the build for desktop-loader tooling.
icd['ICD']['library_path'] = 'package/arm64-v8a/libvulkan_freedreno.so'
(package.parent / 'freedreno_icd.armv8-a.json').write_text(json.dumps(icd, indent=2) + '\n', newline='\n')
