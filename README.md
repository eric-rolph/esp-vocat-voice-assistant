# Echo Voice Gateway — ESP32 Voice AI Assistant

Real-time voice assistant that connects directly to OpenAI's Realtime API over WebSocket. Captures audio, streams it to OpenAI, and plays back spoken AI responses — all running on an ESP32-S3 with no proxy or intermediate server.

## 🎯 Project Status

**Active Development:** `firmware/echoear_brookesia/` — ESP-IDF + ESP-Brookesia framework

**Current State:**
- ✅ Direct WSS connection to OpenAI Realtime API (no proxy)
- ✅ TLS handshake + certificate validation
- ✅ Session configuration (voice, VAD, modalities)
- ✅ Bidirectional audio pipeline (mic → OpenAI → speaker)
- ✅ Server-side VAD for natural turn-taking
- ✅ Self-interruption prevention with precise playback duration tracking
- ✅ Display expressions mapped to supported 'neutral' emoji asset

> For detailed AI agent development context, see [AGENTS.md](AGENTS.md).

## 🛠️ Hardware

**Primary Device:** ESP-VOCAT v1.2 Board
- ESP32-S3 (QFN56) — 240MHz Dual Core, 4MB Flash, 16MB PSRAM
- I2S audio codec with microphone and speaker
- Round LCD display with touch (ST77916 + CST816S)
- Wi-Fi 802.11 b/g/n + BLE 5.0
- Wake word: "Hi, ESP"

**Legacy Hardware:** M5Stack ATOM Echo — see `platformio-espidf/` directory (not actively maintained)

## 📁 Project Structure

```
echo-voice-gateway/
├── firmware/echoear_brookesia/    # ⭐ ACTIVE — ESP-IDF firmware
│   ├── main/
│   │   ├── modules/
│   │   │   ├── agent_realtime.cpp  # OpenAI Realtime API agent
│   │   │   ├── agent_realtime.hpp  # Agent class declaration
│   │   │   └── ai_agents.cpp       # Agent registration & init
│   │   ├── Kconfig.projbuild       # Menuconfig options
│   │   └── CMakeLists.txt          # Build configuration
│   ├── sdkconfig                   # Build config (gitignored, has API key)
│   └── managed_components/         # ESP-IDF dependencies (auto-downloaded)
├── AGENTS.md                        # 📖 AI agent development guide
├── platformio-espidf/               # Legacy: M5Stack ATOM Echo firmware
├── arduino/                         # Legacy: Arduino implementation
├── micropython/                     # Legacy: MicroPython implementation
└── server/                          # Legacy: Python proxy server (no longer needed)
```

## 🚀 Quick Start

### Prerequisites
- ESP-IDF v5.5
- ESP-VOCAT v1.2 board (or compatible ESP32-S3 board)
- OpenAI API key with Realtime API access
- Wi-Fi network (2.4GHz)

### Setup

```bash
# 1. Clone
git clone https://github.com/eric-rolph/m5stack-atom-echo-voice-assistant.git
cd m5stack-atom-echo-voice-assistant/firmware/echoear_brookesia

# 2. Configure
idf.py menuconfig
# Navigate to: Example Configuration → set API key, model, voice

# 3. Build & Flash
idf.py build
idf.py -p COM4 flash

# 4. Monitor (filtered to reduce noise)
python -m serial.tools.miniterm COM4 115200 --raw
```

### Configuration (via menuconfig)

| Setting | Key | Default |
|---------|-----|---------|
| OpenAI API Key | `EXAMPLE_AGENTS_REALTIME_API_KEY` | (required) |
| Model | `EXAMPLE_AGENTS_REALTIME_MODEL` | `gpt-4o-realtime-preview` |
| Voice | `EXAMPLE_AGENTS_REALTIME_VOICE` | `alloy` |
| System Prompt | `EXAMPLE_AGENTS_REALTIME_SYSTEM_PROMPT` | Short IoT assistant |

## 🔊 Audio Pipeline

```
Microphone → AFE (AEC + VAD) → 16-bit PCM → Base64 → JSON → WSS → OpenAI
OpenAI → WSS → JSON parse → Base64 decode → PCM → Speaker
```

- **Input:** 24kHz, 16-bit, mono PCM
- **Output:** 24kHz, 16-bit, mono PCM
- **Protocol:** OpenAI Realtime API over WebSocket (JSON text frames)
- **VAD:** Server-side (OpenAI handles turn detection)

## 🏗️ Architecture

The firmware uses **ESP-Brookesia** (Espressif's agent framework):

- `RealTimeAgent` extends `esp_brookesia::agent::Base`
- Connects directly to `wss://api.openai.com/v1/realtime` — no proxy needed
- All large buffers allocated in PSRAM to preserve internal SRAM
- TLS via `mbedtls` with ESP certificate bundle
- Automatic SNTP time sync (required for TLS certificate validation)

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

## 🙏 Acknowledgments

- **Espressif** — ESP-IDF framework, ESP-Brookesia agent system, and audio front-end
- **OpenAI** — Realtime API
- **M5Stack** — Original ATOM Echo hardware inspiration
