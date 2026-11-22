#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#define I2C_PORT I2C_NUM_0
#define I2S_PORT I2S_NUM_0

#define I2C_SDA_PIN 2
#define I2C_SCL_PIN 1

#define I2S_MCLK_PIN 42
#define I2S_BCLK_PIN 40
#define I2S_WS_PIN   39
#define I2S_DOUT_PIN 41
#define I2S_DIN_PIN  3

#define POWER_AUDIO_PIN 48
#define PA_CONTROL_PIN  15

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WS_URI    "ws://YOUR_SERVER_IP:8080/realtime"

static const char *TAG = "ECHOEAR_PROXY";

static esp_websocket_client_handle_t client;
static i2s_chan_handle_t rx_chan;
static i2s_chan_handle_t tx_chan;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        // Handle incoming data
        if (data->op_code == 0x02) { // Binary frame
            size_t bytes_written = 0;
            if (tx_chan) {
                i2s_channel_write(tx_chan, data->data_ptr, data->data_len, &bytes_written, portMAX_DELAY);
            }
        }
        break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying connection to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        // Start WebSocket
        esp_websocket_client_start(client);
    }
}

static void audio_task(void *pvParameters)
{
    uint8_t buffer[1024];
    while (1) {
        if (esp_websocket_client_is_connected(client)) {
            size_t bytes_read = 0;
            if (i2s_channel_read(rx_chan, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
                if (bytes_read > 0) {
                    esp_websocket_client_send_bin(client, (const char *)buffer, bytes_read, portMAX_DELAY);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Turn on audio power
    gpio_set_direction(POWER_AUDIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_AUDIO_PIN, 1);
    
    // PA control
    gpio_set_direction(PA_CONTROL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CONTROL_PIN, 1);

    // Initialize Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize I2C
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_PORT, &i2c_conf);
    i2c_driver_install(I2C_PORT, i2c_conf.mode, 0, 0, 0);

    // Initialize I2S
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_init_std_mode(rx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
    i2s_channel_enable(rx_chan);

    // Initialize ES8311 & ES7210
    audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&chan_cfg);
    audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(I2C_PORT, 0x18); // ES8311 addr
    
    // We would need the exact setup for codec, but right now let's just make it compile.
    
    // Initialize WebSocket
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = WS_URI;
    client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);
}
