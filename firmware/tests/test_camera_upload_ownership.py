"""AI-assisted by GPT-6: execute the real camera upload with deterministic fakes.

The legacy schedule lets face detection join the encoder before the uploader.
The upload implementation is extracted verbatim; only std::thread is replaced
by a deterministic scheduler, avoiding undefined concurrent-join behaviour on
the host's pthread implementation. No firmware or network operations occur.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CAMERA = ROOT / "main/hal/board/stackchan_camera.cc"
SOURCE_REF = None
if "--source-ref" in sys.argv:
    index = sys.argv.index("--source-ref")
    SOURCE_REF = sys.argv[index + 1]
    del sys.argv[index:index + 2]


class TestCameraUploadOwnership(unittest.TestCase):
    def test_actual_upload_and_cleanup(self):
        source = (subprocess.check_output(
            ["git", "show", f"{SOURCE_REF}:firmware/main/hal/board/stackchan_camera.cc"],
            cwd=ROOT, text=True) if SOURCE_REF else CAMERA.read_text())
        upload = source[source.index("std::string StackChanCamera::StreamJpegToBridge("):]
        detector = source.split("bool StackChanCamera::StreamCaptures()", 1)[1]
        detector_join = detector.split("if (!streaming_on_", 1)[0].lstrip().removeprefix("{")
        with tempfile.TemporaryDirectory(prefix="dotty-camera-test-") as directory:
            temp = Path(directory)
            generated = temp / "upload.cc"
            preamble = (ROOT / "tests/camera_upload_platform_stub.h").read_text()
            generated.write_text(preamble + "\n" + upload.replace("std::thread", "FakeThread")
                                 + "\nvoid StackChanCamera::DetectorJoin() {\n" + detector_join + "}\n"
                                 + (ROOT / "tests/camera_upload_ownership_test.cpp").read_text())
            binary = temp / "camera-test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-function",
                            "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                            str(generated), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
