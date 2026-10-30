"""Run standalone probes in separate shell processes; never access the game app."""
import argparse
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--adb', default=str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk/platform-tools/adb.exe'))
parser.add_argument('--serial', default='R5CX41MRV6V')
parser.add_argument('--direct', action='store_true', help='Also probe the ICD directly to diagnose namespace failures')
args = parser.parse_args()
adb = [args.adb, '-s', args.serial]
build = root / 'work/build/turnip-arm64'
out = root / 'work/turnip'
out.mkdir(parents=True, exist_ok=True)
remote = '/data/local/tmp/ac1-turnip-probe'
def checked(*command):
    subprocess.run(adb + list(command), check=True)
checked('shell', 'mkdir', '-p', remote)
checked('push', str(build / 'ac1_vulkan_probe'), remote + '/')
for library in sorted((build / 'package/arm64-v8a').glob('*.so')):
    checked('push', str(library), remote + '/')
checked('shell', 'chmod', '755', remote + '/ac1_vulkan_probe')
metadata = subprocess.run(adb + ['shell', 'getprop ro.product.model; getprop ro.build.version.release; getprop ro.build.version.sdk; id; ls -l /dev/kgsl-3d0'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(out / 'probe-device.txt').write_bytes(metadata.stdout)
for mode in ['system', 'turnip'] + (['direct'] if args.direct else []):
    command = f'cd {remote} && LD_LIBRARY_PATH={remote} ./ac1_vulkan_probe {mode}'
    if mode != 'system':
        command += ' ' + remote + '/'
    process = subprocess.run(adb + ['shell', command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = process.stdout + f'\nprobe_exit={process.returncode}\n'.encode()
    (out / f'probe-{mode}.txt').write_bytes(output)
    print(output.decode(errors='replace'), flush=True)
    pid = re.search(rb'probe_pid=(\d+)', output)
    if pid:
        log = subprocess.run(adb + ['logcat', '-d', '-v', 'threadtime', '--pid=' + pid[1].decode(), '-t', '200'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f'probe-{mode}-logcat.txt').write_bytes(log.stdout)
