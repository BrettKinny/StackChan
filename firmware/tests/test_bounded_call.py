"""AI-assisted (Claude Opus 5.5): compile the real main/hal/board/bounded_call.h on the host.

#189: esp_video's VIDIOC_DQBUF blocks for ever when the sensor delivers no frame. BoundedCall lets
the caller give up after a deadline, and a late result is picked up by the next Wait(). This does not
exercise esp_video or the sensor; a bench check of take_photo remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

TEST = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include "bounded_call.h"
using namespace std::chrono;
int main() {
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    BoundedCall c([&]() {
        calls++;
        while (!release) std::this_thread::sleep_for(milliseconds(5));
        return 7;
    });
    int rc = -1;
    auto t0 = steady_clock::now();
    assert(!c.Wait(100, &rc));                     // stuck: must time out
    assert(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 500);
    assert(c.Pending());
    assert(!c.Wait(50, &rc));                      // still stuck: adopts the same call
    assert(calls == 1);                            // no second call started
    release = true;                                // late frame arrives
    assert(c.Wait(500, &rc) && rc == 7);           // next Wait picks it up
    assert(!c.Pending());
    release = false;
    assert(!c.Wait(50, &rc));                      // fresh call, stuck again
    assert(calls == 2);
    release = true;
    assert(c.Wait(500, &rc) && rc == 7);
    return 0;
}
'''

class BoundedCallTest(unittest.TestCase):
    def test_times_out_and_resumes(self):
        with tempfile.TemporaryDirectory(prefix='dotty-bounded-') as d:
            src = Path(d) / 't.cc'
            src.write_text(TEST)
            exe = Path(d) / 't'
            subprocess.run(['g++', '-std=c++17', '-g', '-fsanitize=address,undefined', '-pthread',
                            '-I', str(ROOT / 'main/hal/board'), str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

if __name__ == '__main__':
    unittest.main()
