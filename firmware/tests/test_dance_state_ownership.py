"""Host-side source contract for server-owned dance choreography."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "main/stackchan/modes/state_manager.cpp").read_text()
HEADER = (ROOT / "main/stackchan/modes/state_manager.h").read_text()
IMU = (ROOT / "main/stackchan/modifiers/imu.h").read_text()
MCP = (ROOT / "main/hal/hal_mcp.cpp").read_text()


class TestDanceStateOwnership(unittest.TestCase):
    def test_dance_state_uses_balanced_cooperative_lock(self):
        enter = CPP.split("void StateManager::onEnterDance()", 1)[1].split(
            "void StateManager::onExitDance()", 1
        )[0]
        exit_ = CPP.split("void StateManager::onExitDance()", 1)[1].split(
            "void StateManager::securityPanTaskEntry", 1
        )[0]
        self.assertIn("setModifyLock(true)", enter)
        self.assertIn("setModifyLock(false)", exit_)

    def test_firmware_does_not_start_competing_dance_modifier(self):
        self.assertNotIn("DanceModifier", CPP)
        self.assertNotIn("_dance_modifier_id", HEADER)

    def test_imu_releases_only_its_own_lock_reference(self):
        self.assertIn("_motion_lock_held = true", IMU)
        self.assertIn("if (_motion_lock_held)", IMU)
        self.assertIn("_motion_lock_held = false", IMU)

    def test_mcp_head_keyframes_deliberately_bypass_modifier_lock(self):
        tool = MCP.split('AddTool("self.robot.set_head_angles"', 1)[1].split(
            'mclog::tagInfo(_tag, "add robot.set_led_color tool")', 1
        )[0]
        self.assertIn("motion.pitchServo().moveWithSpeed", tool)
        self.assertIn("motion.yawServo().moveWithSpeed", tool)
        self.assertNotIn("isModifyLocked", tool)


if __name__ == "__main__":
    unittest.main()
