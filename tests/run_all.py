"""Run every host test file on its own and fail if any fails. Needs the installed game."""
import pathlib
import subprocess
import sys

TESTS = pathlib.Path(__file__).resolve().parent
ROOT = TESTS.parent
SCRIPTS = sorted(TESTS.glob('test_*.py')) + [TESTS/'run_scheduler_contract.py', TESTS/'run_critical_section_span.py',
                                             TESTS/'run_win32_compat.py']

failed = []
for script in SCRIPTS:
    run = subprocess.run([sys.executable, '-B', str(script)], cwd=TESTS if script.name.startswith('test_') else ROOT,
                         capture_output=True, text=True, errors='replace')
    print(f'{"ok  " if run.returncode == 0 else "FAIL"} {script.name}', flush=True)
    if run.returncode:
        failed.append(script.name)
        print((run.stdout + run.stderr)[-2000:])
print(f'{len(SCRIPTS) - len(failed)} of {len(SCRIPTS)} passed')
sys.exit(1 if failed else 0)
