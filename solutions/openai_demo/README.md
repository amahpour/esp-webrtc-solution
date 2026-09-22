# Voice lab assistant — OpenAI Realtime on the ESP32-S3-BOX-3

Talk to an ESP32-S3-BOX-3 and it changes a voltage on your bench. The model decides what to do; the firmware decides what's allowed.

This is a fork of Espressif's `openai_demo`. Upstream builds the hard part — a WebRTC session to OpenAI's Realtime API, Opus both ways, tool calls over the data channel — and hands the model a smart-home toolset whose handlers only `printf`. Here the tools drive a pair of DAC7578 converters over I2C, the box listens for a wake word before it connects to anything, and the screen shows what it heard and did.

**Architecture, before and after: https://amahpour.github.io/esp-webrtc-solution/**

---

## What it does

- **Speech in, speech out.** Say *"set channel three to one point eight volts"*; the output steps and the box tells you it did it. A scope on that channel shows 1.801 V for a commanded 1800 mV.
- **Real limits.** Ask for nine volts and the firmware refuses, tells the model why, and the model says so out loud. Nothing is clipped to a nearby value and nothing is written.
- **It also just talks.** Ask it anything — it knows electronics, and only reaches for a tool when one fits.
- **Quiet until addressed.** While idle it runs the wake-word engine locally and throws every audio frame away. Nothing reaches OpenAI, and nothing is billed, until someone says *"Hi, ESP"*.
- **Closes itself.** Two minutes without speech ends the session. It never reconnects silently.
- **Shows its work.** The LCD carries the connection state, the last thing said, and the last tool call — green when it ran, red when it was refused.

### The tools the model sees

| Tool | Arguments | Effect |
|---|---|---|
| `set_channel_voltage` | `channel` 0–15, `millivolts` 0–3200 | 3-byte I2C write to a DAC7578; the output steps immediately |
| `get_channel_voltage` | `channel` 0–15 | Reports the last value written to that channel |
| `set_speaker_volume` | `volume` 0–100 | Really sets the ES8311 codec |

Limits live in [`main/bench_dac.h`](main/bench_dac.h): `BENCH_DAC_MAX_MV`, `BENCH_DAC_CHANNEL_MASK`. Out-of-range requests return `ESP_ERR_INVALID_ARG` and an error JSON, before any I2C traffic.

---

## Hardware

| Item | Notes |
|---|---|
| **ESP32-S3-BOX-3** | The main board plus its dock. Chip is an ESP32-S3 with 16 MB flash and 16 MB PSRAM. |
| **2 × DAC7578 breakout** | 8 channels each. Optional — without them the demo still runs, talks and drives the speaker; only the voltage tools fail. |
| **USB-C cable** | Console, logs and flashing all go over the S3's native USB. This board has no separate serial chip. |
| **Bench supply (recommended)** | Powering the DACs from the box's 3V3 rail works but the rail sags under audio load, and the DAC is ratiometric, so the output moves with it. |
| **Scope or meter** | To see the point. |

### Wiring

The dock exposes Espressif's designated dock I2C bus, which is separate from the internal codec and touch bus on GPIO8/18:

| Signal | BOX-3 dock pin | To |
|---|---|---|
| SCL | **GPIO40** | Both DAC boards' SCL |
| SDA | **GPIO41** | Both DAC boards' SDA |
| GND | GND | Both DAC boards, **and the bench supply's ground** |
| VCC | 3V3, or the bench supply | Both DAC boards' VCC (this is also their reference) |

Addresses: **DAC A at 0x4C** serves channels 0–7, **DAC B at 0x48** serves channels 8–15. Set them with the address jumpers on the breakouts. Both boards carry 10 kΩ pull-ups, which is fine for one or two boards on a short bus.

The DAC's reference is whatever feeds VCC, so the firmware needs to know it: see `vref` under [Console](#console) below.

---

## Build and flash

### Prerequisites

**ESP-IDF v5.5.x.** The version in upstream's README (v5.4) does not build this demo: it fails in `esp_asrc` with `error: format '%d' expects argument of type 'int'` under `-Werror=format`. CI uses the `espressif/idf:release-v5.5` image.

```bash
. $IDF_PATH/export.sh          # v5.5.x
export OPENAI_API_KEY=sk-...   # read at build time, never stored in the repo
```

### Configure

Edit [`main/settings.h`](main/settings.h):

```c
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"
```

Everything else has a working default. The knobs worth knowing:

| Setting | Default | What it does |
|---|---|---|
| `WAKE_WORD_ENABLED` | 1 | Wait for *"Hi, ESP"* before opening a session |
| `AUTO_START_SESSION` | 0 | Open a session as soon as Wi-Fi is up. **Bills from boot**; useful when filming |
| `SESSION_IDLE_TIMEOUT_S` | 120 | Close the session after this much silence, 0 to disable |
| `AEC_REF_TDM_SLOT` | 1 | Which microphone slot carries the speaker's own sound. Measured with `micscan` |
| `MIC_INPUT_GAIN_DB` | 36 | Analog gain on the microphone slots only |
| `MIC_GATE_WHILE_SPEAKING` | 1 | Mute the mic while the box talks (half duplex). Without it, it answers its own echo |
| `MIC_GATE_DRAIN_MS` | 300 | Quiet time after the speaker stops before the mic reopens |
| `PLAYBACK_JITTER_MS` | 0 | Keep at 0 unless you have measured a reason: any other value turns small network dips into audible stutters |
| `DEFAULT_PLAYBACK_VOL` | 85 | Starting speaker volume |

The model is `gpt-realtime-mini` (`OPENAI_DEFAULT_MODEL` in [`main/common.h`](main/common.h)), switchable at runtime with `model`.

### Flash

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Partition sizes and flash settings are already in [`partitions.csv`](partitions.csv) and [`sdkconfig.defaults`](sdkconfig.defaults): a 6 MB app partition (the app is ~4.1 MB and does not fit upstream's 3 MB), 16 MB flash, a 4 MB `model` partition for the wake-word data, and the console on USB-Serial-JTAG. `idf.py flash` writes the wake-word model image too.

If the port number moves after a reset, find it by its stable id: `ls /dev/serial/by-id/*Espressif*`.

---

## Running it

1. Power up. The screen shows Wi-Fi progress, then **Say "Hi, ESP" to start**.
2. Say **"Hi, ESP"**, or type `start`. The screen goes to **Listening…** in green.
3. Talk:
   - *"Set channel three to one point eight volts."*
   - *"What's channel three at?"*
   - *"Turn your volume down to fifty."*
   - *"Set channel two to nine volts."* → refused, and it tells you why.
4. `stop`, or two minutes of quiet, ends the session.

Wait for the green **Listening…** before speaking: the mic is muted while the box talks, so a sentence started too early loses its first word.

### Console

Over the USB-Serial-JTAG port. Upstream's commands, plus the diagnostics this fork needed.

| Command | Does |
|---|---|
| `start` / `stop` | Open or close a session by hand |
| `wake` | Simulate the wake word, same path as saying it |
| `text "..."` | Send a typed message instead of speaking |
| `dac <ch> <mv>` | Drive a channel directly, with the same limits as the voice tool |
| `vref [mv]` | Show or re-calibrate the DAC reference. Drive a channel to 3200 mV, measure the output, then `vref <measured>` |
| `micscan` | Play a tone and print the RMS of all four microphone slots — this is how you find `AEC_REF_TDM_SLOT`. The reference slot is dramatically hotter than the others |
| `vad <0.0-1.0>` | Speech-detection threshold, applied live. Lower is more sensitive |
| `vol [0-100]` | Speaker volume. Louder makes echo more likely |
| `model [name]` | Show or choose the realtime model; applies on the next `start` |
| `net` | Wi-Fi signal strength. Below about −67 dBm, audio gets choppy and no buffering can fix it |
| `trace [0\|1]` | Log realtime audio events with timing |
| `i` / `m` | System status / CPU and memory load |
| `rec2play` | Record and play back locally, to test the audio path without the cloud |

---

## Cost and privacy

A realtime session bills for as long as it is open, roughly $0.02 per minute listening and $0.08 per minute speaking, as measured on this rig. Realtime is **not** in OpenAI's free tier: `/v1/realtime/client_secrets` happily returns a token with no credits on the account, and only `/v1/realtime/calls` returns 429. If the call is rejected, the reason appears on the screen.

Three defaults keep that bounded, and they are the reason the wake word exists: no auto-start, a wake word before anything connects, and a 120-second idle timeout. While idle the microphone is live locally — the wake-word engine needs it — but every frame is discarded on the device.

Your API key is read from the environment at build time and compiled in. It is never committed.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Build fails in `esp_asrc` on a format warning | ESP-IDF v5.4. Use v5.5.x |
| "app partition is too small" | Stale build directory; `idf.py fullclean`. The sizes here are correct |
| Nothing on the serial port | This board has no UART bridge. Use the USB-C port that enumerates as `/dev/ttyACM*`, and note the number changes after a reset while a program holds the port |
| It answers itself in a loop | The AEC reference slot is wrong. Run `micscan` and set `AEC_REF_TDM_SLOT` to the hot slot |
| It cannot hear you at all | The AEC reference points at a real microphone, so the canceller is subtracting one mic from the other. Same fix |
| It talks over itself, or cuts you off | Check `vad`; raise the threshold in a noisy room |
| Choppy audio | Check `net` first. Genuine packet loss looks like buffering trouble, and `PLAYBACK_JITTER_MS` will not help |
| Voltage is off by a few percent | `vref` does not match the actual supply. Re-measure and set it |
| `DAC B @0x48: not responding` at boot | Only one breakout fitted, or its address jumpers differ. Channels 0–7 still work |
| Session rejected immediately | No credits on the account, or a bad key. The screen shows OpenAI's own reason |

---

## How this differs from upstream

The full picture, with diagrams: **https://amahpour.github.io/esp-webrtc-solution/**

| | Upstream | Here |
|---|---|---|
| Board | ESP32-S3-Korvo-2 | ESP32-S3-BOX-3 and its dock |
| Tools | Six `printf` setters: light, RGB, volume, door | Two DAC tools and a real volume control |
| Limits | None needed: nothing moved | Channel and voltage limits, refused rather than clipped |
| Argument parsing | A wrong JSON type reported success and kept the previous value | `"3"` and `3` both parse; no value survives between calls |
| Tool result | `{"ok":true}` whenever the name matched | The real outcome, or the error, returned to the model |
| Echo | On-chip AEC only | AEC reference measured, plus the mic muted while speaking |
| Turn taking | Server defaults | `server_vad` with barge-in, near-field noise reduction, uplink transcription |
| Session | Opens at boot, stays open | Wake word or `start`, closes after 120 s idle |
| Feedback | Serial log | Status, transcript and tool results on the LCD |
| Toolchain | IDF v5.4 or master, 4 MB flash, 3 MB app | IDF v5.5.5, 16 MB flash, 6 MB app, 4 MB wake-word model |

One component is vendored rather than pulled from the registry: `esp_capture`, so the wake-word result the AFE already computes can be surfaced through a callback. Two additive edits, documented in [`components/esp_capture/README-fork.md`](../../components/esp_capture/README-fork.md).

---

## How it works

Signaling follows upstream: the API key mints a short-lived token from `POST /v1/realtime/client_secrets`, then SDP is exchanged through `POST /v1/realtime/calls` with that token. No STUN or TURN — `on_ice_info` returns `stun_url` as `NULL`. Everything after that is the normal `esp_webrtc` [call sequence](../../components/esp_webrtc/README.md#typical-call-sequence-of-esp_webrtc).

Once the data channel is up, the device sends `session.update` with its instructions, its tools, and the audio settings. A tool call arrives as `response.function_call_arguments.done`; the device runs it and replies with `conversation.item.create` carrying a `function_call_output`, then `response.create` so the model speaks the outcome.

Two details worth knowing if you build on this:

- **`response.done` does not mean the speaker has stopped.** The render FIFO holds seconds of audio. The mic gate waits for `output_audio_buffer.stopped` and then a short drain, or it reopens the mic into the tail of the box's own sentence.
- **Tool arguments arrive with inconsistent JSON types.** The same tool gets `{"channel": 3}` and `{"channel": "3"}` from the same model. Both are accepted here, and values never carry over between calls.

Source layout: [`main/webrtc.c`](main/webrtc.c) (session config, event routing, mic gate, tool execution), [`main/media_sys.c`](main/media_sys.c) (capture, playback, wake-word listening), [`main/bench_dac.c`](main/bench_dac.c) (I2C and limits), [`main/display.c`](main/display.c) (LCD), [`main/main.c`](main/main.c) (session lifecycle and console).

---

## Credits

Forked from [espressif/esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution). The WebRTC stack, media pipeline and the original demo are Espressif's work; the same licenses apply.
