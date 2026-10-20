// AI-assisted by Claude Opus 5.5. Fake I2C boundary for the real
// Pmic::TryReadReg()/ConsumePekLongPress() methods, which are spliced into
// the FakePmic class below by test_pek_long_press.py.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define TAG "pek-test"
#define ESP_LOGI(tag, fmt, ...) std::printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
typedef void* i2c_master_dev_handle_t;

struct FakeRead { esp_err_t err; uint8_t value; };

struct FakeBus {
    std::vector<FakeRead> script;  // one entry per read, in order
    size_t next = 0;
    std::vector<std::pair<uint8_t, uint8_t>> writes;
} bus;

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t* reg, size_t,
                                      uint8_t* out, size_t, int)
{
    if (*reg != 0x49 || bus.next >= bus.script.size()) {
        std::printf("unexpected read reg=0x%02X\n", *reg);
        return ESP_ERR_TIMEOUT;
    }
    const FakeRead r = bus.script[bus.next++];
    // On failure the buffer is left looking like a "0xFF" read, which is what
    // tripped the old code: the fix must ignore it.
    *out = r.err == ESP_OK ? r.value : 0xFF;
    return r.err;
}

class FakePmic {
public:
    i2c_master_dev_handle_t i2c_device_ = nullptr;
    void WriteReg(uint8_t reg, uint8_t value) { bus.writes.push_back({reg, value}); }
