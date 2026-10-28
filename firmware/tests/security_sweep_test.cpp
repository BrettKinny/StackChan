}  // namespace stackchan
// AI-assisted by Claude Opus 5.5. Drives the real sweep methods on the
// caller's (update-task) thread with a fake clock.
int main() {
    int failures = 0;
    auto check = [&](bool pass, const char* label) {
        std::printf("%s: %s\n", label, pass ? "PASS" : "FAIL");
        failures += !pass;
    };
    auto& m = GetStackChan().motion();
    StateManager sm;

    GetHAL().now = 1000;
    sm._state = State::SECURITY;
    sm.onEnterSecurity();
    check(m.lock == 1, "entry takes one motion-lock reference");
    check(m.moves.size() == 1 && m.moves[0].yaw == -500, "first leg commanded synchronously on entry");
    check(GetStackChan().avatar().emotion == avatar::Emotion::Angry, "angry face on entry");
    check(!Application::GetInstance().events.empty()
          && Application::GetInstance().events.back() == "security_pose", "security_pose emitted");

    sm.updateSecuritySweep(4999);
    check(m.moves.size() == 1, "holds leg 1 until its deadline");
    sm.updateSecuritySweep(5000);
    check(m.moves.size() == 2 && m.moves[1].yaw == 500, "advances to leg 2 (+500)");
    sm.updateSecuritySweep(9000);
    check(m.moves.size() == 3 && m.moves[2].yaw == 0, "advances to centre");
    sm.updateSecuritySweep(15999);
    check(m.moves.size() == 3, "centre pause holds 7 s");
    sm.updateSecuritySweep(16000);
    check(m.moves.size() == 4 && m.moves[3].yaw == -500, "cycle repeats from leg 1");

    // Wrap-safe: deadline just past the 32-bit millis() wrap.
    StateManager wrap;
    wrap._state = State::SECURITY;
    wrap._security_sweep_active = true;
    wrap.startSecurityLeg(0, 0xFFFFF000u);  // deadline wraps to 0x00000FA0
    size_t before = m.moves.size();
    wrap.updateSecuritySweep(0xFFFFFF00u);
    check(m.moves.size() == before, "no premature advance across millis wrap");
    wrap.updateSecuritySweep(0x00000FA0u);
    check(m.moves.size() == before + 1, "advances after millis wrap");

    // Exit: sequencer stops, lock released, head homed.
    sm.onExitSecurity();
    sm._state = State::IDLE;
    size_t after_exit = m.moves.size();
    sm.updateSecuritySweep(60000);
    check(m.moves.size() == after_exit, "no sweep moves after exit");
    check(m.lock == 0, "exit releases the lock reference");
    check(m.homes == 1, "exit homes the head");
    check(GetStackChan().avatar().emotion == avatar::Emotion::Neutral, "neutral face on exit");

    // A sweep left active but in another state must not move the head.
    StateManager stale;
    stale._security_sweep_active = true;
    stale._state = State::IDLE;
    size_t stale_before = m.moves.size();
    stale.updateSecuritySweep(1u << 30);
    check(m.moves.size() == stale_before, "inactive outside SECURITY");
    return failures;
}
