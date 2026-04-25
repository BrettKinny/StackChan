/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
//
// Parental gate for sensitive face-recognition operations (enroll / forget).
// Layer 4 of the Dotty/StackChan stack — see firmware/main/stackchan/face/PRIVACY.md
// for the threat model and retention policy.
//
// The gate is a single-shot, time-bounded unlock token. A successful unlock
// (PIN or long-press) flips the gate "open" for kUnlockWindowMs milliseconds;
// after the next sensitive operation OR window expiry, the gate snaps shut
// again. This keeps a dropped MCP session, an overheard PIN, or a stale shell
// from being able to enroll/forget on its own.
//
// DORMANT (2026-04-25)
// --------------------
// The Phase B plan defers the parental gate for the v1 family-only deployment;
// the new server-side face MCP tools (self.camera.face_*) call the bridge
// directly without an unlock step. This scaffold is retained so the gate can
// be re-enabled before any wider deployment (tracked in tasks.md). It is not
// currently referenced by any MCP tool.
//
// SCAFFOLD NOTE
// -------------
// PIN check uses a hardcoded constexpr placeholder. Production must:
//   - move the PIN to menuconfig (CONFIG_DOTTY_PARENTAL_PIN) or NVS-only
//     storage with hash-based comparison;
//   - prefer the long-press path (head-pet hold ~5 s) as the primary path
//     so kids can't snoop a PIN over the user's shoulder;
//   - rate-limit failed attempts (lockout after N tries within window).
//

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace stackchan {

class ParentalGate {
public:
    // Window during which a successful unlock remains valid. Sensitive
    // ops (enroll / forget) consume the unlock; window expiry also clears it.
    static constexpr uint32_t kUnlockWindowMs = 30 * 1000;  // 30 s

    // Dev placeholder — REPLACE before production. Tracked in PRIVACY.md.
    // Anyone reading the binary can see this; the long-press path is the
    // only credentialless path and should be preferred once wired.
    static constexpr const char* kEnrollPIN = "1234";

    // PIN unlock. Returns true if `pin` matches kEnrollPIN; sets the
    // single-shot token. Empty / oversized inputs are rejected to keep
    // this resilient to garbage MCP arguments.
    static bool tryUnlockByPIN(const std::string& pin);

    // Long-press unlock. Stub: returns true and sets the token if the
    // global flag has been raised by the head-pet long-press detector.
    // TODO(layer4-followup): wire to the actual head-pet hold detector
    // (see hal_imu / capacitive scalp sensor — not yet routed). For now
    // there is no path that sets the flag, so this always returns false
    // unless `setLongPressFlag(true)` has been called from a test hook.
    static bool tryUnlockByLongPress();

    // Test/integration hook for raising the long-press flag without the
    // real detector wired. NOT exposed via MCP. The flag self-clears when
    // tryUnlockByLongPress consumes it.
    static void setLongPressFlag(bool armed);

    // True iff a successful unlock is still within the window AND has not
    // yet been consumed. Sensitive ops should call consume() after passing.
    static bool isUnlocked();

    // Drop the unlock immediately. Called by any sensitive op after a
    // successful guarded action (single-shot semantics).
    static void consume();

private:
    // Monotonic ms timestamp of the last successful unlock; 0 = never.
    static std::atomic<uint32_t> _unlocked_at_ms;
    static std::atomic<bool>     _long_press_flag;
};

}  // namespace stackchan
