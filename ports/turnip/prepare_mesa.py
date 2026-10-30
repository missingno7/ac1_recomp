"""Materialize upstream Git symlinks for Windows without changing the clone."""
from pathlib import Path
import shutil
import subprocess
import sys

source, destination = map(lambda p: Path(p).resolve(), sys.argv[1:])
revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
marker = destination / 'AC1_SOURCE_REVISION'
if marker.exists():
    if marker.read_text().strip() != revision:
        raise SystemExit('Prepared Mesa revision differs; select a new build directory')
else:
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('.git', '__pycache__'), dirs_exist_ok=True)
entries = subprocess.check_output(['git', '-C', str(source), 'ls-files', '-s', '-z']).decode().split('\0')
for entry in filter(None, entries):
    info, filename = entry.split('\t', 1)
    if not info.startswith('120000 '):
        continue
    # CI directory links are unused by the driver build.
    if filename.startswith('.gitlab-ci/'):
        continue
    target = subprocess.check_output(['git', '-C', str(source), 'show', f'HEAD:{filename}']).decode().strip()
    resolved = (source / filename).parent.joinpath(target).resolve()
    if not resolved.is_relative_to(source):
        raise SystemExit('Symlink escapes source: ' + filename)
    out = destination / filename
    if out.is_symlink():
        out.unlink()
    shutil.copyfile(resolved, out)
marker.write_text(revision + '\n', newline='\n')
# Upstream chooses win_flex's --wincompat based on the build machine, but
# that generates <io.h> for Android too. Select the skeleton for the target.
meson = destination / 'meson.build'
text = meson.read_text()
old = "prog_flex = [prog_flex, '--wincompat']"
new = "prog_flex = host_machine.system() == 'windows' ? [prog_flex, '--wincompat'] : [prog_flex]"
if old in text:
    meson.write_text(text.replace(old, new), newline='\n')
elif new not in text:
    raise SystemExit('Pinned Mesa win_flex rule changed')
