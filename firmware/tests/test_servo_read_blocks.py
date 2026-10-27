"""AI-assisted (Claude Opus 5.5): compile the real SCSerial::readSCS against a fake UART.

2026-10-27 (#193): the stackchan task held CPU 1 for minutes polling a servo
that didn't answer. readSCS slept with vTaskDelay(1 / portTICK_PERIOD_MS),
which is vTaskDelay(0) at CONFIG_FREERTOS_HZ=100, so it spun. This test
extracts readSCS from SCSerial.cpp and checks that a silent servo costs a
few blocking UART waits rather than a spin, and that replies still read
correctly. It does not model the UART driver; the bench check (no task_wdt
"IDLE1 (CPU 1)" warnings in a long idle serial log) remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "main/hal/drivers/SCServo_lib/src/SCSerial.cpp").read_text()

STUB = """
#include <cassert>
#include <cstdint>
#include <cstring>
typedef uint32_t TickType_t;
#define UART_NUM_MAX 3
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10))   // CONFIG_FREERTOS_HZ=100
#define portTICK_PERIOD_MS 10
static int64_t now_us = 0;
static int reads = 0, delays = 0;
static unsigned char pending[16]; static int pending_len = 0, pending_pos = 0;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(TickType_t ticks) { delays++; now_us += ticks * 10000; }
int uart_get_buffered_data_len(int, size_t* n) { *n = pending_len - pending_pos; return 0; }
int uart_read_bytes(int, unsigned char* out, int, TickType_t wait) {
    reads++;
    if (pending_pos < pending_len) { *out = pending[pending_pos++]; now_us += 100; return 1; }
    now_us += (int64_t)wait * 10000;   // blocked for the whole wait
    return 0;
}
struct SCSerial {
    int uart_num = 1;
    unsigned long int IOTimeOut = 100;
    int readSCS(unsigned char* nDat, int nLen);
};
"""

TEST = """
int main() {
    SCSerial bus;
    unsigned char buf[8];
    // Silent servo: gives up after the 100 ms timeout without spinning.
    assert(bus.readSCS(buf, 6) == 0);
    assert(reads <= 3 && delays == 0);
    assert(now_us >= 100000);
    // Answering servo: every byte read in order.
    reads = 0; now_us = 0;
    const unsigned char reply[] = {0xFF, 0xFF, 1, 3, 0, 0xFB};
    memcpy(pending, reply, 6); pending_len = 6; pending_pos = 0;
    assert(bus.readSCS(buf, 6) == 6);
    assert(memcmp(buf, reply, 6) == 0);
    assert(reads == 6);
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


class ServoReadBlocksTest(unittest.TestCase):
    def test_real_read_scs(self):
        body = method(CPP, "int SCSerial::readSCS(")
        with tempfile.TemporaryDirectory(prefix="dotty-servo-read-test-") as directory:
            temp = Path(directory)
            generated = temp / "test.cc"
            generated.write_text(STUB + body + TEST)
            binary = temp / "servo-read-test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Werror", "-Wno-unused-variable",
                            str(generated), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
