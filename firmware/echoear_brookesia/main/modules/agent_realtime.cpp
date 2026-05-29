#include "agent_realtime.hpp"
#include "esp_log.h"
#include "brookesia/lib_utils/task_scheduler.hpp"
#include "cJSON.h"
#include "esp_timer.h"

#define TAG "RealTimeAgent"

namespace esp_brookesia::agent {

RealTimeAgent::RealTimeAgent()
    : Base(
        AgentAttributes{
            .name = "RealTimeAgent",
            .operation_timeout = {
                .activate = 60000,
                .start = 10000,
                .sleep = 5000,
                .wake_up = 5000,
            },
            .support_general_functions = {},
            .support_general_events = {
                helper::Manager::AgentGeneralEvent::SpeakingStatusChanged,
                helper::Manager::AgentGeneralEvent::ListeningStatusChanged,
                helper::Manager::AgentGeneralEvent::EmoteGot
            }
        },
        AudioConfig{
            .encoder = {
                .type = service::helper::Audio::CodecFormat::PCM,
                .general = {
                    .channels = 1,
                    .sample_bits = 16,
                    .sample_rate = 24000,
                    .frame_duration = 60,
                }
            },
            .decoder = {
                .type = service::helper::Audio::CodecFormat::PCM,
                .general = {
                    .channels = 1,
                    .sample_bits = 16,
                    .sample_rate = 24000,
                    .frame_duration = 60,
                }
            }
        }
    )
{
}

RealTimeAgent::~RealTimeAgent()
{
}

bool RealTimeAgent::on_activate()
{
    ESP_LOGI(TAG, "Activating RealTimeAgent...");

    // Setup WebSocket client configuration
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = "ws://YOUR_SERVER_IP:8080";

    ws_client_ = esp_websocket_client_init(&ws_cfg);
    if (!ws_client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return false;
    }

    esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY, websocket_event_handler, this);

    trigger_general_event(GeneralEvent::Activated);
    return true;
}

bool RealTimeAgent::on_startup()
{
    ESP_LOGI(TAG, "Starting RealTimeAgent...");
    
    if (ws_client_) {
        esp_websocket_client_start(ws_client_);
    }
    
    trigger_general_event(GeneralEvent::Started);
    return true;
}

void RealTimeAgent::on_shutdown()
{
    ESP_LOGI(TAG, "Shutting down RealTimeAgent...");

    if (ws_client_) {
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
    }
    
    trigger_general_event(GeneralEvent::Stopped);
}

bool RealTimeAgent::on_sleep()
{
    ESP_LOGI(TAG, "Sleeping RealTimeAgent...");
    trigger_general_event(GeneralEvent::Slept);
    return true;
}

bool RealTimeAgent::on_wakeup()
{
    ESP_LOGI(TAG, "Waking up RealTimeAgent...");
    trigger_general_event(GeneralEvent::Awake);
    return true;
}

bool RealTimeAgent::on_encoder_data_ready(const uint8_t *data, size_t data_size)
{
    uint64_t now = esp_timer_get_time() / 1000;
    if (is_speaking() && (now - last_audio_recv_time_ > 1500)) {
        ESP_LOGI(TAG, "Speaking timeout, resetting speaking state");
        set_speaking(false);
    }

    if (is_speaking()) {
        // Discard microphone audio during playback to avoid acoustic feedback self-interruption
        return true;
    }

    if (is_ws_connected_ && ws_client_) {
        esp_websocket_client_send_bin(ws_client_, (const char *)data, data_size, portMAX_DELAY);
    }
    return true;
}

void RealTimeAgent::websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    RealTimeAgent *agent = static_cast<RealTimeAgent *>(handler_args);
    agent->on_websocket_event(event_id, event_data);
}

bool RealTimeAgent::on_websocket_event(int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        is_ws_connected_ = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        is_ws_connected_ = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x02) { // Binary payload (raw audio from server)
            if (!is_speaking()) {
                ESP_LOGI(TAG, "Server audio received, setting speaking=true");
                set_speaking(true);
            }
            last_audio_recv_time_ = esp_timer_get_time() / 1000;
            feed_audio_decoder_data((const uint8_t *)data->data_ptr, data->data_len);
        } else if (data->op_code == 0x01) { // Text payload (JSON messages)
            // parse JSON, trigger emotes, text events, etc.
            parse_server_message(data->data_ptr, data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
    return true;
}

bool RealTimeAgent::parse_server_message(const char *data, size_t len)
{
    std::string json_str(data, len);
    cJSON *root = cJSON_Parse(json_str.c_str());
    if (!root) {
        return false;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (type && type->valuestring) {
        std::string type_str(type->valuestring);
        if (type_str == "response.output_audio.done" || type_str == "response.done") {
            ESP_LOGI(TAG, "Server finished sending audio response: %s", type_str.c_str());
            set_speaking(false);
        } else if (type_str == "input_audio_buffer.speech_started") {
            ESP_LOGI(TAG, "Speech started detected by server VAD, stopping speaking state");
            set_speaking(false);
        }
    }
    cJSON_Delete(root);
    return true;
}

BROOKESIA_PLUGIN_REGISTER_SINGLETON(
    Base, RealTimeAgent, RealTimeAgent::get_instance().get_attributes().get_name(), RealTimeAgent::get_instance()
);
BROOKESIA_PLUGIN_REGISTER_SINGLETON(
    service::ServiceBase, RealTimeAgent, RealTimeAgent::get_instance().get_attributes().get_name(), RealTimeAgent::get_instance()
);

} // namespace esp_brookesia::agent
