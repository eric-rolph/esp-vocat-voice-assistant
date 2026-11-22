#include "agent_realtime.hpp"
#include "esp_log.h"
#include "brookesia/lib_utils/task_scheduler.hpp"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include <cstring>

#define TAG "RealTimeAgent"

// Buffer sizes for Base64 encode/decode (allocated in PSRAM)
// Audio frame: 24kHz * 16bit * 1ch * 60ms = 2880 bytes raw
// Base64 expands 3:4, so 2880 -> 3840 chars + null
// JSON overhead: ~80 chars for {"type":"input_audio_buffer.append","audio":"..."}
#define AUDIO_FRAME_MAX_SIZE   4096
#define BASE64_BUF_SIZE        (AUDIO_FRAME_MAX_SIZE * 4 / 3 + 4)
#define DECODE_BUF_SIZE        32768   // Incoming audio deltas can be large (e.g. 16k chars)
#define JSON_SEND_BUF_SIZE     (BASE64_BUF_SIZE + 128)  // Base64 + JSON wrapper
#define JSON_ACCUMULATOR_MAX_SIZE (128 * 1024)          // Guard against unbounded WS text accumulation
#define WS_AUDIO_SEND_TIMEOUT_MS  50
#define WS_CTRL_SEND_TIMEOUT_MS   2000

namespace esp_brookesia::agent {

namespace {
template <typename T>
auto ws_event_is_fin(const T *d, int) -> decltype(d->fin, bool()) {
    return d->fin;
}
template <typename T>
bool ws_event_is_fin(const T *, long) {
    return true;
}
} // namespace

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
    if (base64_buf_) { heap_caps_free(base64_buf_); base64_buf_ = nullptr; }
    if (decode_buf_) { heap_caps_free(decode_buf_); decode_buf_ = nullptr; }
    if (json_send_buf_) { heap_caps_free(json_send_buf_); json_send_buf_ = nullptr; }
    if (playback_timer_) {
        esp_timer_stop(playback_timer_);
        esp_timer_delete(playback_timer_);
        playback_timer_ = nullptr;
    }
}

bool RealTimeAgent::on_activate()
{
    ESP_LOGI(TAG, "Activating RealTimeAgent (direct OpenAI connection)...");

    // Allocate reusable buffers in PSRAM to avoid heap fragmentation
    base64_buf_size_ = BASE64_BUF_SIZE;
    base64_buf_ = (char *)heap_caps_malloc(base64_buf_size_, MALLOC_CAP_SPIRAM);
    if (!base64_buf_) {
        ESP_LOGE(TAG, "Failed to allocate base64 encode buffer in PSRAM");
        return false;
    }

    decode_buf_size_ = DECODE_BUF_SIZE;
    decode_buf_ = (uint8_t *)heap_caps_malloc(decode_buf_size_, MALLOC_CAP_SPIRAM);
    if (!decode_buf_) {
        ESP_LOGE(TAG, "Failed to allocate base64 decode buffer in PSRAM");
        return false;
    }

    json_send_buf_size_ = JSON_SEND_BUF_SIZE;
    json_send_buf_ = (char *)heap_caps_malloc(json_send_buf_size_, MALLOC_CAP_SPIRAM);
    if (!json_send_buf_) {
        ESP_LOGE(TAG, "Failed to allocate JSON send buffer in PSRAM");
        return false;
    }

    // Build the WebSocket URI with model parameter (stored as member for lifetime)
    ws_uri_ = "wss://api.openai.com/v1/realtime?model=";
#ifdef CONFIG_EXAMPLE_AGENTS_REALTIME_MODEL
    ws_uri_ += CONFIG_EXAMPLE_AGENTS_REALTIME_MODEL;
#else
    ws_uri_ += "gpt-4o-realtime-preview";
#endif

    // Build authorization headers (stored as member for lifetime)
    auth_header_ = "Authorization: Bearer ";
#ifdef CONFIG_EXAMPLE_AGENTS_REALTIME_API_KEY
    auth_header_ += CONFIG_EXAMPLE_AGENTS_REALTIME_API_KEY;
#else
    auth_header_ += "sk-proj-xxx";
#endif
    auth_header_ += "\r\n";

    ESP_LOGI(TAG, "Connecting to: %s", ws_uri_.c_str());
    ESP_LOGI(TAG, "Free heap: %lu, PSRAM: %lu",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Setup WebSocket client configuration for direct TLS connection
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = ws_uri_.c_str();
    ws_cfg.headers = auth_header_.c_str();
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.buffer_size = 8192;          // Larger buffer for TLS + JSON payloads
    ws_cfg.task_stack = 16384;          // TLS handshake needs ~12-16KB stack
    ws_cfg.network_timeout_ms = 30000;  // 30s timeout for TLS handshake
    ws_cfg.reconnect_timeout_ms = 5000; // Retry after 5s on disconnect

    ws_client_ = esp_websocket_client_init(&ws_cfg);
    if (!ws_client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return false;
    }

    esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY, websocket_event_handler, this);

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            RealTimeAgent* agent = (RealTimeAgent*)arg;
            ESP_LOGI(TAG, "Playback timeout, resetting speaking state");
            agent->reset_speaking_state();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "playback_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &playback_timer_);

    trigger_general_event(GeneralEvent::Activated);
    return true;
}

bool RealTimeAgent::on_startup()
{
    ESP_LOGI(TAG, "Starting RealTimeAgent...");
    
    if (ws_client_) {
        esp_websocket_client_start(ws_client_);
    }
    
    set_listening(true);
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
    
    session_configured_ = false;
    is_ws_connected_ = false;

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

bool RealTimeAgent::send_session_update()
{
    if (!is_ws_connected_ || !ws_client_) {
        return false;
    }

    const char *voice = "alloy";
    const char *instructions = "You are a helpful assistant on a small IoT device. Keep responses concise.";
#ifdef CONFIG_EXAMPLE_AGENTS_REALTIME_VOICE
    voice = CONFIG_EXAMPLE_AGENTS_REALTIME_VOICE;
#endif
#ifdef CONFIG_EXAMPLE_AGENTS_REALTIME_SYSTEM_PROMPT
    instructions = CONFIG_EXAMPLE_AGENTS_REALTIME_SYSTEM_PROMPT;
#endif

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");

    cJSON *session = cJSON_CreateObject();
    cJSON_AddStringToObject(session, "type", "realtime");  // Required for GA API
    cJSON_AddStringToObject(session, "instructions", instructions);

    // Audio configuration object
    cJSON *audio = cJSON_CreateObject();

    // audio.input
    cJSON *input = cJSON_CreateObject();
    cJSON *input_format = cJSON_CreateObject();
    cJSON_AddStringToObject(input_format, "type", "audio/pcm");
    cJSON_AddNumberToObject(input_format, "rate", 24000);
    cJSON_AddItemToObject(input, "format", input_format);

    cJSON *turn_detection = cJSON_CreateObject();
    cJSON_AddStringToObject(turn_detection, "type", "server_vad");
    cJSON_AddNumberToObject(turn_detection, "threshold", 0.5);
    cJSON_AddNumberToObject(turn_detection, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(turn_detection, "silence_duration_ms", 500);
    cJSON_AddItemToObject(input, "turn_detection", turn_detection);
    cJSON_AddItemToObject(audio, "input", input);

    // audio.output
    cJSON *output = cJSON_CreateObject();
    cJSON *output_format = cJSON_CreateObject();
    cJSON_AddStringToObject(output_format, "type", "audio/pcm");
    cJSON_AddNumberToObject(output_format, "rate", 24000);
    cJSON_AddItemToObject(output, "format", output_format);
    cJSON_AddStringToObject(output, "voice", voice);
    cJSON_AddItemToObject(audio, "output", output);

    cJSON_AddItemToObject(session, "audio", audio);

    cJSON_AddItemToObject(root, "session", session);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "Sending session.update (%d bytes)", (int)strlen(json_str));
        esp_websocket_client_send_text(ws_client_, json_str, strlen(json_str), pdMS_TO_TICKS(WS_CTRL_SEND_TIMEOUT_MS));
        cJSON_free(json_str);
    }
    cJSON_Delete(root);

    session_configured_ = true;
    return true;
}

bool RealTimeAgent::encode_and_send_audio(const uint8_t *data, size_t len)
{
    if (!is_ws_connected_ || !ws_client_ || !session_configured_) {
        // Silently drop — don't log here to avoid flooding the console
        return false;
    }

    // Base64 encode the raw PCM data
    size_t b64_len = 0;
    int ret = mbedtls_base64_encode(
        (unsigned char *)base64_buf_, base64_buf_size_, &b64_len,
        data, len
    );
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", ret);
        return false;
    }

    // Build JSON: {"type":"input_audio_buffer.append","audio":"<base64>"}
    int json_len = snprintf(json_send_buf_, json_send_buf_size_,
        "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
        (int)b64_len, base64_buf_);

    if (json_len <= 0 || (size_t)json_len >= json_send_buf_size_) {
        ESP_LOGE(TAG, "JSON buffer overflow: %d >= %d", json_len, (int)json_send_buf_size_);
        return false;
    }

    int sent = esp_websocket_client_send_text(
        ws_client_, json_send_buf_, json_len, pdMS_TO_TICKS(WS_AUDIO_SEND_TIMEOUT_MS)
    );
    return sent >= 0;
}

bool RealTimeAgent::on_encoder_data_ready(const uint8_t *data, size_t data_size)
{
    if (is_speaking()) {
        // Discard microphone audio during playback to avoid acoustic feedback self-interruption
        return true;
    }

    // Base64 encode and send as JSON to OpenAI directly
    encode_and_send_audio(data, data_size);
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
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED to OpenAI Realtime API");
        is_ws_connected_ = true;
        json_accumulator_.clear();
        receiving_text_frame_ = false;
        // Send session configuration immediately after connection
        send_session_update();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        is_ws_connected_ = false;
        session_configured_ = false;
        json_accumulator_.clear();
        receiving_text_frame_ = false;
        if (playback_timer_) {
            esp_timer_stop(playback_timer_);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 || (data->op_code == 0x00 && receiving_text_frame_)) {
            // Start of a new text message resets any stale partial buffer
            if (data->op_code == 0x01 && data->payload_offset == 0) {
                json_accumulator_.clear();
                receiving_text_frame_ = true;
            }
            // Accumulate fragmented text frames with an upper memory bound
            if (data->data_ptr && data->data_len > 0) {
                if (json_accumulator_.size() + (size_t)data->data_len > JSON_ACCUMULATOR_MAX_SIZE) {
                    ESP_LOGW(TAG, "Dropping oversized WS text message (>%d bytes)", JSON_ACCUMULATOR_MAX_SIZE);
                    json_accumulator_.clear();
                    receiving_text_frame_ = false;
                    break;
                }
                json_accumulator_.append(data->data_ptr, data->data_len);
            }
            // Process once the final fragment of the message has arrived
            bool frame_complete = (data->payload_offset + data->data_len >= data->payload_len);
            if (frame_complete && ws_event_is_fin(data, 0)) {
                parse_server_message(json_accumulator_.c_str(), json_accumulator_.size());
                json_accumulator_.clear();
                receiving_text_frame_ = false;
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR: {
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        if (data) {
            ESP_LOGE(TAG, "  error_type: %d", data->error_handle.error_type);
            ESP_LOGE(TAG, "  esp_tls_last_esp_err: 0x%x", data->error_handle.esp_tls_last_esp_err);
            ESP_LOGE(TAG, "  esp_tls_stack_err: 0x%x", data->error_handle.esp_tls_stack_err);
            ESP_LOGE(TAG, "  esp_tls_cert_verify_flags: 0x%x", data->error_handle.esp_tls_cert_verify_flags);
            ESP_LOGE(TAG, "  esp_transport_sock_errno: %d", data->error_handle.esp_transport_sock_errno);
            ESP_LOGE(TAG, "  handshake_status_code: %d", data->error_handle.esp_ws_handshake_status_code);
        }
        break;
    }
    }
    return true;
}

bool RealTimeAgent::parse_server_message(const char *data, size_t len)
{
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON (%d bytes)", (int)len);
        return false;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !type->valuestring) {
        cJSON_Delete(root);
        return false;
    }

    std::string type_str(type->valuestring);

    if (type_str == "session.created" || type_str == "session.updated") {
        ESP_LOGI(TAG, "Session event: %s", type_str.c_str());
    }
    else if (type_str == "response.audio.delta" || type_str == "response.output_audio.delta") {
        // Decode Base64 audio delta and feed to the audio decoder
        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta && delta->valuestring) {
            size_t b64_input_len = strlen(delta->valuestring);
            size_t decoded_len = 0;

            int ret = mbedtls_base64_decode(
                decode_buf_, decode_buf_size_, &decoded_len,
                (const unsigned char *)delta->valuestring, b64_input_len
            );
            if (ret == 0 && decoded_len > 0) {
                if (!is_speaking()) {
                    ESP_LOGI(TAG, "First audio delta received, setting speaking=true");
                    set_listening(false);
                    set_speaking(true);
                    total_audio_bytes_ = 0;
                    start_playback_time_ = esp_timer_get_time() / 1000;
                }
                total_audio_bytes_ += decoded_len;
                last_audio_recv_time_ = esp_timer_get_time() / 1000;
                feed_audio_decoder_data(decode_buf_, decoded_len);
            } else {
                ESP_LOGE(TAG, "Base64 decode failed: ret=%d, input_len=%d", ret, (int)b64_input_len);
            }
        }
    }
    else if (type_str == "response.audio.done" || type_str == "response.output_audio.done") {
        ESP_LOGI(TAG, "Response audio stream finished");
        // Don't immediately stop speaking — let the last audio chunk play out
    }
    else if (type_str == "response.done") {
        uint64_t current_time = esp_timer_get_time() / 1000;
        uint64_t total_duration_ms = (total_audio_bytes_ * 1000ULL) / (24000ULL * 2ULL);
        uint64_t elapsed_ms = (current_time >= start_playback_time_) ? (current_time - start_playback_time_) : 0;
        
        uint64_t remaining_ms = (total_duration_ms > elapsed_ms) ? (total_duration_ms - elapsed_ms) : 0;
        
        // Add 500ms padding
        uint64_t timer_delay_ms = remaining_ms + 500ULL;
        
        ESP_LOGI(TAG, "Response complete, duration: %llums, elapsed: %llums, remaining: %llums", 
                 (unsigned long long)total_duration_ms, (unsigned long long)elapsed_ms, (unsigned long long)remaining_ms);
                 
        if (playback_timer_) {
            esp_timer_stop(playback_timer_);
            esp_timer_start_once(playback_timer_, timer_delay_ms * 1000ULL);
        }
    }
    else if (type_str == "input_audio_buffer.speech_started") {
        ESP_LOGI(TAG, "User speech detected by server VAD");
        if (playback_timer_) esp_timer_stop(playback_timer_);
        if (is_speaking()) {
            set_speaking(false);
        }
        set_listening(true);
    }
    else if (type_str == "input_audio_buffer.speech_stopped") {
        ESP_LOGI(TAG, "User speech ended (server VAD)");
    }
    else if (type_str == "error") {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (error) {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            if (msg && msg->valuestring) {
                ESP_LOGE(TAG, "OpenAI error: %s", msg->valuestring);
            }
        }
    }
    else {
        // Log other event types at debug level
        ESP_LOGD(TAG, "Event: %s", type_str.c_str());
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
