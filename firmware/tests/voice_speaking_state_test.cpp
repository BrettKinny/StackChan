// AI-assisted by GPT-6. No real timers, LEDs, audio or servo access.
int main() {
 using stackchan::State;
 int failures=0;
 auto check=[&](bool ok,const char* name){std::printf("%s: %s\n",name,ok?"PASS":"FAIL");failures+=!ok;};
 auto& sc=GetStackChan();auto& sm=sc.manager;auto& events=Application::GetInstance().events;
 Display display;display.Speaking();
 check(sm._state==State::TALK && !sm._listening && events==std::vector<std::string>{"state_changed{\"state\":\"talk\"}"},
       "first speech claims Talk without lighting listening LED");
 for(now_ms=1200;now_ms<=121000;now_ms+=200)sm._update(sc);
 check(sm._state==State::TALK && !sm._listening && sm.paints>=600,
       "two-minute speech survives repeated mode ticks");
 sm.setListening(true);sm.onVoiceListening();
 check(sm._state==State::TALK && sm._listening,"auto-stop enters listening while retaining Talk");
 display.Speaking();check(sm._state==State::TALK && !sm._listening,"next speech turns listening LED off");
 sm.setListening(false);sm.onVoiceStandby();
 check(sm._state==State::IDLE && !sm._listening && events.back()=="state_changed{\"state\":\"idle\"}",
       "standby recovers Idle and publishes state");
 for(State sticky:{State::STORY_TIME,State::SECURITY,State::SLEEP,State::DANCE}) {
  sm._state=sticky;events.clear();display.Speaking();sm.onVoiceListening();sm.onVoiceStandby();
  check(sm._state==sticky && !sm._listening && events.empty(),"sticky/privacy mode unchanged by voice edges");
 }
 sm._state=State::TALK;sm.onFaceLost();
 check(sm._state==State::IDLE,"existing face-lost exit remains explicit");
 return failures;
}
