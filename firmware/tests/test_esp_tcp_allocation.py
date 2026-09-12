"""AI-assisted by GPT-6 (OpenAI): compile real dependency source, inject task failure.

Requires fetched IDF managed components, g++, and patch. No device/network use.
--unpatched runs the same assertions against upstream to demonstrate the failure.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
COMPONENT = ROOT / "managed_components/78__esp-ml307"
UNPATCHED = "--unpatched" in sys.argv
if UNPATCHED:
    sys.argv.remove("--unpatched")


class TestEspTcpAllocation(unittest.TestCase):
    def test_real_connection_cleanup_retry_and_success(self):
        self.assertTrue((COMPONENT / "src/esp/esp_tcp.cc").is_file(), "Fetch IDF managed dependencies first")
        with tempfile.TemporaryDirectory(prefix="dotty-tcp-test-") as directory:
            temp = Path(directory)
            source = temp / "src/esp/esp_tcp.cc"
            source.parent.mkdir(parents=True)
            shutil.copyfile(COMPONENT / "src/esp/esp_tcp.cc", source)
            if not UNPATCHED:
                subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "--input",
                                str(ROOT / "patches/esp-ml307-receive-task.patch")], cwd=temp, check=True)
            include = temp / "include"
            for name in ("freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/task.h",
                         "esp_log.h", "esp_heap_caps.h"):
                header = include / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "esp_tcp_platform_stub.h"\n')
            binary = temp / "tcp-test"
            subprocess.run(["g++", "-std=c++17", "-g", "-Wall", "-Wextra",
                            "-I", str(include), "-I", str(TESTS), "-I", str(COMPONENT / "include"),
                            "-I", str(COMPONENT / "src/esp"), str(source),
                            str(TESTS / "esp_tcp_failure_test.cpp"), "-o", str(binary),
                            *[f"-Wl,--wrap={name}" for name in ("socket", "connect", "close", "send", "recv", "gethostbyname")]],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
