// AI-assisted by GPT-6 (OpenAI). Fakes surrounding the verbatim upload method.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
static int queues = 0, buffers = 0, closes = 0, status_calls = 0;
static bool fail_open = false, fail_encode = false, fail_write = false, fail_capture_lock = false;
static int http_status = 200;
static bool arbiter_held = false;
static bool require_owned_frame = false;
static std::string sent;
static std::function<void()> detector_hook;
static constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, pdPASS = 1, portMAX_DELAY = 0;
using v4l2_pix_fmt_t = int;
struct JpegChunk { uint8_t* data; size_t len; };
using QueueHandle_t = std::deque<JpegChunk>*;
static QueueHandle_t xQueueCreate(int, size_t) { ++queues; return new std::deque<JpegChunk>; }
static int xQueueSend(QueueHandle_t queue, JpegChunk* chunk, int) { queue->push_back(*chunk); return 1; }
static int xQueueReceive(QueueHandle_t queue, JpegChunk* chunk, int) {
    if (queue->empty()) throw std::runtime_error("would block forever");
    *chunk = queue->front(); queue->pop_front(); return 1;
}
static void vQueueDelete(QueueHandle_t queue) { --queues; delete queue; }
static void* heap_caps_aligned_alloc(int, size_t size, int) { ++buffers; return std::malloc(size); }
static void heap_caps_free(void* pointer) { if (pointer) { --buffers; std::free(pointer); } }
static size_t uxTaskGetStackHighWaterMark(void*) { return 2000; }
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
class FakeThread {
    bool joinable_ = false;
public:
    FakeThread() = default;
    template<class F> explicit FakeThread(F work) { work(); joinable_ = true; }
    FakeThread& operator=(FakeThread&& other) {
        joinable_ = other.joinable_; other.joinable_ = false;
        if (detector_hook) detector_hook();
        return *this;
    }
    bool joinable() { return joinable_; }
    void join() {
        if (!joinable_) throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        joinable_ = false;
    }
};
namespace stackchan {
class CameraArbiter {
public:
    static CameraArbiter& getInstance() { static CameraArbiter instance; return instance; }
    bool acquireForCapture(unsigned) { assert(!arbiter_held); arbiter_held = !fail_capture_lock; return arbiter_held; }
    void releaseForCapture() { assert(arbiter_held); arbiter_held = false; }
};
}
static bool image_to_jpeg(uint8_t*, size_t, uint16_t, uint16_t, int, int, uint8_t** out, size_t* size) {
    if (require_owned_frame) assert(arbiter_held);
    *out = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, 4, 0));
    std::memcpy(*out, "JPEG", 4); *size = 4;
    return !fail_encode;  // Failure may leave a buffer: caller must still own it.
}
template<class Callback> static bool image_to_jpeg_cb(uint8_t*, size_t, uint16_t, uint16_t, int, int, Callback callback, QueueHandle_t queue) {
    uint8_t jpeg[] = {'J','P','E','G'};
    callback(queue, 0, jpeg, 4); callback(queue, 1, nullptr, 0); return true;
}
class FakeHttp {
public:
    ~FakeHttp() { Close(); }
    void SetHeader(const std::string&, const std::string&) {}
    bool Open(const std::string&, const std::string&) { if (require_owned_frame) assert(!arbiter_held); return !fail_open; }
    int Write(const char* data, size_t size) {
        if (require_owned_frame) assert(!arbiter_held);
        if (fail_write) return -1;
        sent.append(data, size); return static_cast<int>(size);
    }
    int GetStatusCode() { ++status_calls; return http_status; }
    int GetLastError() { return 0; }
    std::string ReadAll() { return "ok"; }
    void Close() { ++closes; }
};
class FakeNetwork { public: std::unique_ptr<FakeHttp> CreateHttp(int) { return std::make_unique<FakeHttp>(); } };
class Board {
public:
    static Board& GetInstance() { static Board board; return board; }
    FakeNetwork* GetNetwork() { static FakeNetwork network; return &network; }
    std::string GetUuid() { return "test-client"; }
};
class SystemInfo { public: static std::string GetMacAddress() { return "test-device"; } };
class StackChanCamera {
public:
    struct { uint8_t* data = nullptr; size_t len = 16; uint16_t width = 2, height = 2; int format = 1; } frame_;
    FakeThread encoder_thread_;
    void DetectorJoin();
    std::string StreamJpegToBridge(const std::string&, const std::string&, const std::vector<std::pair<std::string,std::string>>&);
};
