# Dotty managed-component patch

`patch_esp_ml307.cmake` runs after IDF resolves dependencies. It copies only
`esp_tcp.cc` into the build directory, applies the committed patch with zero
fuzz, and replaces that one source on the managed component target. Original
managed files and their checksums remain unchanged. A patch mismatch or missing
source aborts configuration rather than silently building unpatched firmware.

The patch makes failed TCP receive-task allocation fail the connection with
`ENOMEM`, closes its socket, and avoids waiting for a nonexistent receiver.
It records free and largest internal heap blocks to diagnose memory pressure.
An outgoing socket can otherwise upload a photo successfully while no task
exists to receive the HTTP response. This mechanism is reproduced by injected
allocation failure; its role in a particular physical failure still requires
device evidence.

Run `python firmware/tests/test_esp_tcp_allocation.py` from the repository root.
The test compiles the real patched source with host FreeRTOS/socket fakes and
checks failed allocation, socket cleanup, destructor behaviour, successful
retry, and normal send/disconnect. Add `--unpatched` to demonstrate the upstream
failure. It needs the fetched managed component, a host C++ compiler, and
`patch`. It does not simulate ESP-IDF scheduling, heap fragmentation, or Wi-Fi;
the firmware build and physical A/V tests remain required.

AI-assisted implementation and documentation: GPT-6 (OpenAI).

## Camera ownership regression

`python firmware/tests/test_camera_upload_ownership.py` compiles the actual
camera upload method with fake camera/HTTP/FreeRTOS boundaries. Add
`--source-ref ed4161e` to reproduce the earlier double-join and outstanding
queue: the controlled scheduler completes encoding, lets the face detector's
actual join prelude join first, then resumes the uploader's second join.
This deliberately models the observed IDF `EINVAL` result without relying on
undefined concurrent joins in the host C++ runtime.

The fix encodes into one owned PSRAM JPEG under the camera arbiter and releases
the arbiter before HTTP operations. It needs neither a shared encoder thread
nor a queue. Tests assert ownership release after encoding, connection, write,
HTTP-status and arbiter failures, and successful retry. Physical warm-camera
repetition and the main-task stack watermark remain necessary acceptance checks;
this does not remove synchronous HTTP timeout stalls.
