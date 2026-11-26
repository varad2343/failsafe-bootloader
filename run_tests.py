"""Build and run the host tests with GCC."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
gcc = os.environ.get("CC") or shutil.which("gcc")
if not gcc:
    sys.exit("gcc not found: add it to PATH or set CC")
(ROOT / "build").mkdir(exist_ok=True)
exe = ROOT / "build" / ("test_boot.exe" if os.name == "nt" else "test_boot")
subprocess.run([gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-Isrc",
                "src/boot.c", "test/test_boot.c", "-o", exe], cwd=ROOT, check=True)
out = subprocess.run([exe], capture_output=True, text=True)
print(out.stdout, end="")
sys.exit(0 if out.returncode == 0 and "ALL TESTS PASSED" in out.stdout else 1)
