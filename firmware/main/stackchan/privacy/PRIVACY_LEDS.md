# Privacy LEDs (Layer 1)

Hardware-guaranteed mic / camera capture indicators for Dotty / StackChan.

This module owns two pixels on the right LED ring:

| Pixel       | Global ring index | Right-ring local index | Indicates |
| ----------- | ----------------- | ---------------------- | --------- |
| MIC         | 6                 | 0                      | mic ADC state |
| CAMERA      | 7                 | 1                      | camera frame consumer state |

Indices 8..11 are still owned by the existing chat-state and face-detect ring animations and are NOT touched here.

---

## State table — Mic

| `MicState` | LED color (R,G,B) | When | Codepath |
| ---------- | ----------------- | ---- | -------- |
| `Off`      | (0, 0, 0)         | codec input device closed | `CoreS3AudioCodec::EnableInput(false)` |
| `Local`    | (40, 40, 40) dim white | mic ADC open, frames go ONLY to local wake-word / VAD | `EnableInput(true)` while `IsAudioProcessorRunning() == false` |
| `Stream`   | (200, 200, 200) bright white | mic ADC open AND audio processor is pushing opus frames to xiaozhi server | `EnableInput(true)` while `IsAudioProcessorRunning() == true`, OR upgrade in `EnableVoiceProcessing(true)` |

## State table — Camera

| `CameraState` | LED color (R,G,B) | When | Codepath |
| ------------- | ----------------- | ---- | -------- |
| `Off`         | (0, 0, 0)         | no consumer is dequeuing frames (detection disabled, no `Capture()` in flight) | guard destructors |
| `Active`      | (200, 0, 0) bright red | a consumer is actively dequeuing frames | guard constructors in `StackChanCamera::Capture` and `StackChanCamera::StreamCaptures` |

> Note: in the current scaffold the camera VIDIOC_STREAMON is permanent
> after init (see L362 in `tasks.md`). The LED tracks **consumer
> activity** — i.e. "is anything reading frames?" — rather than the raw
> V4L2 stream-on flag. See "Deferred work" below.

---

## Why this design

### Indicator follows the peripheral, not the server

Setting `MicState` and `CameraState` is **not** exposed via MCP. The mutators are private members of `PrivacyLeds`, reachable only through the friend RAII guard classes. The guards are inserted on the same code path that physically opens / closes the peripheral:

```
CoreS3AudioCodec::EnableInput(true)
  -> esp_codec_dev_open(input_dev_)         // hardware: ADC enabled
  -> MicPeripheralGuard(MicState::Local)    // hardware-tied: LED on

CoreS3AudioCodec::EnableInput(false)
  -> ~MicPeripheralGuard                    // hardware-tied: LED off
  -> esp_codec_dev_close(input_dev_)        // hardware: ADC disabled
```

This means a compromised xiaozhi server, or a misbehaving custom LLM provider, **cannot** turn capture on while the indicator is off. To bypass the indicator they would have to bypass the codec / V4L2 driver entirely — at which point the device is rooted and all bets are off anyway.

The diagnostic MCP tool `self.robot.get_privacy_state` reports the **current** indicator state for cross-checking from the server side, but it is **read-only** — the server cannot WRITE state through any MCP tool.

### Hybrid pixel scheme

Both `LeftNeonLight` and `RightNeonLight` animate ALL 6 LEDs of their ring on every `setColor(...)` call. We can't avoid that — those animations are what produce the listening / speaking / face-detect feedback the rest of the firmware relies on.

Instead, `PrivacyLeds::update()` re-asserts the two privacy pixels every tick, AFTER the ring animations have run for that tick. This is the same hybrid pattern documented in `NeonLight::setColorAt` and used by `self.robot.set_led_multi`. The trade-off: the privacy pixel briefly flashes in the animation colour at every tick boundary; in practice this is invisible at 50 Hz.

`update()` is plumbed into the `_stackchan_update_task` loop (see `hal.cpp`), running right after `GetStackChan().update()`.

---

## Audit findings — when IS the mic hot?

> Source files inspected:
> - `firmware/main/hal/board/cores3_audio_codec.cc` — actual peripheral driver
> - `firmware/xiaozhi-esp32/main/audio/audio_service.cc` — input task
> - `firmware/xiaozhi-esp32/main/application.cc` — state machine

The mic ADC is opened by `CoreS3AudioCodec::EnableInput(true)`, which calls `esp_codec_dev_open(input_dev_)`. The codec input device stays open as long as `input_enabled_ == true`.

**`EnableInput(true)` is called every time `AudioService::ReadAudioData` runs and the codec is currently disabled** (`audio_service.cc:186-189`). `ReadAudioData` is the inner-loop method of `AudioInputTask`; it fires whenever `AS_EVENT_WAKE_WORD_RUNNING` or `AS_EVENT_AUDIO_PROCESSOR_RUNNING` is set in the audio service event group.

`AS_EVENT_WAKE_WORD_RUNNING` is set whenever wake-word detection is enabled. `Application::HandleStateChangedEvent()` calls `audio_service_.EnableWakeWordDetection(true)` on every entry into `kDeviceStateIdle` (line 888). So in the steady state — boot → idle → no conversation — **the mic is hot continuously**, feeding wake-word detection.

`CheckAndUpdateAudioPowerState` will tear the codec down if `input_elapsed > AUDIO_POWER_TIMEOUT_MS` AND no input has been read in that window. But because wake-word feeds a 10 ms frame from `ReadAudioData` continuously, `last_input_time_` is constantly refreshed, so this timeout effectively never fires while wake-word is running.

**Conclusion: in the current Dotty production firmware, the microphone ADC is on essentially 24/7 from boot.** Frames are consumed locally by the wake-word model. Nothing is sent over WS unless `EnableVoiceProcessing(true)` is also called — which happens after wake-word triggers and the device transitions to `kDeviceStateListening`.

This is exactly the trust gap the privacy LED closes:

- Mic indicator dim white for ~99% of uptime: "I'm listening locally for the wake word."
- Mic indicator bright white for the duration of a conversation: "I'm streaming to the server."

### When does the camera stream?

Camera initialisation issues `VIDIOC_STREAMON` once in the `StackChanCamera` constructor (`stackchan_camera.cc:329`). `VIDIOC_STREAMOFF` is only ever issued in the destructor (line 375), which never runs during normal operation — `StackChanCamera` is constructed once via `Board::GetCamera()` and lives forever.

So the V4L2 driver always has DMA buffers in flight after `InitializeCamera()`. Reading frames vs not reading frames is the difference between `face_detector.processFrame()` running (calls `camera->StreamCaptures()` → `VIDIOC_DQBUF` / `VIDIOC_QBUF`) versus nothing happening — the DMA writes still hit the mmap buffers, but no one reads them.

The privacy LED currently tracks the **consumer**, not the V4L2 stream-on flag — see "Deferred work" below.

---

## Failure modes

- **PY32 IO expander failed to init.** `Hal::setRgbColor` is a no-op (`hal_io_expander.cpp:70-76`). The privacy LED is dark even while the peripheral may be active. There is no hardware fallback. We log loudly during init in `hal_io_expander_init()`.
- **A future patch enables the codec / camera without going through the guard.** The LED stays Off. This is a code-review defence only. To harden: make `AudioCodec::EnableInput` private and force callers through `MicPeripheralGuard`. Same for the camera. Not done in this scaffold.
- **Stack-chan ring animation overpaints the privacy pixel.** Mitigated by `PrivacyLeds::update()` running once per tick AFTER the ring animation.
- **An MCP tool calls `self.robot.set_led_multi` with index 6 or 7.** **Closed.** The `set_led_multi` handler in `hal_mcp.cpp` now rejects `index == privacy::kMicLedIndex || index == privacy::kCameraLedIndex` with a warn log and a `false` return, before any `setColorAt` call. The server is no longer trusted with the privacy pixels — they belong to the peripheral-enable code path (`MicPeripheralGuard` / `CameraPeripheralGuard`) only.

---

## Deferred work — true camera VIDIOC_STREAMOFF

As of this scaffold, the V4L2 stream-on flag is permanent after camera init. The right fix is:

1. Add `start_streaming()` / `stop_streaming()` to `StackChanCamera` that wrap `VIDIOC_STREAMON` / `VIDIOC_STREAMOFF` and re-queue mmap buffers correctly.
2. Move the constructor's `VIDIOC_STREAMON` into a no-op so the camera boots in a "stopped" state.
3. Have `FaceDetector::start()` / `FaceDetector::stop()` call those.
4. Have `Capture()` call them around its inner loop (it already takes the camera arbiter mutex).
5. Move the actual STREAMON / STREAMOFF into the `CameraPeripheralGuard` constructor / destructor so the LED state and the V4L2 state are literally bound to the same RAII object.

That's intentionally out of scope for the Layer 1 scaffold because it interacts with ESP-DL ISP warmup (see the 5-second post-STREAMON warmup loop in the constructor for `CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE`). Doing it without breaking detection requires a follow-up that we test on hardware.

---

## Family-card sticker

The intent is that on the actual product there is a small physical card / sticker shipped with Dotty saying:

> **What the lights mean**
>
> - **Dim white light on the right side** — Dotty is listening for her wake word. Audio stays on the device.
> - **Bright white light on the right side** — Dotty is talking to the cloud. Your conversation is being sent to the server.
> - **Bright red light on the right side** — Dotty's camera is on right now. A photo or face-detect is in progress.
>
> If the lights are off, Dotty is not capturing. If you ever see capture without the matching light, that's a bug — please report it.

The wording on this sticker is the *contract* this code implements. If the sticker is ever updated, the `kMic*` / `kCamera*` colour constants in `privacy_leds.h` MUST stay in sync.
