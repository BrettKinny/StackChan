"""AI-assisted (Claude Opus 5.5): compile the real CoreS3AudioCodec enable paths against a fake codec.

2026-10-23 (#196): one failed I2C read while the amp powered up made
esp_codec_dev_open() fail in EnableOutput(), and ESP_ERROR_CHECK rebooted the
robot. This test extracts OpenDevice/EnableOutput/EnableInput/Read from
cores3_audio_codec.cc and drives them against a fake esp_codec_dev that, like
the real one, leaves the device marked open after a failed open. It does not
model the AW88298 or the I2C bus; a bench check (Dotty still speaks normally)
remains required.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = (ROOT / "main/hal/board/cores3_audio_codec.cc").read_text()

STUB = """
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#define TAG "test"
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_ERROR_CHECK(x) do { if ((x) != 0) abort(); } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_NOT_SUPPORT (-3)
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(ch) ((uint16_t)1 << (ch))
#define pdMS_TO_TICKS(ms) ((ms) / 10)
static int64_t now_us = 0;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(int ticks) { now_us += ticks * 10000; }
struct esp_codec_dev_sample_info_t {
    uint8_t bits_per_sample; uint8_t channel; uint16_t channel_mask; uint32_t sample_rate; int mclk_multiple;
};
struct FakeDev {
    int fail_opens = 0;      // how many opens fail before one succeeds
    int opens = 0, closes = 0;
    bool marked_open = false, codec_enabled = false;
};
typedef FakeDev* esp_codec_dev_handle_t;
int esp_codec_dev_open(esp_codec_dev_handle_t d, esp_codec_dev_sample_info_t*) {
    d->opens++;
    if (d->marked_open) return ESP_CODEC_DEV_OK;   // real driver: "already open", codec untouched
    d->marked_open = true;                          // set before the codec I2C writes
    if (d->fail_opens > 0) { d->fail_opens--; return ESP_CODEC_DEV_NOT_SUPPORT; }
    d->codec_enabled = true;
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_close(esp_codec_dev_handle_t d) {
    d->closes++; d->marked_open = false; d->codec_enabled = false; return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int) { return 0; }
int esp_codec_dev_set_in_channel_gain(esp_codec_dev_handle_t, uint16_t, int) { return 0; }
int esp_codec_dev_read(esp_codec_dev_handle_t, void* dest, int bytes) { memset(dest, 0x55, bytes); return 0; }
struct AudioCodec {
    bool input_enabled_ = false, output_enabled_ = false;
    int output_volume_ = 70;
    void EnableInput(bool on) { input_enabled_ = on; }
    void EnableOutput(bool on) { output_enabled_ = on; }
};
struct CoreS3AudioCodec : AudioCodec {
    static constexpr int kOpenAttempts = 3;
    static constexpr int kOpenRetryDelayMs = 20;
    static constexpr int kOpenBackoffMs = 500;
    int64_t output_retry_after_us_ = 0, input_retry_after_us_ = 0;
    esp_codec_dev_handle_t output_dev_, input_dev_;
    bool input_reference_ = true;
    int output_sample_rate_ = 24000, input_gain_ = 30;
    bool OpenDevice(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t*, int64_t&, const char*);
    void EnableInput(bool);
    void EnableOutput(bool);
    int Read(int16_t*, int);
};
"""

TEST = """
int main() {
    // One failed open (the #196 I2C glitch): retried, and the codec is really enabled.
    FakeDev out1, in1; CoreS3AudioCodec a; a.output_dev_ = &out1; a.input_dev_ = &in1;
    out1.fail_opens = 1;
    a.EnableOutput(true);
    assert(a.output_enabled_ && out1.codec_enabled);
    assert(out1.opens == 2 && out1.closes == 1);

    // The codec never opens: no abort, output stays off, then a back-off.
    FakeDev out2, in2; CoreS3AudioCodec b; b.output_dev_ = &out2; b.input_dev_ = &in2;
    out2.fail_opens = 1000;
    b.EnableOutput(true);
    assert(!b.output_enabled_ && out2.opens == 3 && !out2.marked_open);
    b.EnableOutput(true);                       // inside the back-off: no I2C traffic
    assert(out2.opens == 3);
    now_us += 500 * 1000;
    out2.fail_opens = 0;
    b.EnableOutput(true);                       // after it: tries again and recovers
    assert(b.output_enabled_ && out2.codec_enabled);

    // The mic never opens: reads give silence, not stale samples.
    FakeDev out3, in3; CoreS3AudioCodec c; c.output_dev_ = &out3; c.input_dev_ = &in3;
    in3.fail_opens = 1000;
    c.EnableInput(true);
    assert(!c.input_enabled_);
    int16_t buf[4] = {7, 7, 7, 7};
    assert(c.Read(buf, 4) == 4);
    assert(buf[0] == 0 && buf[3] == 0);
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


class CodecOpenRetryTest(unittest.TestCase):
    def test_real_codec_enable_paths(self):
        bodies = [method(CC, sig) for sig in (
            "bool CoreS3AudioCodec::OpenDevice(",
            "void CoreS3AudioCodec::EnableInput(",
            "void CoreS3AudioCodec::EnableOutput(",
            "int CoreS3AudioCodec::Read(",
        )]
        with tempfile.TemporaryDirectory(prefix="dotty-codec-open-test-") as directory:
            temp = Path(directory)
            generated = temp / "test.cc"
            generated.write_text(STUB + "\n".join(bodies) + TEST)
            binary = temp / "codec-open-test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Werror", str(generated), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
