#pragma once

#include "esp_gap_ble_api.h"
#include "esp_bt_defs.h"
#include <cstdint>

class AdvertisingManager {
public:
    AdvertisingManager();
    ~AdvertisingManager() = default;
    
    // Initialize advertising configuration
    void init(const char* device_name, const uint8_t* service_uuid);
    
    // Handle GAP events
    void handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
    
    // Start/stop advertising
    esp_err_t start_advertising();
    esp_err_t stop_advertising();
    
    // Check if advertising is configured
    bool is_config_done() const { return adv_config_done_ == 0; }
    
private:
    // Configuration flags
    static constexpr uint8_t ADV_CONFIG_FLAG = (1 << 0);
    static constexpr uint8_t SCAN_RSP_CONFIG_FLAG = (1 << 1);
    
    uint8_t adv_config_done_;
    esp_ble_adv_data_t adv_data_;
    esp_ble_adv_data_t scan_rsp_data_;
    esp_ble_adv_params_t adv_params_;
    uint8_t service_uuid_[ESP_UUID_LEN_128];
    
    // Helper methods
    void setup_adv_data();
    void setup_scan_rsp_data();
    void setup_adv_params();
};