#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include "config.h"
#include "base64.h"

#define I2S_PORT I2S_NUM_0

// Codec addresses
#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x40

WebSocketsClient webSocket;
i2s_chan_handle_t rx_chan;
i2s_chan_handle_t tx_chan;

enum SystemState {
  STATE_INIT,
  STATE_WIFI_CONNECTING,
  STATE_WIFI_CONNECTED,
  STATE_WS_CONNECTING,
  STATE_WS_CONNECTED,
  STATE_READY,
  STATE_RECORDING,
  STATE_SPEAKING,
  STATE_ERROR
};

SystemState currentState = STATE_INIT;
unsigned long lastPingTime = 0;
String sessionId = "";

void setupWiFi();
void setupI2S();
void setupCodecs();
void setupWebSocket();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendSessionUpdate();
void handleAudioTask();

void write_i2c(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ATOM Echo Voice Assistant (PlatformIO) ===");

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Power on audio circuits
    pinMode(POWER_AUDIO_PIN, OUTPUT);
    digitalWrite(POWER_AUDIO_PIN, HIGH);
    pinMode(PA_CONTROL_PIN, OUTPUT);
    digitalWrite(PA_CONTROL_PIN, HIGH);

    setupWiFi();

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
    setupCodecs();
    setupI2S();
    setupWebSocket();

    Serial.println("Setup complete - Ready!");
    currentState = STATE_READY;
}

void loop() {
    webSocket.loop();
    handleAudioTask();
}

void setupWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    currentState = STATE_WIFI_CONNECTING;
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
    Serial.println(WiFi.localIP());
    currentState = STATE_WIFI_CONNECTED;
}

void setupCodecs() {
    Serial.println("Initializing ES8311 and ES7210 via I2C...");
    // Basic ES8311 init
    write_i2c(ES8311_ADDR, 0x45, 0x00); // clear CGREG10
    write_i2c(ES8311_ADDR, 0x01, 0x3F); // chip reset
    delay(10);
    write_i2c(ES8311_ADDR, 0x01, 0x00); // clear reset
    write_i2c(ES8311_ADDR, 0x0D, 0x01); // system start
    write_i2c(ES8311_ADDR, 0x0E, 0x02); // system start
    write_i2c(ES8311_ADDR, 0x12, 0x00); // 16bit I2S
    write_i2c(ES8311_ADDR, 0x13, 0x10); // I2S, 16bit
    write_i2c(ES8311_ADDR, 0x14, 0x1A); // DAC Mute off
    write_i2c(ES8311_ADDR, 0x32, 0xBF); // DAC volume
    
    // Basic ES7210 init
    write_i2c(ES7210_ADDR, 0x01, 0x3F); // reset
    delay(10);
    write_i2c(ES7210_ADDR, 0x01, 0x00); // clear reset
    write_i2c(ES7210_ADDR, 0x08, 0x00); // MCLK input
    write_i2c(ES7210_ADDR, 0x11, 0x60); // 16bit I2S
    write_i2c(ES7210_ADDR, 0x40, 0x42); // MIC gain
    write_i2c(ES7210_ADDR, 0x41, 0x70); // ADC vol
}

void setupI2S() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_WS_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din = (gpio_num_t)I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

void setupWebSocket() {
    Serial.println("Setting up WebSocket...");
    currentState = STATE_WS_CONNECTING;
    webSocket.begin(PROXY_IP, PROXY_PORT, "/realtime");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected!");
            currentState = STATE_WIFI_CONNECTED;
            break;
        case WStype_CONNECTED:
            Serial.println("[WS] Connected to Proxy!");
            currentState = STATE_WS_CONNECTED;
            sendSessionUpdate();
            break;
        case WStype_TEXT:
            {
                DynamicJsonDocument doc(4096);
                deserializeJson(doc, payload, length);
                const char* eventType = doc["type"];
                if (eventType && strcmp(eventType, "session.created") == 0) {
                    sessionId = doc["session"]["id"].as<String>();
                    Serial.println("[WS] Session created.");
                    currentState = STATE_READY;
                }
            }
            break;
        case WStype_BIN:
            // Receive raw 16-bit PCM audio from proxy and play it
            size_t bytes_written = 0;
            if (tx_chan) {
                i2s_channel_write(tx_chan, payload, length, &bytes_written, portMAX_DELAY);
            }
            break;
    }
}

void sendSessionUpdate() {
    DynamicJsonDocument doc(2048);
    doc["type"] = "session.update";
    JsonObject session = doc.createNestedObject("session");
    session["type"] = "realtime";
    session["output_modalities"][0] = "audio";
    session["instructions"] = "You are a helpful voice assistant on an EchoEar.";
    
    JsonObject audio = session.createNestedObject("audio");
    
    JsonObject audioInput = audio.createNestedObject("input");
    JsonObject audioInputFormat = audioInput.createNestedObject("format");
    audioInputFormat["type"] = "audio/pcm";
    audioInputFormat["rate"] = 24000;
    
    JsonObject turnDetection = audioInput.createNestedObject("turn_detection");
    turnDetection["type"] = "server_vad";
    turnDetection["threshold"] = 0.5;
    turnDetection["prefix_padding_ms"] = 300;
    turnDetection["silence_duration_ms"] = 500;
    
    JsonObject audioOutput = audio.createNestedObject("output");
    JsonObject audioOutputFormat = audioOutput.createNestedObject("format");
    audioOutputFormat["type"] = "audio/pcm";
    audioOutputFormat["rate"] = 24000;
    audioOutput["voice"] = "alloy";
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
}

void handleAudioTask() {
    // If we're connected to websocket, stream microphone data continuously 
    // server_vad handles Voice Activity Detection on the server side!
    if (currentState >= STATE_READY) {
        uint8_t buffer[1024];
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_chan, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
            if (bytes_read > 0) {
                // We send raw binary to the proxy!
                webSocket.sendBIN(buffer, bytes_read);
            }
        }
    }
}
