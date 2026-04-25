# microWakeWord Setup — "Hey Dotty"

Implementation guide for replacing the prebuilt ESP-SR WakeNet9 wake word with a
custom microWakeWord (TFLite-Micro) "Hey Dotty" model. **This is Path B in
`dotty-stackchan/docs/wake-word.md`.** Path A (prebuilt "Hi, ESP") is what ships
today — this file describes what comes next.

This is a scaffold / TODO, not yet implemented. When picked up, fill in the
empty sections with concrete patches and link back from the public doc.

## Prerequisites

- Trained `.tflite` artifact + manifest from
  [microWakeWord](https://github.com/kahrendt/microWakeWord) — see the public
  doc for sample-collection + training plan.
- ESP-IDF 5.5.x (already required for esp-sr 2.3.x).
- Free flash budget for a new partition (~256 KB).

## Component dependency

Add to `firmware/main/idf_component.yml` (NOT the bundled
`xiaozhi-esp32/main/idf_component.yml` — that's a vendored upstream and we
should keep our additions in our own component):

```yaml
dependencies:
  espressif/esp-tflite-micro: "~1.3.4"     # TFLite-Micro for ESP32
```

`microWakeWord` itself is not an IDF component; we're integrating its
*runtime* (load `.tflite`, run streaming-quantised inference) into our own
wake-word path. The training-side Python lib is not needed on-device.

## Partition table

Append to `firmware/partitions.csv`:

```
mww_model, data, fat,    ,         0x40000,
```

`0x40000` (256 KB) is comfortable headroom for the typical ~80 KB INT8
streaming-DS-CNN microWakeWord model. Place it after `human_face_det` so the
existing OTA layout is untouched.

After editing the partition table, reflash the partition table and the new
partition:

```
idf.py partition-table-flash
parttool.py --port <PORT> write_partition --partition-name=mww_model --input model.tflite
```

## Source layout

Plan to add the following files under
`firmware/main/stackchan/wake_word/`:

```
microwakeword.h         // class MicroWakeWord : public WakeWord  (mirror AfeWakeWord interface)
microwakeword.cc        // streaming inference task, tensor arena, threshold logic
mww_model_loader.cc     // mmap the mww_model partition into PSRAM
```

Mirror the existing `AfeWakeWord` interface (defined in
`firmware/xiaozhi-esp32/main/audio/wake_words/afe_wake_word.h`) so the
audio service can hold either implementation behind the same pointer:

```cpp
class WakeWord {
    virtual bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) = 0;
    virtual void OnWakeWordDetected(std::function<void(const std::string&)> cb) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual size_t GetFeedSize() = 0;
};
```

Critically: keep AFE running *in front of* microWakeWord. We still want
AEC + noise-suppress before any wake decision. The integration is:

```
Mic ──▶ AFE (AEC, NS, VAD) ──▶ MicroWakeWord (TFLite streaming) ──▶ wake event
```

— i.e. `MicroWakeWord::Feed` consumes AFE-cleaned PCM, not raw mic PCM.
Easiest path: subclass / fork `AfeWakeWord` and replace the `WAKENET_DETECTED`
fetch path with TFLite inference; everything else (encode-and-send wake PCM,
wake-word-during-listening, send-wake-word-data) stays identical.

## Kconfig

Add a new option under
`firmware/xiaozhi-esp32/main/Kconfig.projbuild` (or, better, in our own
`firmware/main/Kconfig.projbuild` to avoid touching upstream):

```
config USE_MICROWAKEWORD
    bool "microWakeWord (custom Hey Dotty TFLite)"
    depends on (IDF_TARGET_ESP32S3 || IDF_TARGET_ESP32P4) && SPIRAM
    select USE_AFE_WAKE_WORD     # we still want AFE upstream
    help
        Use a microWakeWord TFLite model (e.g. "Hey Dotty") in addition to
        AFE preprocessing. Model must be flashed to the mww_model partition.

config MICROWAKEWORD_THRESHOLD
    int "microWakeWord detection threshold (0-99)"
    default 50
    range 1 99
    depends on USE_MICROWAKEWORD
```

## Integration hook

The wake-event entry point is unchanged — `Application::HandleWakeWordDetectedEvent`
(`firmware/xiaozhi-esp32/main/application.cc:793`). The wake-word *string*
that propagates through `protocol_->SendWakeWordDetected(wake_word)` (line 859)
becomes `"Hey Dotty"` instead of `"hi esp"`. Server side, this string is
informational only — the server already supports arbitrary wake phrases — so
no bridge or xiaozhi-server change is required.

The state-change re-arm logic in `Application::HandleStateChangedEvent`
(`application.cc:872`) calls `audio_service_.EnableWakeWordDetection(...)`
agnostically of which engine is active. Wire `MicroWakeWord` through the
same `audio_service_.IsAfeWakeWord()` gate (or rename it to `IsAlwaysOn` and
have both AFE and MWW return true).

## Test plan

1. Flash model to `mww_model` partition. Verify `mmap` succeeds at boot — log
   should print model size.
2. Speak training phrase 10× from 1.5 m, no background noise. Expect 10/10
   detections with the threshold from the training manifest.
3. Run a 1-hour silence + TV-on negative test. Target ≤1 false-positive.
4. Hard-negative test: have someone say "Hey Daddy", "Hey Polly", "Hey Dotty
   ... no wait" — expect zero detections except for the clean trigger.
5. Compare CPU + memory: baseline (AFE only) vs (AFE + MWW). Should add ~5–8%
   CPU on core 1 and ~250–300 KB PSRAM. If higher, profile the tensor arena.
6. Field test: a week of normal household use. Log all wake events with
   timestamps. Hand-review logs for false positives.

## Rollback

`USE_MICROWAKEWORD=n` in menuconfig falls back to the prebuilt WakeNet9 model
selected via `SR_WN_WN9_*` — i.e. Path A. The `mww_model` partition can stay
flashed but unused; no code path reads it when the flag is off.

## Open questions for implementation day

- Tensor arena placement: `MALLOC_CAP_SPIRAM` vs `MALLOC_CAP_INTERNAL`? PSRAM
  is fine for steady-state but cold-cache latency on the first few inferences
  may push past the 30 ms audio frame budget. Benchmark both.
- microWakeWord ships its detection threshold inside the manifest JSON. Decide
  whether to bake it into the firmware (simpler) or read it from a sidecar
  file in the partition (more flexible for OTA model updates).
- OTA story for the model: extend the existing OTA endpoint
  (`CONFIG_OTA_URL`) to deliver `model.tflite` as a separate artifact, or
  bundle it into the app image? Separate is cleaner — model retraining
  shouldn't require a firmware OTA.
- Multi-wake-phrase support: microWakeWord trains one phrase per model. If we
  ever want both "Hey Dotty" and "Dotty wake up", run two interpreters. The
  ESP32-S3 has the headroom; just budget +250 KB PSRAM per extra phrase.
