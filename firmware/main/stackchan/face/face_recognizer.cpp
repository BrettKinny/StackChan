/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_recognizer.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "FaceRecognizer"

namespace stackchan {

// NVS namespace. NVS namespaces are limited to 15 chars including the NUL,
// so keep this short and stable — changing it mid-deploy strands existing
// enrollments.
//
// ENCRYPTION-AT-REST (planned, post-scaffold):
// ESP32-S3 supports NVS encryption via a 2-key scheme with eFuse-pinned
// keys. Enabling it requires:
//   1) menuconfig: CONFIG_NVS_ENCRYPTION=y
//   2) custom partition table with an `nvs_keys` partition
//   3) flash-time provisioning of the encryption keys
// The scaffold writes plaintext blobs; the on-disk format below is
// deliberately self-describing so a one-time re-encrypt migration is
// straightforward when encryption lands. See PRIVACY.md for the
// deletion-on-reset story (NVS erase will wipe both encrypted and plaintext).
static constexpr const char* kNvsNamespace = "face_recog";

// NVS keys. Limited to 15 chars (NUL included → 14 usable). We use a 2-digit
// slot index so kMaxEnrolled can grow up to 100 without changing the key
// format.
static constexpr const char* kNvsCountKey      = "count";
static constexpr const char* kNvsKeyPrefix     = "enroll_";  // + "NN"
static constexpr size_t      kNvsKeyBufSize    = 16;

// On-disk blob layout for a single enrollment:
//   [u16  name_len]             — bytes that follow as UTF-8 name (no NUL)
//   [u8 * name_len  name_data]
//   [f32 * kEmbeddingDim  embedding] — little-endian native floats
//
// Total = 2 + name_len + 4 * kEmbeddingDim. With kMaxNameLen=32 and
// kEmbeddingDim=512 that's 2 + 32 + 2048 = 2082 B per slot. NVS handles
// blobs up to 4000 B per entry, so we're safely under the per-entry cap.
static constexpr size_t kBlobMaxSize =
    2 + FaceRecognizer::kMaxNameLen + 4 * FaceRecognizer::kEmbeddingDim;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void make_slot_key(size_t slot, char (&buf)[kNvsKeyBufSize])
{
    // Slot index zero-padded to 2 chars. Format guarantees we never overflow
    // the 14-usable-char NVS key budget for slots 0..99.
    std::snprintf(buf, kNvsKeyBufSize, "%s%02u", kNvsKeyPrefix,
                  (unsigned)(slot % 100));
}

// Serialise enrollment to an in-memory blob; returns bytes written, 0 on err.
static size_t encode_blob(const std::string& name,
                          const float (&embedding)[FaceRecognizer::kEmbeddingDim],
                          uint8_t* out, size_t out_cap)
{
    if (!out || out_cap < kBlobMaxSize) return 0;
    if (name.empty() || name.size() > FaceRecognizer::kMaxNameLen) return 0;

    size_t off = 0;
    uint16_t nlen = (uint16_t)name.size();
    std::memcpy(out + off, &nlen, sizeof(nlen)); off += sizeof(nlen);
    std::memcpy(out + off, name.data(), nlen);   off += nlen;
    std::memcpy(out + off, embedding,
                sizeof(float) * FaceRecognizer::kEmbeddingDim);
    off += sizeof(float) * FaceRecognizer::kEmbeddingDim;
    return off;
}

// Reverse of encode_blob. Returns true on success.
static bool decode_blob(const uint8_t* in, size_t in_size,
                        std::string& name_out,
                        float (&embedding_out)[FaceRecognizer::kEmbeddingDim])
{
    if (!in || in_size < 2) return false;
    uint16_t nlen = 0;
    std::memcpy(&nlen, in, sizeof(nlen));
    if (nlen == 0 || nlen > FaceRecognizer::kMaxNameLen) return false;
    const size_t expected = 2 + nlen + 4 * FaceRecognizer::kEmbeddingDim;
    if (in_size < expected) return false;

    name_out.assign(reinterpret_cast<const char*>(in + 2), nlen);
    std::memcpy(embedding_out, in + 2 + nlen,
                sizeof(float) * FaceRecognizer::kEmbeddingDim);
    return true;
}

// ---------------------------------------------------------------------------
// FaceRecognizer
// ---------------------------------------------------------------------------

FaceRecognizer& FaceRecognizer::getInstance()
{
    static FaceRecognizer instance;
    return instance;
}

bool FaceRecognizer::isValidName(const std::string& name)
{
    if (name.empty()) return false;
    if (name.size() > kMaxNameLen) return false;
    // Reject control chars / NUL inside the name. ASCII-only is fine for
    // the scaffold; a future revision can relax this once we have a
    // confirmed UTF-8 source from the bridge.
    for (char c : name) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool FaceRecognizer::init()
{
    std::lock_guard<std::mutex> lock(_mu);
    if (_initialized) return true;

    _enrollments.clear();
    _enrollments.reserve(kMaxEnrolled);

    bool ok = loadAll();
    _initialized = true;  // mark even on failure; recognize() will be safe
    ESP_LOGI(TAG, "init: loaded %u enrolled face(s) from NVS",
             (unsigned)_enrollments.size());
    return ok;
}

std::string FaceRecognizer::recognize(const FaceImage& cropped)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (_enrollments.empty()) {
        // Nothing to compare against. Cheap fast-path; no model invocation.
        return std::string(kUnknown);
    }

    float probe[kEmbeddingDim] = {0};
    if (!computeEmbedding(cropped, probe)) {
        // Embedding compute failed (or not yet wired). Defensive: don't
        // misidentify on a model error — fall through to unknown.
        return std::string(kUnknown);
    }

    // Cosine similarity against every enrolled embedding. Linear scan is
    // fine at kMaxEnrolled=10. If we ever raise the cap, swap for a
    // heap-based top-1.
    float best_sim = -1.0f;
    const Enrollment* best = nullptr;
    for (const auto& e : _enrollments) {
        float s = cosineSim(probe, e.embedding, kEmbeddingDim);
        if (s > best_sim) {
            best_sim = s;
            best = &e;
        }
    }

    if (!best || best_sim < kMatchThreshold) {
        return std::string(kUnknown);
    }
    return best->name;
}

bool FaceRecognizer::enroll(const std::string& name,
                            const FaceImage& cropped,
                            bool parental_gate_passed)
{
    if (!parental_gate_passed) {
        ESP_LOGW(TAG, "enroll rejected: parental gate not passed");
        return false;
    }
    if (!isValidName(name)) {
        ESP_LOGW(TAG, "enroll rejected: invalid name (len=%u)",
                 (unsigned)name.size());
        return false;
    }

    std::lock_guard<std::mutex> lock(_mu);
    if (!_initialized) {
        ESP_LOGW(TAG, "enroll rejected: recognizer not initialized");
        return false;
    }
    if (_enrollments.size() >= kMaxEnrolled) {
        ESP_LOGW(TAG, "enroll rejected: cap reached (%u)", (unsigned)kMaxEnrolled);
        return false;
    }
    for (const auto& e : _enrollments) {
        if (e.name == name) {
            ESP_LOGW(TAG, "enroll rejected: '%s' already enrolled (forget first)",
                     name.c_str());
            return false;
        }
    }

    Enrollment fresh;
    fresh.name = name;
    // Compute the embedding. In the scaffold this is the zero vector and
    // returns false; we still persist so the storage path is exercised
    // and the slot is reserved. The .so integration replaces this branch.
    if (!computeEmbedding(cropped, fresh.embedding)) {
        ESP_LOGW(TAG,
                 "enroll: computeEmbedding stub returned false — "
                 "persisting placeholder embedding for '%s'",
                 name.c_str());
        // Fall through with zeroed embedding. Recognize() will treat this
        // as unknown until the .so backfills real embeddings.
    }

    _enrollments.push_back(std::move(fresh));
    if (!persistAll()) {
        // Roll back the in-memory addition so we don't drift from NVS.
        _enrollments.pop_back();
        ESP_LOGE(TAG, "enroll failed: persistAll error");
        return false;
    }
    ESP_LOGI(TAG, "enrolled '%s' (slot %u/%u)", name.c_str(),
             (unsigned)_enrollments.size(), (unsigned)kMaxEnrolled);
    return true;
}

bool FaceRecognizer::forget(const std::string& name)
{
    if (!isValidName(name)) return false;

    std::lock_guard<std::mutex> lock(_mu);
    if (!_initialized) return false;

    auto it = std::find_if(_enrollments.begin(), _enrollments.end(),
                           [&](const Enrollment& e) { return e.name == name; });
    if (it == _enrollments.end()) {
        ESP_LOGW(TAG, "forget: '%s' not enrolled", name.c_str());
        return false;
    }
    _enrollments.erase(it);
    if (!persistAll()) {
        ESP_LOGE(TAG, "forget: persistAll failed — in-memory and NVS now diverged");
        return false;
    }
    ESP_LOGI(TAG, "forgot '%s' (now %u enrolled)", name.c_str(),
             (unsigned)_enrollments.size());
    return true;
}

bool FaceRecognizer::forgetAll()
{
    std::lock_guard<std::mutex> lock(_mu);
    _enrollments.clear();
    if (!persistAll()) {
        ESP_LOGE(TAG, "forgetAll: persistAll failed");
        return false;
    }
    ESP_LOGI(TAG, "forgot all enrolled faces");
    return true;
}

std::vector<std::string> FaceRecognizer::enrolledNames() const
{
    std::lock_guard<std::mutex> lock(_mu);
    std::vector<std::string> out;
    out.reserve(_enrollments.size());
    for (const auto& e : _enrollments) out.push_back(e.name);
    return out;
}

size_t FaceRecognizer::enrolledCount() const
{
    std::lock_guard<std::mutex> lock(_mu);
    return _enrollments.size();
}

// ---------------------------------------------------------------------------
// Embedding stub
// ---------------------------------------------------------------------------

bool FaceRecognizer::computeEmbedding(const FaceImage& /*cropped*/,
                                      float (&out)[kEmbeddingDim])
{
    // TODO(esp-dl): replace this stub with the real embedding compute.
    //
    // The follow-up commit should:
    //   1) include "human_face_recognition.hpp" (the ESP-DL companion to
    //      the human_face_detect component already pulled in via
    //      idf_component.yml);
    //   2) hold a `static HumanFaceRecognition recognizer;` here, lazy-
    //      initialized like HumanFaceDetect in face_detector.cpp;
    //   3) build a dl::image::img_t from `cropped.data` + bbox (or pre-
    //      cropped frame if bbox_x1 < 0), feed it to recognizer.run(...),
    //      copy the resulting feature vector into `out`;
    //   4) return true on success, false on any model error (caller
    //      treats false as "skip and report unknown").
    //
    // For now: zero-fill so cosineSim is well-defined (returns 0 against
    // any other zero vector → below kMatchThreshold → unknown).
    std::memset(out, 0, sizeof(out));
    return false;
}

float FaceRecognizer::cosineSim(const float* a, const float* b, size_t n)
{
    if (!a || !b || n == 0) return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < n; i++) {
        dot += (double)a[i] * (double)b[i];
        na  += (double)a[i] * (double)a[i];
        nb  += (double)b[i] * (double)b[i];
    }
    if (na == 0.0 || nb == 0.0) return 0.0f;
    double denom = std::sqrt(na) * std::sqrt(nb);
    if (denom == 0.0) return 0.0f;
    double sim = dot / denom;
    if (sim > 1.0)  sim = 1.0;
    if (sim < -1.0) sim = -1.0;
    return (float)sim;
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

bool FaceRecognizer::loadAll()
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot or just-erased — nothing to load.
        ESP_LOGI(TAG, "loadAll: namespace '%s' not found yet", kNvsNamespace);
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "loadAll: nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t count = 0;
    err = nvs_get_u8(h, kNvsCountKey, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "loadAll: get count failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    if (count > kMaxEnrolled) {
        ESP_LOGW(TAG, "loadAll: count %u > cap %u — clamping",
                 (unsigned)count, (unsigned)kMaxEnrolled);
        count = (uint8_t)kMaxEnrolled;
    }

    std::vector<uint8_t> buf(kBlobMaxSize);
    for (uint8_t i = 0; i < count; i++) {
        char key[kNvsKeyBufSize];
        make_slot_key(i, key);

        size_t blob_size = buf.size();
        err = nvs_get_blob(h, key, buf.data(), &blob_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "loadAll: slot %u missing — skipping", (unsigned)i);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "loadAll: slot %u read err %s — skipping",
                     (unsigned)i, esp_err_to_name(err));
            continue;
        }

        Enrollment e;
        if (!decode_blob(buf.data(), blob_size, e.name, e.embedding)) {
            ESP_LOGW(TAG, "loadAll: slot %u decode failed — skipping", (unsigned)i);
            continue;
        }
        if (!isValidName(e.name)) {
            ESP_LOGW(TAG, "loadAll: slot %u invalid name — skipping", (unsigned)i);
            continue;
        }
        _enrollments.push_back(std::move(e));
        if (_enrollments.size() >= kMaxEnrolled) break;
    }

    nvs_close(h);
    return true;
}

bool FaceRecognizer::persistAll()
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persistAll: nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    // Wipe all existing slots first so a `forget` can't leave stale entries
    // behind. NVS erase_all on the namespace handle is the simplest route;
    // we then re-write count + active slots.
    err = nvs_erase_all(h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "persistAll: erase_all failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    err = nvs_set_u8(h, kNvsCountKey, (uint8_t)_enrollments.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persistAll: set count failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    std::vector<uint8_t> buf(kBlobMaxSize);
    for (size_t i = 0; i < _enrollments.size(); i++) {
        const auto& e = _enrollments[i];
        size_t n = encode_blob(e.name, e.embedding, buf.data(), buf.size());
        if (n == 0) {
            ESP_LOGE(TAG, "persistAll: encode slot %u failed", (unsigned)i);
            nvs_close(h);
            return false;
        }
        char key[kNvsKeyBufSize];
        make_slot_key(i, key);
        err = nvs_set_blob(h, key, buf.data(), n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "persistAll: set blob slot %u failed: %s",
                     (unsigned)i, esp_err_to_name(err));
            nvs_close(h);
            return false;
        }
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persistAll: commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace stackchan
