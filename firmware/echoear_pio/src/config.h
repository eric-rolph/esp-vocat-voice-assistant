#ifndef CONFIG_H
#define CONFIG_H

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define PROXY_IP "YOUR_SERVER_IP"
#define PROXY_PORT 8080

// Serial Baud Rate
#define SERIAL_BAUD 115200

// I2C pins
#define I2C_SDA_PIN 2
#define I2C_SCL_PIN 1

// I2S pins for ESP-VoCat
#define I2S_MCLK_PIN 42
#define I2S_BCLK_PIN 40
#define I2S_WS_PIN   39
#define I2S_DOUT_PIN 41
#define I2S_DIN_PIN  3

// PA / Audio Power
#define POWER_AUDIO_PIN 48
#define PA_CONTROL_PIN  15

// Button
#define BUTTON_PIN 0

// Audio settings
#define SAMPLE_RATE 24000
#define CHANNELS 1

#endif
