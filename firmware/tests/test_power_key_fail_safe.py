"""Host-side source contract for fail-safe PMIC power-key polling."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
BOARD = (ROOT / "main/hal/board/stackchan.cc").read_text()


class TestPowerKeyFailSafe(unittest.TestCase):
    def test_failed_status_read_cannot_look_like_a_long_press(self):
        handler = BOARD.split("bool ConsumePekLongPress()", 1)[1].split(
            "bool IsExternalPowerConnected()", 1
        )[0]

        self.assertIn("uint8_t status = 0;", handler)
        self.assertIn("ReadRegs(0x49, &status, 1);", handler)
        self.assertNotIn("ReadReg(0x49)", handler)


if __name__ == "__main__":
    unittest.main()
