// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#include "advertising.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "AdvertisingManager";

AdvertisingManager::AdvertisingManager() : adv_config_done_(ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG) {
    setup_adv_params();
}

void AdvertisingManager::init(const char* device_name, const uint8_t* service_uuid) {
    // Copy service UUID
    memcpy(service_uuid_, service_uuid, ESP_UUID_LEN_128);
    
    // Set device name
    esp_err_t ret = esp_ble_gap_set_device_name(device_name);
    if (ret) {
        ESP_LOGE(TAG, "set device name failed, error code = %x", ret);
        return;
    }
    
    // Setup and configure advertising data
    setup_adv_data();
    setup_scan_rsp_data();
    
    // Configure advertising data
    ret = esp_ble_gap_config_adv_data(&adv_data_);
    if (ret) {
        ESP_LOGE(TAG, "config adv data failed, error code = %x", ret);
        return;
    }
    adv_config_done_ &= (~ADV_CONFIG_FLAG);
    
    // Configure scan response data
    ret = esp_ble_gap_config_adv_data(&scan_rsp_data_);
    if (ret) {
        ESP_LOGE(TAG, "config scan response data failed, error code = %x", ret);
        return;
    }
    adv_config_done_ &= (~SCAN_RSP_CONFIG_FLAG);
}

void AdvertisingManager::handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done_ &= (~ADV_CONFIG_FLAG);
        if (adv_config_done_ == 0) {
            esp_ble_gap_start_advertising(&adv_params_);
        }
        break;
        
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done_ &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done_ == 0) {
            esp_ble_gap_start_advertising(&adv_params_);
        }
        break;
        
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising start successfully");
        }
        break;
        
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising stop successfully");
        }
        break;
        
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "Connection params update, status %d, conn_int %d, latency %d, timeout %d",
                 param->update_conn_params.status,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
        
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT:
        ESP_LOGI(TAG, "Packet length update, status %d, rx %d, tx %d",
                 param->pkt_data_length_cmpl.status,
                 param->pkt_data_length_cmpl.params.rx_len,
                 param->pkt_data_length_cmpl.params.tx_len);
        break;
        
    default:
        break;
    }
}

esp_err_t AdvertisingManager::start_advertising() {
    return esp_ble_gap_start_advertising(&adv_params_);
}

esp_err_t AdvertisingManager::stop_advertising() {
    return esp_ble_gap_stop_advertising();
}

void AdvertisingManager::setup_adv_data() {
    adv_data_ = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = false,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = nullptr,
        .service_data_len = 0,
        .p_service_data = nullptr,
        .service_uuid_len = ESP_UUID_LEN_128,
        .p_service_uuid = service_uuid_,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };
}

void AdvertisingManager::setup_scan_rsp_data() {
    scan_rsp_data_ = {
        .set_scan_rsp = true,
        .include_name = true,
        .include_txpower = true,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = nullptr,
        .service_data_len = 0,
        .p_service_data = nullptr,
        .service_uuid_len = ESP_UUID_LEN_128,
        .p_service_uuid = service_uuid_,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };
}

void AdvertisingManager::setup_adv_params() {
    adv_params_ = {
        .adv_int_min = 0x20,
        .adv_int_max = 0x40,
        .adv_type = ADV_TYPE_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
}