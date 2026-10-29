"""AI-authored by Claude Opus 5.5 (Anthropic) for #200: WebSocket teardown race.

Compiles the real esp-ml307 web_socket.cc + esp_tcp.cc (each with its Dotty
patch) on host threads under AddressSanitizer and replays the wake-from-sleep
race (close frame on the receive task while another task replaces the socket).
Requires fetched IDF managed components (or ESP_ML307_DIR), g++ with ASan, and
patch. No device/network use. --unpatched leaves web_socket.cc as upstream to
demonstrate the heap-use-after-free.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
COMPONENT = Path(os.environ.get("ESP_ML307_DIR", ROOT / "managed_components/78__esp-ml307"))
UNPATCHED = "--unpatched" in sys.argv
if UNPATCHED:
    sys.argv.remove("--unpatched")


def apply(temp, patch):
    subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "--input", str(ROOT / "patches" / patch)],
                   cwd=temp, check=True, capture_output=True)


class TestWebSocketTeardown(unittest.TestCase):
    def test_close_frame_racing_socket_replacement(self):
        self.assertTrue((COMPONENT / "src/web_socket.cc").is_file(), "Fetch IDF managed dependencies first")
        with tempfile.TemporaryDirectory(prefix="dotty-ws-test-") as directory:
            temp = Path(directory)
            for rel in ("src/esp/esp_tcp.cc", "src/web_socket.cc"):
                (temp / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(COMPONENT / rel, temp / rel)
            apply(temp, "esp-ml307-receive-task.patch")
            if not UNPATCHED:
                apply(temp, "esp-ml307-websocket-teardown.patch")
            include = temp / "include"
            for name in ("freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/task.h",
                         "esp_log.h", "esp_heap_caps.h", "esp_pthread.h"):
                header = include / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "websocket_teardown_platform_stub.h"\n')
            binary = temp / "ws-test"
            subprocess.run(["g++", "-std=c++17", "-g", "-O0", "-pthread", "-fsanitize=address",
                            "-fno-omit-frame-pointer",
                            "-I", str(include), "-I", str(TESTS), "-I", str(COMPONENT / "include"),
                            "-I", str(COMPONENT / "src/esp"),
                            str(temp / "src/esp/esp_tcp.cc"), str(temp / "src/web_socket.cc"),
                            str(TESTS / "websocket_teardown_test.cpp"), "-o", str(binary),
                            *[f"-Wl,--wrap={name}" for name in
                              ("socket", "connect", "close", "send", "recv", "gethostbyname")]],
                           check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0")
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("websocket teardown: ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
