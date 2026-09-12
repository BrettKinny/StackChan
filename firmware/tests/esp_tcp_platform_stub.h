// AI-assisted by GPT-6 (OpenAI). Host fakes for the real EspTcp implementation.
#pragma once
#include <cstddef>
#include <cstdint>
using BaseType_t = int;
using EventBits_t = unsigned;
using EventGroupHandle_t = void*;
using TaskHandle_t = void*;
inline constexpr int pdPASS = 1, pdFALSE = 0;
inline constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
extern bool task_creation_succeeds;
extern int wait_count, task_create_count;
inline EventGroupHandle_t xEventGroupCreate() { return reinterpret_cast<void*>(1); }
inline void vEventGroupDelete(EventGroupHandle_t) {}
inline void xEventGroupClearBits(EventGroupHandle_t, unsigned) {}
inline void xEventGroupSetBits(EventGroupHandle_t, unsigned) {}
inline unsigned xEventGroupWaitBits(EventGroupHandle_t, unsigned bits, int, int, unsigned) {
    ++wait_count;
    return bits;
}
inline BaseType_t xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t* task) {
    ++task_create_count;
    if (!task_creation_succeeds) return 0;
    *task = reinterpret_cast<void*>(2);
    return pdPASS;
}
inline void vTaskDelete(void*) {}
inline std::size_t heap_caps_get_free_size(unsigned) { return 3000; }
inline std::size_t heap_caps_get_largest_free_block(unsigned) { return 1024; }
#define pdMS_TO_TICKS(value) (value)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
