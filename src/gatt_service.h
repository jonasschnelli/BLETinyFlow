// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#pragma once

#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include <cstdint>
#include <cstring>

class GATTService {
public:
    virtual ~GATTService() = default;
    
    // Pure virtual methods that derived services must implement
    virtual void handle_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) = 0;
    virtual void init(esp_gatt_if_t gatts_if) = 0;
    
    // Getters
    uint16_t get_app_id() const { return app_id_; }
    uint16_t get_service_handle() const { return service_handle_; }
    esp_gatt_if_t get_gatts_if() const { return gatts_if_; }
    
protected:
    GATTService(uint16_t app_id, const uint8_t* service_uuid, uint16_t num_handles)
        : app_id_(app_id), service_handle_(0), gatts_if_(ESP_GATT_IF_NONE), num_handles_(num_handles) {
        memcpy(service_uuid_, service_uuid, ESP_UUID_LEN_128);
    }
    
    // Protected setters for derived classes
    void set_service_handle(uint16_t handle) { service_handle_ = handle; }
    void set_gatts_if(esp_gatt_if_t gatts_if) { gatts_if_ = gatts_if; }
    
    // Service configuration
    uint16_t app_id_;
    uint16_t service_handle_;
    esp_gatt_if_t gatts_if_;
    uint16_t num_handles_;
    uint8_t service_uuid_[ESP_UUID_LEN_128];
};