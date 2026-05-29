#pragma once

#include "brookesia/agent_manager/base.hpp"
#include "esp_websocket_client.h"

namespace esp_brookesia::agent {

class RealTimeAgent: public Base {
public:
    static RealTimeAgent &get_instance()
    {
        static RealTimeAgent instance;
        return instance;
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

    esp_websocket_client_handle_t ws_client_ = nullptr;
    bool is_ws_connected_ = false;
    uint64_t last_audio_recv_time_ = 0;
};

} // namespace esp_brookesia::agent
