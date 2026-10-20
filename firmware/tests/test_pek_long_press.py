"""AI-assisted (Claude Opus 5.5): compile the actual PEK long-press poll logic.

Bench 2026-10-20: an I2C timeout burst during mic/camera start made the
patched I2cDevice::ReadReg() return 0xFF for INTSTS2 (0x49). Bit 0x04 (PEK
long-press) is set in 0xFF, so the 200 ms PEK poll powered the robot off.
This test extracts Pmic::TryReadReg() and Pmic::ConsumePekLongPress() from
stackchan.cc and drives them against a scripted fake I2C bus. It does not
model the AXP2101 or the ESP-IDF I2C driver; the bench test (hold the power
key ~1 s and confirm power-off; run a conversation and confirm no self-off)
remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = (ROOT / "main/hal/board/stackchan.cc").read_text()


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PekLongPressTest(unittest.TestCase):
    def test_poll_uses_checked_read(self):
        body = method(CC, "bool ConsumePekLongPress(")
        self.assertNotIn("ReadReg(0x49)", body.replace("TryReadReg(0x49", ""))
        self.assertIn("pmic_->ConsumePekLongPress()", method(CC, "void PollPowerKey("))

    def test_real_pek_logic(self):
        implementation = "\n".join(
            method(CC, sig) for sig in ("bool TryReadReg(", "bool ConsumePekLongPress("))
        with tempfile.TemporaryDirectory(prefix="dotty-pek-test-") as directory:
            temp = Path(directory)
            generated = temp / "test.cc"
            generated.write_text((ROOT / "tests/pek_long_press_platform_stub.h").read_text()
                                 + implementation
                                 + (ROOT / "tests/pek_long_press_test.cpp").read_text())
            binary = temp / "pek-test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Werror", str(generated), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
