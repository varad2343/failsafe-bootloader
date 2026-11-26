"""Cross-compile the bootloader for the STM32G0B1 with the Arm GNU toolchain."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
(ROOT / "build").mkdir(exist_ok=True)
subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-Wall", "-Wextra", "-Werror",
                "-ffreestanding", "--specs=nano.specs", "-nostartfiles", "-Isrc", "-T", "target/boot.ld",
                "src/boot.c", "target/port.c", "target/startup.c", "-o", "build/boot.elf"], cwd=ROOT, check=True)
subprocess.run(["arm-none-eabi-size", "build/boot.elf"], cwd=ROOT, check=True)
