"""AI-assisted (Claude Opus 5.5): compile the real power-save sync in GetBatteryLevel.

2026-10-27 (#194): on USB power the robot powered itself off 600 s after boot
whenever the voice server was down. The PowerSaveTimer starts enabled and
GetBatteryLevel() only re-synced it on a discharging edge from false, so it
was never disabled while charging. This test extracts GetBatteryLevel() from
stackchan.cc and drives it against a fake PMIC and timer. It does not model
the AXP2101; the bench test (leave the robot on USB with the server stopped
for 11 minutes and confirm it stays on) remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = (ROOT / "main/hal/board/stackchan.cc").read_text()

STUB = """
#include <cassert>
struct FakePmic {
    bool discharging = false;
    bool IsCharging() { return !discharging; }
    bool IsDischarging() { return discharging; }
    int GetBatteryLevel() { return 80; }
};
struct FakeTimer {
    bool enabled = true;  // InitializePowerSaveTimer() enables it at boot
    void SetEnabled(bool on) { enabled = on; }
};
struct Board {
    FakePmic* pmic_;
    FakeTimer* power_save_timer_;
    bool power_save_synced_ = false;
    bool last_discharging_  = false;
"""

TEST = """
};
int main() {
    FakePmic pmic; FakeTimer timer; Board board{&pmic, &timer};
    int level; bool charging, discharging;
    // Booted on USB: the first reading must turn the shutdown timer off.
    board.GetBatteryLevel(level, charging, discharging);
    assert(!timer.enabled);
    // Unplugged: back on, so the battery is still protected.
    pmic.discharging = true;
    board.GetBatteryLevel(level, charging, discharging);
    assert(timer.enabled);
    // Plugged in again: off.
    pmic.discharging = false;
    board.GetBatteryLevel(level, charging, discharging);
    assert(!timer.enabled);
    // Booted on battery: stays on from the first reading.
    FakePmic battery; battery.discharging = true; FakeTimer timer2; Board board2{&battery, &timer2};
    board2.GetBatteryLevel(level, charging, discharging);
    assert(timer2.enabled);
    return 0;
}
"""


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PowerSaveOnUsbTest(unittest.TestCase):
    def test_real_power_save_sync(self):
        body = method(CC, "virtual bool GetBatteryLevel(").replace("virtual ", "").replace(" override", "")
        with tempfile.TemporaryDirectory(prefix="dotty-power-save-test-") as directory:
            temp = Path(directory)
            generated = temp / "test.cc"
            generated.write_text(STUB + body + TEST)
            binary = temp / "power-save-test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Werror", str(generated), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
