// AI-authored by Claude Opus 5.5 (Anthropic) for #200. Host FreeRTOS/ESP fakes
// with real threads, so the actual EspTcp + WebSocket sources run their receive
// task concurrently with the task that destroys the socket.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

using BaseType_t = int;
using EventBits_t = unsigned;
using TickType_t = unsigned;
using TaskHandle_t = void*;
inline constexpr int pdPASS = 1, pdFALSE = 0, pdTRUE = 1;
inline constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
#define BIT0 0x01u
#define BIT1 0x02u
#define pdMS_TO_TICKS(value) (value)

struct HostEventGroup {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned bits = 0;
};
using EventGroupHandle_t = HostEventGroup*;

inline EventGroupHandle_t xEventGroupCreate() { return new HostEventGroup(); }
inline void vEventGroupDelete(EventGroupHandle_t group) { delete group; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t group, unsigned bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    unsigned old = group->bits;
    group->bits &= ~bits;
    return old;
}
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t group, unsigned bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    group->bits |= bits;
    group->cv.notify_all();
    return group->bits;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, unsigned bits, int clear, int all,
                                       unsigned timeout_ms) {
    std::unique_lock<std::mutex> lock(group->mutex);
    group->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return all ? (group->bits & bits) == bits : (group->bits & bits) != 0;
    });
    unsigned result = group->bits;
    if (clear) group->bits &= ~bits;
    return result;
}

inline thread_local void* host_current_task = reinterpret_cast<void*>(1);  // "main" task
inline BaseType_t xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned,
                              TaskHandle_t* handle) {
    static int next_id = 100;
    void* id = reinterpret_cast<void*>(static_cast<std::intptr_t>(++next_id));
    if (handle) *handle = id;
    std::thread([fn, arg, id] {
        host_current_task = id;
        fn(arg);
    }).detach();
    return pdPASS;
}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return host_current_task; }
inline void vTaskDelete(void*) {}
inline std::size_t heap_caps_get_free_size(unsigned) { return 100000; }
inline std::size_t heap_caps_get_largest_free_block(unsigned) { return 100000; }
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
