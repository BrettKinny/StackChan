// AI-assisted by GPT-6. Exercise both real call sites and delayed cancellation.
int main() {
 int failures=0;
 auto check=[&](bool pass,const char* label){std::printf("%s: %s\n",label,pass?"PASS":"FAIL");failures+=!pass;};
 for(bool detected:{true,false}) {
  auto invoke=[&](Application& a){if(detected)a.HandleWakeWordDetectedEvent();else a.WakeWordInvoke("Hi ESP");};
  for(bool opened:{true,false}) {
   Application a;a.p.opened=opened;invoke(a);a.Drain();
   check(a.state==kDeviceStateListening && a.p.starts==1 && a.p.opens==!opened,
         opened?"Idle + open channel":"Idle + closed channel");
  }
  Application asleep;asleep.privacy_gate_=true;invoke(asleep);asleep.Drain();
  check(asleep.state==kDeviceStateIdle && asleep.p.starts==0 && asleep.p.opens==0 && asleep.audio_service_.encoded==0,
        "Privacy sleep rejects wake before encoding");
  for(bool sleep:{false,true}) {
   Application stale;stale.p.opened=false;invoke(stale);
   stale.state=sleep?kDeviceStateIdle:kDeviceStateSpeaking;stale.privacy_gate_=sleep;stale.Drain();
   check(stale.p.opens==0 && stale.p.starts==0 && !stale.audio_service_.wake_enabled,
         sleep?"Sleep cancels scheduled wake":"Speaking cancels scheduled wake");
  }
  Application during;during.p.opened=false;
  during.p.during_open=[&](){during.privacy_gate_=true;during.state=kDeviceStateIdle;};
  invoke(during);during.Drain();
  check(during.state==kDeviceStateIdle && during.p.starts==0 && !during.audio_service_.wake_enabled,
        "Sleep during network open cancels listening");
 }
 return failures;
}
