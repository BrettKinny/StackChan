/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
//
// FaceRecognizer — Layer 4 of the Dotty/StackChan stack.
//
// On-device face recognition (Option A from tasks.md). Embeddings + names
// stored in NVS namespace `face_recog`. NEVER egresses biometric data; see
// firmware/main/stackchan/face/PRIVACY.md for the retention model.
//
// DORMANT (2026-04-25)
// --------------------
// Compute moved server-side per the Phase B plan
// (~/.claude/plans/i-want-to-design-gentle-charm.md). The bridge owns
// embedding + match via dlib in /root/.zeroclaw/faces.sqlite. The MCP
// tools that previously called into this class (self.robot.face_*) were
// removed; the new server-side tools are self.camera.face_* in
// firmware/main/hal/hal_mcp.cpp. This scaffold is retained for a future
// on-device fallback (LAN-down mode, or a privacy-LED-gated path) and is
// not currently linked into any tool.
//
// SCAFFOLD STATE
// --------------
// This is the structural scaffold. The real ESP-DL `face_recognition.so`
// hookup is a follow-up commit — the relevant integration points are
// marked `// TODO(esp-dl):` inside the .cpp. For now:
//   - recognize() always returns kUnknown (cosine distance against an empty
//     embedding always exceeds the threshold);
//   - enroll() persists name + a zero-filled embedding placeholder so the
//     storage path is exercised;
//   - forget() / enrolledNames() / count slot semantics are fully working.
//
// The interface is shaped so the .so swap-in is a localized change:
// computeEmbedding(crop) goes from "return zero vector" to "run model →
// return float[kEmbeddingDim]". recognize() / enroll() do not change.
//

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace stackchan {

// FaceImage — a thin descriptor of a cropped face frame, decoupled from the
// detection pipeline's internal types so this header doesn't drag in ESP-DL
// includes. The recognizer doesn't own the buffer.
//
// NOTE: in this scaffold the buffer is unused (no embedding model yet).
// Callers may pass a default-constructed FaceImage{} alongside the bbox.
struct FaceImage {
    const uint8_t* data = nullptr;  // raw pixel buffer (crop or full frame)
    int width           = 0;
    int height          = 0;
    int pix_fmt         = 0;        // V4L2_PIX_FMT_* — see face_detector.cpp
    // Optional bbox in source image coords if `data` is not pre-cropped.
    // Negative or out-of-range values mean "use full image".
    int bbox_x1 = -1, bbox_y1 = -1, bbox_x2 = -1, bbox_y2 = -1;
};

class FaceRecognizer {
public:
    static FaceRecognizer& getInstance();

    // Magic string returned by recognize() when no enrolled face matches
    // (or recognition is disabled / not yet wired up).
    static constexpr const char* kUnknown = "unknown";

    // Hard cap on enrolled identities. Matches the ESP-WHO/ESP-DL on-device
    // reference design (~10 enrolled faces is comfortable for a family).
    static constexpr size_t kMaxEnrolled = 10;

    // Embedding dimensionality. ESP-DL face-recognition models typically
    // emit 512-d float vectors; we reserve that here so the storage layout
    // is final and the .so swap-in doesn't force a re-enroll.
    static constexpr size_t kEmbeddingDim = 512;

    // Cosine-similarity threshold for "this is the same person". Tuned for
    // the model that lands in the follow-up commit; may need lowering once
    // we have real numbers from the kids' faces. Range [0..1], higher =
    // stricter.
    static constexpr float kMatchThreshold = 0.55f;

    // Throttle for `face_recognized` event emission. Even when a face is
    // continuously detected we only run recognize() at most once per
    // window — embedding inference is expensive and the bridge only needs
    // the identity occasionally for greeting / personalization.
    static constexpr uint32_t kRecognizeThrottleMs = 2000;

    // Load enrolled faces from NVS into the in-memory cache. Idempotent;
    // safe to call again after enroll/forget. Logs and returns false on
    // NVS errors but the recognizer remains usable in a degraded state
    // (count = 0, recognize → unknown, enroll/forget will retry the open).
    bool init();

    // Identify the face in `cropped` (or in `bbox` of the full frame).
    // Returns the enrolled name or kUnknown. Never throws; caller-safe
    // even if init() failed. The bbox is currently advisory — the .so
    // integration is what consumes it.
    std::string recognize(const FaceImage& cropped);

    // Enroll a new identity. Requires `parental_gate_passed = true`;
    // callers MUST verify ParentalGate::isUnlocked() and ParentalGate::consume()
    // around this call so the single-shot semantics aren't bypassed.
    // Rejects on:
    //   - empty / oversized name (kMaxNameLen);
    //   - name already enrolled (use forget() first);
    //   - cap reached (kMaxEnrolled);
    //   - parental_gate_passed = false.
    bool enroll(const std::string& name,
                const FaceImage& cropped,
                bool parental_gate_passed);

    // Remove a single identity. Requires the parental gate (caller-checked).
    // Returns true if found+removed, false if not enrolled or NVS error.
    bool forget(const std::string& name);

    // Wipe ALL enrolled identities. Requires the parental gate (caller-checked).
    // Used by the "Dotty, forget everyone" reset path. Returns false on
    // NVS error.
    bool forgetAll();

    // Snapshot of enrolled names. Cheap; reads from in-memory cache under
    // lock.
    std::vector<std::string> enrolledNames() const;

    // Count of enrolled identities. Useful for the MCP tool to report
    // "8 of 10 slots used" without dragging the full name list.
    size_t enrolledCount() const;

    // Hard cap on the name string length. Avoids unbounded NVS keys.
    static constexpr size_t kMaxNameLen = 32;

private:
    FaceRecognizer() = default;

    struct Enrollment {
        std::string name;
        // Embedding stored alongside name. In the scaffold this is the
        // all-zero placeholder; the .so integration fills it for real.
        float embedding[kEmbeddingDim] = {0};
    };

    // Compute the embedding for `cropped`. Scaffold: returns a zero vector
    // and `false`. The .so integration replaces this body — see TODO in
    // .cpp. NOT static so it can hold lazily-initialized model state in a
    // future revision.
    bool computeEmbedding(const FaceImage& cropped, float (&out)[kEmbeddingDim]);

    // Cosine similarity in [-1, 1]. Pure function over fixed-length floats.
    static float cosineSim(const float* a, const float* b, size_t n);

    // Persist the in-memory cache to NVS. Returns false on NVS error.
    bool persistAll();

    // Load cache from NVS. Best-effort: missing/garbled entries are skipped
    // with a warning rather than failing the whole init.
    bool loadAll();

    // Reject names that would be problematic as NVS keys (NVS keys cap at
    // 15 chars, but we store the name as part of a blob keyed by index, so
    // we only need printable / non-empty / length-bounded).
    static bool isValidName(const std::string& name);

    mutable std::mutex _mu;
    std::vector<Enrollment> _enrollments;  // length <= kMaxEnrolled
    bool _initialized = false;
    uint32_t _last_recognize_ms = 0;
    std::string _last_identity = kUnknown;
};

}  // namespace stackchan
