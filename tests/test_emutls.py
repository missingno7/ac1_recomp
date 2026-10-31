"""The MinGW host build's own __emutls_get_address (src/runtime/ac1_emutls.c)."""
from pathlib import Path
import os
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EmulatedTlsTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt" and shutil.which("gcc") and shutil.which("g++"),
                         "MinGW GCC required")
    def test_threads_variables_last_error_and_libstdcxx(self):
        work = ROOT / "work/emutls_test"
        work.mkdir(parents=True, exist_ok=True)
        objects = []
        for compiler, source, standard in (("gcc", "tests/emutls_contract.c", "-std=gnu11"),
                                           ("gcc", "src/runtime/ac1_emutls.c", "-std=gnu11"),
                                           ("g++", "tests/emutls_once.cpp", "-std=gnu++17")):
            objects.append(work / (Path(source).stem + ".o"))
            subprocess.run([compiler, "-O2", standard, "-c", str(ROOT / source), "-o", str(objects[-1])],
                           check=True, capture_output=True, timeout=60)
        exe = work / "check.exe"
        subprocess.run(["g++", *map(str, objects), "-o", str(exe)], check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60)
        self.assertIn("isolation and LastError passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
