// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#pragma once

#include "gatt_service.h"
#include "advertising.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"
#include <vector>
#include <memory>

/* ######### SAMPLE CODE

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create and configure BLE server
    BLEServer ble_server;
    
    // Add image service
    auto image_service = std::make_unique<ImageService>();
    ble_server.add_service(std::move(image_service));
    
    // Initialize BLE server
    ret = ble_server.init(DEVICE_NAME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE server: %s", esp_err_to_name(ret));
        return;
    }
    
    // Initialize advertising with service UUID
    ble_server.get_advertising_manager().init(DEVICE_NAME, service_uuid);
    
    // Start the server
    ret = ble_server.start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE server: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "BLE server started successfully");
    ESP_LOGI(TAG, "Device name: %s", DEVICE_NAME);
    ESP_LOGI(TAG, "Ready to accept image transfers via BLE");

*/
class BLEServer {
public:
    BLEServer();
    ~BLEServer();
    
    // Initialization and lifecycle
    esp_err_t init(const char* device_name);
    esp_err_t start();
    esp_err_t stop();
    
    // Service management
    void add_service(std::unique_ptr<GATTService> service);
    GATTService* get_service(uint16_t app_id);
    
    // Advertising
    AdvertisingManager& get_advertising_manager() { return advertising_manager_; }
    
    // Event handlers (static callbacks for ESP-IDF)
    static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
    static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
    
    // Singleton access
    static BLEServer* get_instance() { return instance_; }
    
private:
    static BLEServer* instance_;
    
    std::vector<std::unique_ptr<GATTService>> services_;
    AdvertisingManager advertising_manager_;
    bool initialized_;
    bool started_;
    uint16_t local_mtu_;
    
    // Internal event handling
    void handle_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
    void handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
    
    // Initialization helpers
    esp_err_t init_bluetooth_stack();
    esp_err_t register_callbacks();
    esp_err_t register_services();
};