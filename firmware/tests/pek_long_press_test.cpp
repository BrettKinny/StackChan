
private:
    bool pek_pending_ = false;
};

// AI-assisted by Claude Opus 5.5. Drives the real PEK poll logic one 200 ms
// poll at a time against a scripted I2C bus.
static const FakeRead FAIL{ESP_ERR_TIMEOUT, 0};
static FakeRead ok(uint8_t v) { return FakeRead{ESP_OK, v}; }

// Runs one poll per scripted read; returns the 1-based polls that reported
// a press (each one would power the robot off).
static std::vector<int> run(std::vector<FakeRead> script)
{
    bus = FakeBus{};
    bus.script = script;
    FakePmic pmic;
    std::vector<int> pressed;
    for (size_t i = 0; i < script.size(); ++i) {
        if (pmic.ConsumePekLongPress()) pressed.push_back(static_cast<int>(i) + 1);
    }
    return pressed;
}

using P = std::vector<int>;

static bool cleared_once() { return bus.writes.size() == 1 && bus.writes[0].first == 0x49 && bus.writes[0].second == 0x04; }

int main()
{
    int failures = 0;
    auto check = [&](bool pass, const char* label) {
        std::printf("%s: %s\n", label, pass ? "PASS" : "FAIL");
        failures += !pass;
    };

    check(run(std::vector<FakeRead>(20, FAIL)).empty() && bus.writes.empty(),
          "a burst of failed reads never powers off or clears the flag");
    check(run({FAIL, ok(0x00), FAIL, FAIL, ok(0x00)}).empty(), "failed reads around idle polls never press");
    check(run({ok(0x04), ok(0x04)}) == P{2} && cleared_once(),
          "real long-press confirms on the second good poll and is cleared (W1C)");
    check(run({ok(0x07), FAIL, FAIL, ok(0x05)}) == P{4} && cleared_once(),
          "failed reads between two good bit-set reads delay but do not cancel a real press");
    check(run({ok(0x04), ok(0x00), ok(0x04), ok(0x00)}).empty() && bus.writes.empty(),
          "one good bit-set read not repeated on the next good read is ignored");
    check(run({ok(0xFB), ok(0xFB), ok(0xFB)}).empty(), "other INTSTS2 bits never count as a press");
    check(run({ok(0x04), ok(0x04), ok(0x04)}) == P{2}, "reports once, then needs a fresh confirmation");
    check(run({ok(0x04), ok(0x04), ok(0x04), ok(0x04)}) == P{2, 4},
          "a bit still latched after a failed W1C is re-confirmed, not acted on at once");
    return failures ? 1 : 0;
}
