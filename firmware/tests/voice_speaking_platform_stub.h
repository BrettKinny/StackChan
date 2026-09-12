// AI-assisted by GPT-6: hardware boundaries for real display/state code.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
static uint32_t now_ms=1000;
struct Hal { uint32_t millis(){return now_ms;} };
static Hal& GetHAL(){static Hal hal;return hal;}
namespace mclog { template<class... A> void tagInfo(A...){} }
static const char* _tag="test";
struct Application {
 std::vector<std::string> events;
 static Application& GetInstance(){static Application app;return app;}
 void SendEvent(const std::string& name,const std::string& data){events.push_back(name+data);}
};
namespace stackchan {
enum class State {IDLE,TALK,STORY_TIME,SECURITY,SLEEP,DANCE};
struct FakeMotion {bool isMoving(){return false;} void setTorqueEnabled(bool){} };
struct Modifiable {FakeMotion& motion(){static FakeMotion motion;return motion;} };
class StateManager {
public:
 static constexpr const char* kName="state_manager";
 static constexpr uint32_t kSleepTorqueReleaseTimeoutMs=3000,kReassertIntervalMs=200;
 State _state=State::IDLE;
 uint32_t _state_change_ms=0,_last_assert_ms=0,_last_face_lost_ms=0;
 bool _listening=false,_sleep_torque_release_pending=false,_face_detected=false;
 enum class FaceState {Off}; FaceState _face_state=FaceState::Off;
 int paints=0;
 static const char* stateName(State);
 void setState(State);void setListening(bool);void onVoiceListening();void onVoiceSpeaking();
 void onVoiceStandby();void onFaceLost();void emitStateChanged();void _update(Modifiable&);
 void writePips(Modifiable&,uint32_t){++paints;}
 void onExitSecurity(){} void onExitDance(){} void applyIdleProfile(){}
 void onEnterSleep(){} void onExitSleep(){} void onEnterSecurity(){} void onEnterDance(){}
};
}
struct FakeStackChan : stackchan::Modifiable {
 stackchan::StateManager manager;
 void* getModifierByName(const char*){return &manager;}
 void removeModifier(int){}
 template<class T> int addModifier(std::unique_ptr<T>){return 1;}
};
static FakeStackChan& GetStackChan(){static FakeStackChan sc;return sc;}
static void set_listening_pixel(bool on){GetStackChan().manager.setListening(on);}
static void esp_timer_stop(int){}
struct SpeakingModifier{};
static const int kChatProfileSpeaking=1;
struct Display {
 bool in_listening_status_=false,thinking_led_pending_=false;
 const int* profile_to_push=nullptr;
 int thinking_timer_=0,bubble_clear_timer_=0,thinking_modifier_id_=-1,speaking_modifier_id_=-1;
 void Speaking();
};
