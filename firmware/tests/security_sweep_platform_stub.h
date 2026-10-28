// AI-assisted by Claude Opus 5.5. Fake motion/HAL/app boundaries for the
// actual StateManager security-sweep methods, which are appended after this.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct Move { int yaw; int pitch; int speed; std::string tag; };

struct FakeMotion {
    int lock = 0;
    std::vector<Move> moves;
    int homes = 0;
    void setModifyLock(bool on) { lock += on ? 1 : (lock > 0 ? -1 : 0); }
    bool isModifyLocked() { return lock > 0; }
    void moveWithSpeed(int y, int p, int s, const char* t) { moves.push_back({y, p, s, t}); }
    void goHome(int, const char*) { ++homes; }
};

namespace avatar { enum class Emotion { Neutral, Angry }; }
struct FakeAvatar { avatar::Emotion emotion = avatar::Emotion::Neutral;
    void setEmotion(avatar::Emotion e) { emotion = e; } };

struct FakeStackChan {
    FakeMotion m; FakeAvatar a;
    FakeMotion& motion() { return m; }
    bool hasAvatar() { return true; }
    FakeAvatar& avatar() { return a; }
};
static FakeStackChan g_sc;
FakeStackChan& GetStackChan() { return g_sc; }

struct FakeHal { uint32_t now = 0; uint32_t millis() { return now; } };
static FakeHal g_hal;
FakeHal& GetHAL() { return g_hal; }

struct Application {
    std::vector<std::string> events;
    static Application& GetInstance() { static Application a; return a; }
    void SendEvent(const std::string& n, const std::string&) { events.push_back(n); }
};

namespace mclog { template <typename... A> void tagInfo(std::string_view, A&&...) {} }

namespace stackchan {
enum class State { IDLE, TALK, STORY_TIME, SECURITY, SLEEP, DANCE };
static const std::string_view _tag = "StateManager";
class StateManager {
public:
    State _state = State::IDLE;
    bool     _security_sweep_active    = false;
    uint8_t  _security_leg             = 0;
    uint32_t _security_leg_deadline_ms = 0;
    void onEnterSecurity();
    void onExitSecurity();
    void startSecurityLeg(uint8_t leg, uint32_t now);
    void updateSecuritySweep(uint32_t now);
};
}  // namespace stackchan
using namespace stackchan;
namespace stackchan {
