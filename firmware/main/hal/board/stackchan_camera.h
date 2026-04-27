#pragma once
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include <lvgl.h>
#include <thread>
#include <memory>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "jpg/image_to_jpeg.h"
#include "esp_video_init.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

namespace stackchan::camera {
class CameraStreamGuard;
}

class StackChanCamera : public Camera {
private:
    struct FrameBuffer {
        uint8_t* data         = nullptr;
        size_t len            = 0;
        uint16_t width        = 0;
        uint16_t height       = 0;
        v4l2_pix_fmt_t format = 0;
    } frame_;
    v4l2_pix_fmt_t sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    uint16_t sensor_width_  = 0;
    uint16_t sensor_height_ = 0;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    int video_fd_      = -1;
    bool streaming_on_ = false;
    struct MmapBuffer {
        void* start   = nullptr;
        size_t length = 0;
    };
    std::vector<MmapBuffer> mmap_buffers_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

    // Wall-time of the last Capture() call (ms since boot; 0 = never).
    // Wraps every ~49 days (uint32_t millis) — fine.
    uint32_t last_capture_ts_ms_ = 0;

    // Multipart streamer used by Explain() to POST a JPEG to the bridge's
    // /api/vision/explain endpoint (room_view / VLM identification path).
    std::string StreamJpegToBridge(
        const std::string& url, const std::string& token,
        const std::vector<std::pair<std::string, std::string>>& extra_fields);

    // V4L2 stream lifecycle. Reachable only through the friend
    // CameraStreamGuard refcount — the only path that should toggle
    // V4L2 stream state. startStreaming() may block up to 5 s for ISP
    // autoexposure warmup on first call; subsequent calls are cheap and
    // return early when streaming_on_ is already true. stopStreaming()
    // is a no-op when already stopped. Both ignore failure to keep the
    // guard refcount honest — diagnostics surface via streaming_on_.
    bool startStreaming();
    void stopStreaming();
    friend class stackchan::camera::CameraStreamGuard;

public:
    StackChanCamera(const esp_video_init_config_t& config);
    ~StackChanCamera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture() override;
    bool StreamCaptures();

    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);

    // True iff V4L2 streaming is currently asserted (between
    // VIDIOC_STREAMON and VIDIOC_STREAMOFF). Tracks the actual driver
    // state, not the refcount.
    bool isStreaming() const;
    uint32_t lastCaptureTimestampMs() const
    {
        return last_capture_ts_ms_;
    }

    const uint8_t* GetFrameData()
    {
        return frame_.data;
    }
    size_t GetFrameSize()
    {
        return frame_.len;
    }
    int GetFrameWidth()
    {
        return frame_.width;
    }
    int GetFrameHeight()
    {
        return frame_.height;
    }
    int GetFrameFormat()
    {
        return frame_.format;
    }
};

#endif  // ndef CONFIG_IDF_TARGET_ESP32
