# AI Agent Development Guide — Echo Voice Gateway

> This file provides the full context an AI agent needs to understand, develop, and debug this project. Read this before making any changes.

## Project Overview

**Goal:** Build an ESP32-based voice assistant that connects **directly** to OpenAI's Realtime API over WebSocket (no proxy server). The device captures audio from a microphone, streams PCM audio to OpenAI, receives spoken responses, and plays them through a speaker — all on a tiny embedded device.

**Active Firmware:** `firmware/echoear_brookesia/` — this is the **only active development target**. All other directories (`arduino/`, `micropython/`, `platformio-espidf/`, `server/`) are legacy experiments and should not be modified unless explicitly requested.

---

## Hardware

### Primary Device: ESP32-S3-based Board (ESP-VOCAT v1.2)

This is **NOT** the original M5Stack ATOM Echo anymore. The active hardware is an ESP-VOCAT v1.2 board with:

- **SoC:** ESP32-S3 (QFN56, revision v0.2) — dual-core 240MHz, Wi-Fi + BLE
- **Memory:** 4MB Flash + 16MB PSRAM (AP_1v8) — PSRAM is critical for audio buffers
- **USB Mode:** USB-Serial/JTAG (not classic UART)
- **Audio Codec:** Onboard I2S codec (managed by `esp_board_manager`)
- **Display:** Round LCD (ST77916 controller) with touch (CST816S) — used for animated face/expressions
- **Wake Word Engine:** ESP-SR with `wn9_hiesp` model ("Hi, ESP")
- **MAC:** `90:e5:b1:ac:ae:d0`
- **Serial Port:** `COM4` (on the development Windows machine)

### Key Hardware Facts
- The board has AEC (Acoustic Echo Cancellation) built into the audio front-end — this helps prevent self-interruption but isn't perfect
- The display shows animated emoji expressions (listening, speaking, etc.) via LVGL
- Wi-Fi connects to network "YOUR_WIFI_SSID" (SSID stored in NVS, not hardcoded)

---

## Architecture

### Framework: ESP-Brookesia

The firmware is built on **ESP-Brookesia** — Espressif's agent framework. Key concepts:

1. **Agents** extend `esp_brookesia::agent::Base` and implement lifecycle callbacks:
   - `on_activate()` — allocate resources, set up connections
   - `on_startup()` — start the connection, begin listening
   - `on_shutdown()` — clean up
   - `on_encoder_data_ready()` — called by the audio pipeline with raw PCM from the mic
   - `on_sleep()` / `on_wakeup()` — power management

2. **Services** (WiFi, Audio, NVS, SNTP) are managed by `SvcMgr`

3. **Agent Registration** uses macros:
   ```cpp
   BROOKESIA_PLUGIN_REGISTER_SINGLETON(Base, RealTimeAgent, ...)
   BROOKESIA_PLUGIN_REGISTER_SINGLETON(service::ServiceBase, RealTimeAgent, ...)
   ```

4. **Agent Selection** in `ai_agents.cpp`:
   ```cpp
   AgentHelper::SetAgentInfo("RealTimeAgent", ...);
   ```

### Our Custom Agent: `RealTimeAgent`

**Files:**
- `main/modules/agent_realtime.cpp` — full implementation
- `main/modules/agent_realtime.hpp` — class declaration
- `main/modules/ai_agents.cpp` — agent registration and initialization glue

**Data Flow:**
```
Microphone → AFE (AEC + VAD) → on_encoder_data_ready()
    → PCM → Base64 encode → JSON wrap → WebSocket → OpenAI

OpenAI → WebSocket → JSON parse → Base64 decode
    → PCM → feed_audio_decoder_data() → Speaker
```

**Audio Format:**
- Sample rate: 24000 Hz (OpenAI Realtime API requirement)
- Channels: 1 (mono)
- Bit depth: 16-bit signed PCM
- Frame duration: 60ms
- The AFE outputs at 16kHz but the framework resamples to match the agent's configured 24kHz

### Configuration System (Kconfig)

Runtime settings are configured via ESP-IDF's Kconfig system (`Kconfig.projbuild`):

| Config Key | Purpose | Current Value |
|---|---|---|
| `CONFIG_EXAMPLE_AGENTS_ENABLE_REALTIME` | Enable RealTimeAgent | `y` |
| `CONFIG_EXAMPLE_AGENTS_REALTIME_API_KEY` | OpenAI API key | Set in sdkconfig (gitignored) |
| `CONFIG_EXAMPLE_AGENTS_REALTIME_MODEL` | Model identifier | `gpt-4o-realtime-preview` |
| `CONFIG_EXAMPLE_AGENTS_REALTIME_VOICE` | TTS voice | `alloy` |
| `CONFIG_EXAMPLE_AGENTS_REALTIME_SYSTEM_PROMPT` | System instructions | Short IoT assistant prompt |
| `CONFIG_EXAMPLE_AGENTS_ENABLE_XIAOZHI` | Enable Xiaozhi agent | `n` (MUST stay disabled) |

> **CRITICAL:** `CONFIG_BROOKESIA_AGENT_XIAOZHI_ENABLE_AUTO_REGISTER` must be `n`. When enabled, the Xiaozhi agent auto-registers and fights for the websocket, flooding the serial log with "Websocket client is not connected" errors.

### sdkconfig Notes

- `sdkconfig` is **gitignored** because it contains the OpenAI API key
- `sdkconfig.defaults` should be used for version-controlled defaults
- After cloning, a developer must run `idf.py menuconfig` or manually create `sdkconfig` with their API key
- The API key format: `sk-proj-...` (OpenAI project key)

---

## Build & Flash

### Prerequisites
- **ESP-IDF v5.5** installed at `C:\Users\ericr\esp\esp-idf`
- **Python 3.13+** with ESP-IDF virtual environment
- Board connected on `COM4`

### Build Commands
```powershell
# Activate ESP-IDF environment
& "C:\Users\ericr\esp\esp-idf\export.ps1"

# Build
cd C:\Users\ericr\echo-voice-gateway\firmware\echoear_brookesia
idf.py build

# Flash (app partition only — fastest for iterating)
python -m esptool --chip esp32s3 -p COM4 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0xb0000 build\example_agent_chatbot.bin

# Flash everything (bootloader, partition table, models, app, spiffs, animations)
idf.py -p COM4 flash
```

### Serial Monitoring

The device produces **extremely verbose** serial output. Use a filtered monitor:

```python
# Filtered monitor script (filters out "not connected" spam):
# Located at: scratch/filtered_monitor.py
python filtered_monitor.py COM4 115200
```

`idf.py monitor` does NOT work in non-TTY environments (e.g., AI agent terminals). Use `python -m serial.tools.miniterm COM4 115200 --raw` or the filtered Python script above.

### Reset Sequence

The ESP32-S3 with USB-Serial/JTAG does not respond to RTS/DTR toggle for reset. To capture boot messages:
1. Flash with `--after hard_reset` (esptool resets the chip)
2. Immediately open serial monitor
3. Or use `esptool.py --chip esp32s3 -p COM4 --after hard_reset run` then open monitor

---

## Current Status (as of 2026-05-30)

### What Works ✅
- Wi-Fi connects automatically (~5.7s boot to connected)
- TLS certificate validation via `esp_crt_bundle`
- WebSocket connects to `wss://api.openai.com/v1/realtime` (~10s from boot)
- Session update JSON is sent successfully (389 bytes)
- Audio pipeline captures mic data and Base64 encodes it
- Audio playback pipeline works (PCM → speaker)
- Self-interruption prevention (mic muted during speaking)
- Server-side VAD configured for turn detection
- Display expressions (partial — "listening" emoji not found, needs asset)

### What Needs Fixing 🔧

1. **Model Access Error:** OpenAI returns `"The model 'gpt-4o-realtime-preview' does not exist or you do not have access to it."` — the model name needs to be updated to a valid GA model identifier. Check OpenAI's documentation for current model names. Possible fixes:
   - `gpt-4o-realtime-preview-2024-12-17` (specific version)
   - `gpt-4o-mini-realtime-preview` (cheaper option)
   - Check the user's OpenAI account for which models are enabled

2. **"Not connected" log spam:** After the server closes the connection (due to error), the audio pipeline keeps trying to send via `esp_websocket_client_send_text`, which logs errors. The guard in `encode_and_send_audio()` catches our code path, but the websocket library's internal reconnect loop may also trigger this. Consider adding `ws_cfg.disable_auto_reconnect = true` during error states.

3. **Display "listening" emoji:** The expression system reports `Not found:"listening"` — the animated emoji asset needs to be added to the SPIFFS partition or the emote name mapped to an existing one.

### Historical Issues (Resolved) ✅
- **Beta header rejection:** `OpenAI-Beta: realtime=v1` header was required during beta but now causes `"The Realtime Beta API is no longer supported"`. Removed.
- **Xiaozhi agent conflict:** When both Xiaozhi and RealTimeAgent were enabled, Xiaozhi auto-registered and consumed websocket resources, causing connection spam. Fixed by disabling `CONFIG_EXAMPLE_AGENTS_ENABLE_XIAOZHI` and `CONFIG_BROOKESIA_AGENT_XIAOZHI_ENABLE_AUTO_REGISTER`.
- **String lifetime bug:** `ws_uri_` was originally a local variable in `on_activate()`, causing the `esp_websocket_client_config_t::uri` pointer to dangle. Fixed by making it a class member.
- **TLS stack overflow:** Default `task_stack` (4KB) was too small for TLS handshake. Increased to 16KB.
- **Buffer too small:** Default `buffer_size` was too small for OpenAI JSON payloads. Increased to 8KB.
- **Proxy dependency:** Originally used `realtime_proxy.py` (Python WebSocket proxy on the host machine). Eliminated in favor of direct `wss://` from the ESP32.

---

## Key Design Decisions

### Direct WebSocket (No Proxy)
We connect directly from the ESP32 to `wss://api.openai.com/v1/realtime` — no intermediate server. This means:
- The OpenAI API key is embedded in the firmware (in `sdkconfig`, gitignored)
- TLS is handled by `mbedtls` on the ESP32 via `esp_crt_bundle`
- The websocket library (`espressif__esp_websocket_client`) handles reconnection

### PSRAM Buffer Allocation
All large buffers (Base64 encode/decode, JSON send buffer) are allocated in PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` to avoid fragmenting the limited internal SRAM (~78KB free at boot).

### Server-Side VAD
We use OpenAI's server-side Voice Activity Detection rather than local VAD for turn management. This means:
- Audio is streamed continuously (when not speaking)
- OpenAI decides when the user has finished talking
- Configured with 500ms silence threshold and 300ms prefix padding

### Self-Interruption Prevention
During playback (`is_speaking() == true`), `on_encoder_data_ready()` discards all microphone data to prevent the device from hearing itself and creating feedback loops. A 1500ms timeout resets the speaking state if no new audio deltas arrive.

---

## File Reference

### Core Files (modify these)
| File | Purpose |
|---|---|
| `main/modules/agent_realtime.cpp` | RealTimeAgent implementation — WebSocket, audio I/O, JSON protocol |
| `main/modules/agent_realtime.hpp` | RealTimeAgent class declaration |
| `main/modules/ai_agents.cpp` | Agent initialization, Kconfig→agent wiring, emote handlers |
| `main/Kconfig.projbuild` | Menu configuration for API key, model, voice, system prompt |
| `main/CMakeLists.txt` | Build file — lists source files for compilation |

### Framework Files (read, don't modify)
| File | Purpose |
|---|---|
| `managed_components/espressif__esp_brookesia/` | Brookesia framework (agent base classes, services) |
| `managed_components/espressif__esp_websocket_client/` | WebSocket client library |
| `main/modules/display/` | Display driver, LVGL screens, animated expressions |
| `main/modules/wifi_provisioning.cpp` | Wi-Fi auto-connect logic |

### Config Files
| File | Purpose |
|---|---|
| `sdkconfig` | **GITIGNORED** — contains API key and all build config |
| `sdkconfig.defaults` | Version-controlled defaults (if it exists) |
| `partitions.csv` | Flash partition layout |

---

## OpenAI Realtime API Protocol

The WebSocket connection uses JSON text frames. Key message types:

### Client → Server
- `session.update` — configure voice, modalities, VAD, system prompt
- `input_audio_buffer.append` — send Base64-encoded PCM audio chunks

### Server → Client
- `session.created` / `session.updated` — session confirmation
- `response.audio.delta` — Base64-encoded audio chunk (spoken response)
- `response.audio.done` — end of audio stream
- `response.done` — full response complete
- `input_audio_buffer.speech_started` — server VAD detected speech
- `input_audio_buffer.speech_stopped` — server VAD detected silence
- `error` — error with message field

### API Endpoint
```
wss://api.openai.com/v1/realtime?model=MODEL_NAME
```
Headers: `Authorization: Bearer sk-proj-...`

> **Note:** The API graduated from beta in early 2026. Do NOT include the `OpenAI-Beta: realtime=v1` header — it will cause an error.

---

## Debugging Tips

1. **Serial log floods:** The `websocket_client` library logs `"Websocket client is not connected"` at ERROR level for every failed send. This drowns out useful logs. Use the filtered monitor script or grep for `RealTimeAgent` tags.

2. **TLS failures:** If the cert bundle validation fails, check SNTP time sync. TLS certs require correct system time — the device syncs via NTP at boot.

3. **Memory pressure:** Check `Free heap` and `PSRAM` values in the activation log. If internal heap drops below ~50KB, the TLS handshake may fail.

4. **Audio resampling:** The AFE outputs 16kHz audio, but OpenAI expects 24kHz. The Brookesia audio service handles resampling between the AFE and the agent's configured sample rate.

5. **Build caching:** After changing `sdkconfig` values, sometimes a `idf.py fullclean` is needed, but usually `idf.py build` picks up changes. Only `agent_realtime.cpp` changes trigger a fast incremental build (~30s).

---

## Git Workflow

- **Branch:** `main` (single branch for now)
- **Remote:** `origin` → `https://github.com/eric-rolph/m5stack-atom-echo-voice-assistant.git`
- **Ignored:** `sdkconfig`, `build/`, `managed_components/` (downloaded by ESP-IDF component manager)
- **Commit style:** Descriptive commits explaining what and why

---

## Next Steps (Priority Order)

1. **Fix model name** — update `CONFIG_EXAMPLE_AGENTS_REALTIME_MODEL` in sdkconfig to a valid model the user's account has access to
2. **Verify end-to-end audio** — speak to device, confirm OpenAI responds with audio
3. **Fix display expressions** — map emote names to available animated assets
4. **Add reconnection handling** — graceful reconnect after network drops
5. **Git push working state** — commit and push once voice conversation works end-to-end
