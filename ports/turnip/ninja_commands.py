"""Windows fallback for restricted-token Ninja child-process pipe hangs.

Consume Ninja's expanded commands and Meson's dependency graph. Always rebuild
the entire target closure; this does not implement Ninja incremental semantics.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

build, ninja = (Path(p).resolve() for p in sys.argv[1:3])
jobs = int(sys.argv[3])
os.chdir(build)
commands = json.loads(subprocess.check_output([str(ninja), '-t', 'compdb', '-x']))
def norm(p):
    return p.replace('\\', '/')
command_map = {norm(c['output']): c['command'] for c in commands}
def tokens(s):
    return [re.sub(r'\$([ :$])', r'\1', x) for x in re.findall(r'(?:\$[ :$]|[^\s])+', s)]
edges = {}
for line in (build / 'build.ninja').read_text().splitlines():
    if not line.startswith('build '):
        continue
    left, right = line[6:].split(': ', 1)
    outputs = [o for o in tokens(left) if o != '|']
    rule, *inputs = tokens(right)
    inputs = [d for d in inputs if d not in ('|', '||')]
    for output in outputs:
        edges[norm(output)] = (outputs, rule, inputs)
pending = {}
def add(target):
    if norm(target) not in edges:
        return
    edge = edges[norm(target)]
    key = norm(edge[0][0])
    if key in pending:
        return
    if edge[1] not in ('phony', 'CUSTOM_COMMAND', 'CUSTOM_COMMAND_DEP', 'c_COMPILER',
                       'c_COMPILER_RSP', 'cpp_COMPILER', 'cpp_COMPILER_RSP',
                       'STATIC_LINKER', 'STATIC_LINKER_RSP', 'cpp_LINKER', 'SHSYM'):
        raise RuntimeError('Unsupported Meson rule: ' + edge[1])
    pending[key] = edge
    for dependency in edge[2]:
        add(dependency)
add('src/freedreno/vulkan/libvulkan_freedreno.so')
add('src/freedreno/vulkan/freedreno_icd.armv8-a.json')
if not pending:
    raise RuntimeError('No Turnip targets in Meson graph')
print('Building full target closure:', len(pending), 'edges', flush=True)
done, running, failed = set(), {}, False
def execute(key, edge):
    outputs, rule, _ = edge
    if rule == 'phony':
        return b''
    command = command_map[key]
    for output in outputs:
        Path(output).parent.mkdir(parents=True, exist_ok=True)
    if len(command) > 20000:
        # Ninja -t compdb -x expands response files. Restore them for CreateProcess.
        if not command.startswith('"'):
            raise RuntimeError('Unquoted executable in long command: ' + key)
        executable, arguments = command[1:].split('"', 1)
        response = build / ('direct-' + hashlib.sha256(key.encode()).hexdigest()[:16] + '.rsp')
        response.write_text(arguments.strip(), encoding='utf-8')
        command = f'"{executable}" "@{response}"'
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if process.returncode:
        raise RuntimeError(key + '\n' + process.stdout.decode(errors='replace'))
    return process.stdout
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
    while pending or running:
        for key, edge in list(pending.items()):
            dependencies = [norm(edges[norm(d)][0][0]) for d in edge[2] if norm(d) in edges]
            if all(d in done for d in dependencies):
                running[pool.submit(execute, key, edge)] = key
                del pending[key]
        if not running:
            raise RuntimeError('Dependency cycle: ' + str(list(pending)[:10]))
        finished, _ = concurrent.futures.wait(running, return_when=concurrent.futures.FIRST_COMPLETED)
        for future in finished:
            key = running.pop(future)
            try:
                output = future.result()
                if output:
                    print(output.decode(errors='replace'), flush=True)
                done.add(key)
                print(f'[{len(done)}] {key}', flush=True)
            except Exception as error:
                print(error, flush=True)
                failed = True
        if failed:
            for future in running:
                future.cancel()
            break
sys.exit(1 if failed else 0)
