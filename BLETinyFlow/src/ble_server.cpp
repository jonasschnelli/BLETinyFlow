// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#include "ble_server.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"

static const char* TAG = "BLEServer";

// Static instance for singleton pattern
BLEServer* BLEServer::instance_ = nullptr;

BLEServer::BLEServer() : initialized_(false), started_(false), local_mtu_(512), connected_count_(0) {
    instance_ = this;
}

BLEServer::~BLEServer() {
    stop();
    instance_ = nullptr;
}

esp_err_t BLEServer::init(const char* device_name) {
    if (initialized_) {
        ESP_LOGW(TAG, "BLE Server already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing BLE Server");
    
    esp_err_t ret = init_bluetooth_stack();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth stack");
        return ret;
    }
    
    ret = register_callbacks();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callbacks");
        return ret;
    }
    
    ret = register_services();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register services");
        return ret;
    }
    
    // Set local MTU
    esp_err_t mtu_ret = esp_ble_gatt_set_local_mtu(local_mtu_);
    if (mtu_ret) {
        ESP_LOGE(TAG, "set local MTU failed, error code = %x", mtu_ret);
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "BLE Server initialized successfully");
    return ESP_OK;
}

esp_err_t BLEServer::start() {
    if (!initialized_) {
        ESP_LOGE(TAG, "BLE Server not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (started_) {
        ESP_LOGW(TAG, "BLE Server already started");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting BLE Server");
    started_ = true;
    return ESP_OK;
}

esp_err_t BLEServer::stop() {
    if (!started_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping BLE Server");
    
    // Stop advertising
    advertising_manager_.stop_advertising();
    
    started_ = false;
    return ESP_OK;
}

void BLEServer::add_service(std::unique_ptr<GATTService> service) {
    services_.push_back(std::move(service));
}

GATTService* BLEServer::get_service(uint16_t app_id) {
    for (auto& service : services_) {
        if (service->get_app_id() == app_id) {
            return service.get();
        }
    }
    return nullptr;
}

esp_err_t BLEServer::restart_advertising() {
    if (!initialized_ || !started_) {
        ESP_LOGW(TAG, "Cannot restart advertising: server not initialized or started");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Restarting advertising to accept new connections");
    esp_err_t ret = advertising_manager_.start_advertising();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Advertising restarted successfully");
    } else {
        ESP_LOGE(TAG, "Failed to restart advertising: %s", esp_err_to_name(ret));
    }
    return ret;
}

// Static callback wrappers
void BLEServer::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    if (instance_) {
        instance_->handle_gatts_event(event, gatts_if, param);
    }
}

void BLEServer::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (instance_) {
        instance_->handle_gap_event(event, param);
    }
}

void BLEServer::handle_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    // Log important connection events
    switch (event) {
    case ESP_GATTS_CONNECT_EVT:
        connected_count_++;
        ESP_LOGI(TAG, "🔗 BLE client connected (conn_id: %d, total connections: %d)", 
                 param->connect.conn_id, connected_count_);
        ESP_LOGI(TAG, "Client address: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(param->connect.remote_bda));
        break;
        
    case ESP_GATTS_DISCONNECT_EVT:
        if (connected_count_ > 0) {
            connected_count_--;
        }
        ESP_LOGI(TAG, "🔌 BLE client disconnected (conn_id: %d, reason: 0x%02x, remaining connections: %d)", 
                 param->disconnect.conn_id, param->disconnect.reason, connected_count_);
        ESP_LOGI(TAG, "Client address: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(param->disconnect.remote_bda));
        
        // Automatically restart advertising when no clients are connected
        if (connected_count_ == 0) {
            ESP_LOGI(TAG, "No clients connected - will restart advertising after service cleanup");
        }
        break;
        
    default:
        break;
    }
    
    // Handle registration events first
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            // Find and initialize the corresponding service
            GATTService* service = get_service(param->reg.app_id);
            if (service) {
                service->init(gatts_if);
            }
        } else {
            ESP_LOGI(TAG, "Reg app failed, app_id %04x, status %d",
                    param->reg.app_id, param->reg.status);
            return;
        }
    }
    
    // Route events to appropriate services
    for (auto& service : services_) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == service->get_gatts_if()) {
            service->handle_event(event, gatts_if, param);
        }
    }
}

void BLEServer::handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    // Delegate to advertising manager
    advertising_manager_.handle_gap_event(event, param);
}

esp_err_t BLEServer::init_bluetooth_stack() {
    ESP_LOGI(TAG, "Initializing Bluetooth stack");
    
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "Failed to release Classic BT memory");
        return ret;
    }
    
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Initialize controller failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Enable controller failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Init bluetooth failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Enable bluetooth failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t BLEServer::register_callbacks() {
    ESP_LOGI(TAG, "Registering callbacks");
    
    esp_err_t ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gatts register error, error code = %x", ret);
        return ret;
    }
    
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gap register error, error code = %x", ret);
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t BLEServer::register_services() {
    ESP_LOGI(TAG, "Registering services");
    
    for (auto& service : services_) {
        esp_err_t ret = esp_ble_gatts_app_register(service->get_app_id());
        if (ret) {
            ESP_LOGE(TAG, "gatts app register error, error code = %x", ret);
            return ret;
        }
    }
    
    return ESP_OK;
}