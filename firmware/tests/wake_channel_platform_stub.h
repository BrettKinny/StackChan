// AI-assisted by GPT-6. Fake transport/audio boundaries; actual wake methods follow.
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
enum DeviceState { kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateSpeaking,
 kDeviceStateListening, kDeviceStateActivating };
enum { kAbortReasonWakeWordDetected, kAbortReasonNone, kListeningModeAutoStop };
#define ESP_LOGI(...) ((void)0)
namespace Lang { namespace Sounds { static const int OGG_POPUP=0; } }
namespace stackchan { struct SoundLocalizer {
 struct Direction {const char* direction="left"; double balance=0,energy=0;};
 static SoundLocalizer& Instance(){static SoundLocalizer s;return s;}
 Direction GetRecentDirection(int){return {};}
}; }
struct Protocol {
 bool opened=true; int opens=0,starts=0; std::function<void()> during_open;
 bool IsAudioChannelOpened(){return opened;}
 bool OpenAudioChannel(){++opens;opened=true;if(during_open)during_open();return true;}
 void CloseAudioChannel(){opened=false;}
 void SendStartListening(int){++starts;}
};
struct Audio {
 int encoded=0; bool wake_enabled=false;
 std::string GetLastWakeWord(){return "Hi ESP";}
 void EncodeWakeWord(){++encoded;} bool PopPacketFromSendQueue(){return false;}
 void ResetDecoder(){} void PlaySound(int){} void EnableWakeWordDetection(bool on){wake_enabled=on;}
};
class Application { public:
 Protocol p; Protocol* protocol_=&p; Audio audio_service_;
 DeviceState state=kDeviceStateIdle; bool play_popup_on_listening_=false;
 bool privacy_gate_=false;
 std::vector<std::function<void()>> scheduled;
 DeviceState GetDeviceState(){return state;}
 void SetDeviceState(DeviceState s){state=s;}
 void Schedule(std::function<void()> fn){scheduled.push_back(fn);}
 void Drain(){while(!scheduled.empty()){auto q=std::move(scheduled);scheduled.clear();for(auto& fn:q)fn();}}
 void SendEvent(const std::string&,const std::string&){}
 int GetDefaultListeningMode(){return kListeningModeAutoStop;}
 void SetListeningMode(int){state=kDeviceStateListening;++p.starts;}
 void AbortSpeaking(int){}
 void HandleWakeWordDetectedEvent(); void WakeWordInvoke(const std::string&);
 void ContinueWakeWordInvoke(const std::string&);
};
