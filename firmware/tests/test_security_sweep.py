"""AI-assisted (Claude Opus 5.5): compile the actual security-sweep methods.

Issue #166: the security yaw sweep never ran. The sweep used to live in a
dedicated FreeRTOS task whose stack allocation was unchecked and which drove
the servo bus without the LVGL lock. It is now a leg sequencer advanced from
StateManager::_update(). This test extracts the real methods from
state_manager.cpp, compiles them against fake motion/HAL/app boundaries, and
drives them with a fake clock. It does not simulate servos, the SCS bus, or
FreeRTOS scheduling; the physical bench test remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "main/stackchan/modes/state_manager.cpp").read_text()


def method(source, name):
    start = source.index("void StateManager::" + name + "(")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class SecuritySweepTest(unittest.TestCase):
    def test_no_dedicated_task_or_semaphore(self):
        self.assertNotIn("xTaskCreate", CPP)
        self.assertNotIn("xSemaphore", CPP)
        self.assertIn("updateSecuritySweep(now);", method(CPP, "_update"))

    def test_chat_micro_motion_honours_cooperative_lock(self):
        for name in ("speaking.h", "thinking.h"):
            src = (ROOT / "main/stackchan/modifiers" / name).read_text()
            self.assertIn("motion.isModifyLocked() || motion.isMoving()", src, name)

    def test_real_sweep_methods(self):
        legs_start = CPP.index("namespace {\n// Security sweep legs.")
        legs_end = CPP.index("}  // namespace", legs_start) + len("}  // namespace")
        implementation = "\n".join(
            [CPP[legs_start:legs_end]]
            + [method(CPP, n) for n in ("onEnterSecurity", "onExitSecurity",
                                         "startSecurityLeg", "updateSecuritySweep")])
        with tempfile.TemporaryDirectory(prefix="dotty-security-test-") as directory:
            temp = Path(directory)
            generated = temp / "test.cc"
            generated.write_text((ROOT / "tests/security_sweep_platform_stub.h").read_text()
                                 + implementation
                                 + (ROOT / "tests/security_sweep_test.cpp").read_text())
            binary = temp / "security-test"
            subprocess.run(["g++", "-std=c++17", str(generated), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
