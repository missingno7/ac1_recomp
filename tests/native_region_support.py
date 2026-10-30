"""Fingerprint-checked private image fixture shared by region oracles."""
import hashlib
import json
from pathlib import Path
import pefile

def guest_image(root: Path, output: Path) -> Path:
    if not output.resolve().is_relative_to((root/'work').resolve()):
        raise ValueError('original image fixtures must remain under ignored work/')
    config=json.loads((root/'config/ac1_dx9.json').read_text())
    raw=Path(config['binary']).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=config['sha256']:
        raise ValueError('original image fingerprint differs from config')
    pe=pefile.PE(data=raw,fast_load=True)
    output.mkdir(parents=True,exist_ok=True)
    result=output/'guest-image.bin';image=pe.get_memory_mapped_image();pe.close()
    if not result.exists() or result.read_bytes()!=image:result.write_bytes(image)
    return result
