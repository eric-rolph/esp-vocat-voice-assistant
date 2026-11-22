#pragma once

#include "brookesia/agent_manager/base.hpp"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include <string>

namespace esp_brookesia::agent {

class RealTimeAgent: public Base {
public:
    static RealTimeAgent &get_instance()
    {
        static RealTimeAgent instance;
        return instance;
    }

    void reset_speaking_state() {
        set_speaking(false);
        set_listening(true);
    }

    RealTimeAgent();
    ~RealTimeAgent();

protected:
    bool on_activate() override;
    bool on_startup() override;
    void on_shutdown() override;
    bool on_sleep() override;
    bool on_wakeup() override;
    bool on_encoder_data_ready(const uint8_t *data, size_t data_size) override;

private:
    static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    bool on_websocket_event(int32_t event_id, void *event_data);
    bool parse_server_message(const char *data, size_t len);
    bool send_session_update();
    bool encode_and_send_audio(const uint8_t *data, size_t len);

    esp_websocket_client_handle_t ws_client_ = nullptr;
    bool is_ws_connected_ = false;
    bool session_configured_ = false;
    uint64_t last_audio_recv_time_ = 0;

    // Reusable buffers allocated in PSRAM to avoid fragmentation
    char *base64_buf_ = nullptr;       // For Base64 encoding outgoing audio
    size_t base64_buf_size_ = 0;
    uint8_t *decode_buf_ = nullptr;    // For Base64 decoding incoming audio
    size_t decode_buf_size_ = 0;
    char *json_send_buf_ = nullptr;    // For building JSON messages to send
    size_t json_send_buf_size_ = 0;

    // For accumulating fragmented WebSocket text frames
    std::string json_accumulator_;
    bool receiving_text_frame_ = false;

    // Authorization header string (built once at activation, must outlive ws_client_)
    std::string ws_uri_;
    std::string auth_header_;
    uint64_t start_playback_time_ = 0;
    uint64_t total_audio_bytes_ = 0;

    esp_timer_handle_t playback_timer_ = nullptr;
};

} // namespace esp_brookesia::agent
