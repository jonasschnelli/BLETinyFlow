// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#include "image_service.h"
#include <cstring>
#include <cstdlib>
#include "esp_heap_caps.h"
#include "ble_server.h"

// Uncomment for detailed chunk logging (impacts performance)
// #define CHUNK_LOGGING

#ifdef CHUNK_LOGGING
    #define CHUNK_LOG(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
    #define CHUNK_LOG(tag, format, ...) do {} while(0)
#endif

static const char* TAG = "ImageService";

// 128-bit UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static uint8_t service_uuid_image[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
};

ImageService::ImageService() 
    : GATTService(APP_ID, service_uuid_image, NUM_HANDLES),
      control_char_handle_(0), data_char_handle_(0), 
      control_notify_handle_(0), data_notify_handle_(0),
      conn_id_(0), mtu_(23),
      control_notifications_enabled_(false), data_notifications_enabled_(false),
      char_count_(0), descr_count_(0), char_creation_state_(CharCreationState::WAITING_FOR_CONTROL),
      status_(Status::IDLE), sequence_number_(0),
      total_size_(0), chunk_size_(0), expected_chunks_(0),
      image_buffer_(nullptr), received_size_(0), next_expected_chunk_(0),
      chunk_received_map_(nullptr),
      current_request_start_(0), current_request_end_(0), chunks_per_request_(DEFAULT_CHUNKS_PER_REQUEST),
      total_chunks_received_(0), current_batch_received_(0),
      image_callback_(nullptr) {
}

ImageService::~ImageService() {
    reset_transfer();
}

void ImageService::init(esp_gatt_if_t gatts_if) {
    set_gatts_if(gatts_if);
}

void ImageService::handle_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        handle_reg_event(param);
        break;
    case ESP_GATTS_CREATE_EVT:
        handle_create_event(param);
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        handle_add_char_event(param);
        break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        handle_add_char_descr_event(param);
        break;
    case ESP_GATTS_WRITE_EVT:
        handle_write_event(gatts_if, param);
        break;
    case ESP_GATTS_CONNECT_EVT:
        handle_connect_event(param);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        handle_disconnect_event(param);
        break;
    case ESP_GATTS_MTU_EVT:
        handle_mtu_event(param);
        break;
    default:
        break;
    }
}

void ImageService::release_image_buffer() {
    if (image_buffer_) {
        ESP_LOGI(TAG, "Releasing image buffer (%lu bytes)", total_size_);
        free(image_buffer_);
        image_buffer_ = nullptr;
    } else {
        ESP_LOGW(TAG, "Image buffer already released or never allocated");
    }
}

void ImageService::reset_transfer() {
    // Release image buffer (safe to call multiple times)
    release_image_buffer();
    
    if (chunk_received_map_) {
        free(chunk_received_map_);
        chunk_received_map_ = nullptr;
    }
    
    total_size_ = 0;
    chunk_size_ = 0;
    expected_chunks_ = 0;
    received_size_ = 0;
    next_expected_chunk_ = 0;
    current_request_start_ = 0;
    current_request_end_ = 0;
    total_chunks_received_ = 0;
    current_batch_received_ = 0;
    status_ = Status::IDLE;
    
    ESP_LOGI(TAG, "Image transfer reset");
}

void ImageService::handle_reg_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "Image service register, status %d, app_id %d, gatts_if %d", 
             param->reg.status, param->reg.app_id, get_gatts_if());
    
    // Reset handle assignment counters for new service registration
    char_count_ = 0;
    descr_count_ = 0;
    char_creation_state_ = CharCreationState::WAITING_FOR_CONTROL;
    control_char_handle_ = 0;
    data_char_handle_ = 0;
    control_notify_handle_ = 0;
    data_notify_handle_ = 0;
    // Note: Keep conn_id_ if already connected
    
    ESP_LOGI(TAG, "Service handles reset for new registration");
    
    // Create service
    esp_gatt_srvc_id_t service_id;
    service_id.is_primary = true;
    service_id.id.inst_id = 0x00;
    service_id.id.uuid.len = ESP_UUID_LEN_128;
    memcpy(service_id.id.uuid.uuid.uuid128, service_uuid_, ESP_UUID_LEN_128);
    
    esp_ble_gatts_create_service(get_gatts_if(), &service_id, NUM_HANDLES);
}

void ImageService::handle_create_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "=== IMAGE SERVICE CREATION START ===");
    ESP_LOGI(TAG, "Service create event: status=%d, service_handle=%d", 
             param->create.status, param->create.service_handle);
    
    if (param->create.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "CRITICAL: Failed to create service, status: %d", param->create.status);
        return;
    }
    
    set_service_handle(param->create.service_handle);
    
    // Start service
    ESP_LOGI(TAG, "Starting GATT service...");
    esp_err_t start_ret = esp_ble_gatts_start_service(get_service_handle());
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start service: %s", esp_err_to_name(start_ret));
        return;
    }
    ESP_LOGI(TAG, "GATT service started successfully");
    
    // ==================== PROTOCOL CHARACTERISTIC CREATION ====================
    
    /**
     * Create Control Characteristic (6E400002)
     * 
     * Bidirectional control messages for transfer coordination.
     * - UUID: CHAR_UUID_CONTROL - 128-bit UUID from specs
     * - Properties: WRITE | NOTIFY (bidirectional communication)
     * - Permissions: WRITE 
     * - Usage: TRANSFER_INIT, TRANSFER_READY, TRANSFER_COMPLETE_ACK
     */
    ESP_LOGI(TAG, "Creating CONTROL characteristic...");
    esp_bt_uuid_t control_uuid;
    control_uuid.len = ESP_UUID_LEN_128;
    memcpy(control_uuid.uuid.uuid128, CHAR_UUID_CONTROL, ESP_UUID_LEN_128);
    
    esp_gatt_char_prop_t control_props = ESP_GATT_CHAR_PROP_BIT_WRITE | 
                                        ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    
    ESP_LOGI(TAG, "Control char props: WRITE(0x%02X) | NOTIFY(0x%02X) = 0x%02X", 
             ESP_GATT_CHAR_PROP_BIT_WRITE, ESP_GATT_CHAR_PROP_BIT_NOTIFY, control_props);
    
    // Configure attribute value with proper maximum length for control messages
    esp_attr_value_t control_attr_val = {
        .attr_max_len = CONTROL_MSG_SIZE,  // 20 bytes for control messages
        .attr_len = 0,                     // Initial length is 0
        .attr_value = nullptr              // No initial value
    };
    
    ESP_LOGI(TAG, "Control characteristic attr_max_len set to: %d bytes", CONTROL_MSG_SIZE);
    
    // Set state to indicate we're waiting for control characteristic to be added
    char_creation_state_ = CharCreationState::WAITING_FOR_CONTROL;
    
    esp_err_t ret = esp_ble_gatts_add_char(get_service_handle(), &control_uuid,
                                          ESP_GATT_PERM_WRITE,
                                          control_props,
                                          &control_attr_val, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CRITICAL: Failed to add control characteristic: %s (0x%x)", esp_err_to_name(ret), ret);
        return;
    }
    ESP_LOGI(TAG, "Control characteristic creation initiated successfully");
    
    // NOTE: Data characteristic will be created sequentially after control CCCD is ready
    ESP_LOGI(TAG, "Control characteristic creation initiated - data characteristic will be created after CCCD");
    ESP_LOGI(TAG, "=== SERVICE CREATION COMPLETE ===");
    // ====================================================================
}

void ImageService::handle_add_char_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "=== CHARACTERISTIC ADD EVENT ===");
    ESP_LOGI(TAG, "Char add event: status=%d, handle=%d, service_handle=%d",
             param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
    
    if (param->add_char.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "CRITICAL: Failed to add characteristic, status: %d", param->add_char.status);
        return;
    }
    
    /**
     * Robust characteristic identification using creation state tracking
     * 
     * Instead of relying on creation order (which can cause race conditions),
     * we track the expected characteristic based on our creation state.
     */
    char_count_++;
    ESP_LOGI(TAG, "Characteristic successfully added - count: %d, state: %d", 
             char_count_, static_cast<int>(char_creation_state_));
    
    if (char_creation_state_ == CharCreationState::WAITING_FOR_CONTROL) {
        // This should be the control characteristic
        control_char_handle_ = param->add_char.attr_handle;
        char_creation_state_ = CharCreationState::WAITING_FOR_CONTROL_CCCD;
        ESP_LOGI(TAG, "✅ Control characteristic ready - handle: %d", control_char_handle_);
        
        // Create CCCD descriptor immediately for control characteristic
        ESP_LOGI(TAG, "Creating CCCD descriptor for control characteristic...");
        
        esp_bt_uuid_t notify_descr_uuid;
        notify_descr_uuid.len = ESP_UUID_LEN_16;
        notify_descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        
        // CCCD initial value (notifications disabled)
        uint8_t cccd_value[2] = {0x00, 0x00};
        esp_attr_value_t cccd_val = {
            .attr_max_len = 2,
            .attr_len = 2,
            .attr_value = cccd_value
        };
        
        ESP_LOGI(TAG, "Adding CCCD descriptor for control characteristic (handle %d)...", control_char_handle_);
        esp_err_t ret = esp_ble_gatts_add_char_descr(get_service_handle(), &notify_descr_uuid,
                                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                   &cccd_val, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CRITICAL: Failed to add control CCCD descriptor: %s (0x%x)", esp_err_to_name(ret), ret);
        } else {
            ESP_LOGI(TAG, "CCCD descriptor creation initiated - waiting for add event...");
        }
        
    } else if (char_creation_state_ == CharCreationState::WAITING_FOR_DATA) {
        // This should be the data characteristic
        data_char_handle_ = param->add_char.attr_handle;
        char_creation_state_ = CharCreationState::BOTH_CREATED;
        ESP_LOGI(TAG, "✅ Data characteristic ready - handle: %d", data_char_handle_);
        ESP_LOGI(TAG, "✅ Both characteristics created successfully");
        
    } else {
        ESP_LOGW(TAG, "Unexpected characteristic add event in state: %d (count: %d)", 
                 static_cast<int>(char_creation_state_), char_count_);
    }
}

void ImageService::handle_add_char_descr_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "CCCD descriptor add event: status=%d, handle=%d, service_handle=%d",
             param->add_char_descr.status, param->add_char_descr.attr_handle, param->add_char_descr.service_handle);
    
    if (param->add_char_descr.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "CRITICAL: Failed to add CCCD descriptor, status: %d (%s)", 
                 param->add_char_descr.status, 
                 param->add_char_descr.status == ESP_GATT_NO_RESOURCES ? "NO_RESOURCES" : "UNKNOWN");
        return;
    }
    
    /**
     * Store descriptor handles based on creation order
     * Currently only creating control CCCD descriptor
     */
    descr_count_++;
    ESP_LOGI(TAG, "CCCD descriptor successfully created - count: %d", descr_count_);
    
    if (descr_count_ == 1 && char_creation_state_ == CharCreationState::WAITING_FOR_CONTROL_CCCD) {
        control_notify_handle_ = param->add_char_descr.attr_handle;
        ESP_LOGI(TAG, "✅ Control CCCD descriptor ready - handle: %d", control_notify_handle_);
        ESP_LOGI(TAG, "Control characteristic setup complete - now creating data characteristic");
        
        // Now that control characteristic and its CCCD are ready, create data characteristic
        create_data_characteristic();
        
    } else {
        ESP_LOGW(TAG, "Unexpected descriptor event: count=%d, state=%d", 
                 descr_count_, static_cast<int>(char_creation_state_));
    }
}

void ImageService::handle_write_event(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "Write event: conn_id %d, handle %d, len %d", 
             param->write.conn_id, param->write.handle, param->write.len);
    
    ESP_LOGI(TAG, "Handle comparison: control_char=%d, data_char=%d, control_notify=%d, data_notify=%d",
             control_char_handle_, data_char_handle_, control_notify_handle_, data_notify_handle_);
    
    // Route to appropriate handler based on handle
    if (param->write.handle == control_char_handle_) {
        ESP_LOGI(TAG, "Control message received");
        handle_control_message(gatts_if, param->write.value, param->write.len);
    } else if (param->write.handle == data_char_handle_) {
        ESP_LOGI(TAG, "Data chunk received");
        handle_data_chunk(param->write.value, param->write.len);
    } else if (param->write.handle == control_notify_handle_) {
        ESP_LOGI(TAG, "Control notification descriptor write");
        if (param->write.len == 2) {
            uint16_t notify_value = param->write.value[0] | (param->write.value[1] << 8);
            control_notifications_enabled_ = (notify_value & 0x0001) != 0;
            ESP_LOGI(TAG, "Control notifications %s", 
                     control_notifications_enabled_ ? "enabled" : "disabled");
        } else {
            ESP_LOGW(TAG, "Invalid descriptor write length: %d", param->write.len);
        }
    } else if (param->write.handle == data_notify_handle_) {
        ESP_LOGI(TAG, "Data notification descriptor write");
        if (param->write.len == 2) {
            uint16_t notify_value = param->write.value[0] | (param->write.value[1] << 8);
            data_notifications_enabled_ = (notify_value & 0x0001) != 0;
            ESP_LOGI(TAG, "Data notifications %s", 
                     data_notifications_enabled_ ? "enabled" : "disabled");
        } else {
            ESP_LOGW(TAG, "Invalid descriptor write length: %d", param->write.len);
        }
    } else {
        ESP_LOGW(TAG, "Write to unknown handle: %d (expected: char=%d,%d or descr=%d,%d)", 
                 param->write.handle, control_char_handle_, data_char_handle_,
                 control_notify_handle_, data_notify_handle_);
    }
    
    // Send response if needed
    if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, 
                                  ESP_GATT_OK, nullptr);
    }
}


void ImageService::handle_connect_event(esp_ble_gatts_cb_param_t *param) {
    esp_ble_conn_update_params_t conn_params = {};
    memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
    conn_params.latency = 0;
    conn_params.min_int = 0x06;    // 6 * 1.25ms = 7.5ms
    conn_params.max_int = 0x0C;    // 12 * 1.25ms = 15ms
    conn_params.timeout = 400;     // timeout = 400*10ms = 4000ms
    
    ESP_LOGI(TAG, "Image service connected, conn_id %d, remote " ESP_BD_ADDR_STR "",
             param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));
    
    conn_id_ = param->connect.conn_id;
    ESP_LOGI(TAG, "Connection ID assigned: %d", conn_id_);
    esp_ble_gap_update_conn_params(&conn_params);
}

void ImageService::handle_disconnect_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "Image service disconnected, remote " ESP_BD_ADDR_STR ", reason 0x%02x",
             ESP_BD_ADDR_HEX(param->disconnect.remote_bda), param->disconnect.reason);
    reset_transfer(); // Clean up any ongoing transfer
    mtu_ = 23; // Reset MTU for next connection
    
    // Reset notification state
    control_notifications_enabled_ = false;
    data_notifications_enabled_ = false;
    
    // Restart advertising to allow new connections
    BLEServer* server = BLEServer::get_instance();
    if (server) {
        ESP_LOGI(TAG, "Requesting server to restart advertising for new connections");
        esp_err_t ret = server->restart_advertising();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restart advertising: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Cannot restart advertising: BLEServer instance not available");
    }
}

void ImageService::handle_mtu_event(esp_ble_gatts_cb_param_t *param) {
    ESP_LOGI(TAG, "MTU exchange, MTU %d", param->mtu.mtu);
    mtu_ = param->mtu.mtu;
}


bool ImageService::validate_jpeg_header() const {
    return received_size_ >= 2 && image_buffer_[0] == 0xFF && image_buffer_[1] == 0xD8;
}

// ==================== PROTOCOL IMPLEMENTATION ====================

void ImageService::handle_control_message(esp_gatt_if_t gatts_if, const uint8_t* data, uint16_t len) {
    if (len < sizeof(ControlMessage)) {
        ESP_LOGE(TAG, "Control message too short: %d bytes", len);
        send_transfer_error(ErrorCode::CONTROL_MESSAGE_TOO_SHORT);
        return;
    }
    
    const ControlMessage* msg = reinterpret_cast<const ControlMessage*>(data);
    ESP_LOGI(TAG, "Control message: cmd=0x%02X, seq=%d, p1=%lu, p2=%lu, p3=%lu",
             msg->command, msg->sequence_number, 
             msg->param1, msg->param2, msg->param3);
    
    switch (msg->command) {
        case static_cast<uint8_t>(CommandType::TRANSFER_INIT):
            handle_transfer_init(*msg);
            break;
        default:
            ESP_LOGW(TAG, "Unknown control command: 0x%02X", msg->command);
            send_transfer_error(ErrorCode::INVALID_COMMAND);
            break;
    }
}

void ImageService::handle_transfer_init(const ControlMessage& msg) {
    ESP_LOGI(TAG, "TRANSFER_INIT: size=%lu, chunk_size=%lu, chunks=%lu", 
             msg.param1, msg.param2, msg.param3);
    
    // Validate parameters
    if (msg.param1 > MAX_TRANSFER_SIZE) {
        ESP_LOGE(TAG, "Transfer too large: %lu bytes (max: %lu)", msg.param1, MAX_TRANSFER_SIZE);
        send_transfer_error(ErrorCode::TRANSFER_TOO_LARGE);
        status_ = Status::ERROR;
        return;
    }
    
    if (msg.param2 > MAX_MTU_SIZE - DATA_HEADER_SIZE) {
        ESP_LOGE(TAG, "Chunk size too large: %lu bytes", msg.param2);
        send_transfer_error(ErrorCode::CHUNK_SIZE_TOO_LARGE);
        status_ = Status::ERROR;
        return;
    }
    
    // Reset any previous transfer
    reset_transfer();
    
    // Store transfer parameters
    total_size_ = msg.param1;
    chunk_size_ = msg.param2;
    expected_chunks_ = msg.param3;
    
    // Allocate buffer for image data
    image_buffer_ = static_cast<uint8_t*>(malloc(total_size_));
    if (!image_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate %lu bytes for image buffer", total_size_);
        send_transfer_error(ErrorCode::MEMORY_ALLOCATION_FAILED);
        status_ = Status::ERROR;
        return;
    }
    
    // Allocate chunk tracking map
    chunk_received_map_ = static_cast<bool*>(calloc(expected_chunks_, sizeof(bool)));
    if (!chunk_received_map_) {
        ESP_LOGE(TAG, "Failed to allocate chunk tracking map");
        free(image_buffer_);
        image_buffer_ = nullptr;
        send_transfer_error(ErrorCode::MEMORY_ALLOCATION_FAILED);
        status_ = Status::ERROR;
        return;
    }
    
    status_ = Status::INIT_RECEIVED;
    
    // Immediately send first chunk request (no TRANSFER_READY per specs)
    uint16_t first_request_size = (expected_chunks_ < chunks_per_request_) ? 
                                expected_chunks_ : chunks_per_request_;
    
    CHUNK_LOG(TAG, "Sending first chunk request for %d chunks (of %lu total)", 
             first_request_size, expected_chunks_);
    
    if (send_chunk_request(0, first_request_size)) {
        // Minimal always-on logging: first chunk request sent
        ESP_LOGI(TAG, "CHUNK_REQUEST sent: chunks 0-%d", first_request_size - 1);
    } else {
        ESP_LOGE(TAG, "❌ Failed to send first chunk request");
        send_transfer_error(ErrorCode::NOTIFICATION_SEND_FAILED);
        status_ = Status::ERROR;
    }
}

void ImageService::handle_data_chunk(const uint8_t* data, uint16_t len) {
    if (status_ != Status::REQUESTING_CHUNKS && status_ != Status::RECEIVING) {
        ESP_LOGW(TAG, "Data chunk received in wrong state: %d", static_cast<int>(status_));
        return;
    }
    
    // Comprehensive size logging for debugging (conditional)
    CHUNK_LOG(TAG, "=== DATA CHUNK RECEIVED ===");
    CHUNK_LOG(TAG, "Total received length: %d bytes", len);
    CHUNK_LOG(TAG, "Transfer state: %s", 
             (status_ == Status::REQUESTING_CHUNKS) ? "REQUESTING_CHUNKS" : "RECEIVING");
    CHUNK_LOG(TAG, "Current request range: %d-%d", current_request_start_, current_request_end_);
    CHUNK_LOG(TAG, "Current negotiated MTU: %d bytes", mtu_);
    CHUNK_LOG(TAG, "ATT header overhead: %d bytes", ATT_HEADER_SIZE);
    CHUNK_LOG(TAG, "Expected ATT payload: %d bytes (MTU - ATT header)", MAX_ATT_PAYLOAD);
    CHUNK_LOG(TAG, "Data header size: %d bytes", DATA_HEADER_SIZE);
    CHUNK_LOG(TAG, "Maximum data payload: %d bytes", MAX_DATA_PAYLOAD);
    CHUNK_LOG(TAG, "Actual received vs expected ATT payload: %d vs %d (%s)", 
             len, MAX_ATT_PAYLOAD, (len == MAX_ATT_PAYLOAD) ? "MATCH" : "MISMATCH");
    CHUNK_LOG(TAG, "Payload size after removing data header: %d bytes", len - DATA_HEADER_SIZE);
    
    if (len < DATA_HEADER_SIZE) {
        ESP_LOGE(TAG, "Data chunk too short: %d bytes (minimum: %d)", len, DATA_HEADER_SIZE);
        send_transfer_error(ErrorCode::DATA_CHUNK_TOO_SHORT);
        return;
    }
    
    const DataChunkHeader* header = reinterpret_cast<const DataChunkHeader*>(data);
    uint16_t chunk_id = header->chunk_id;
    uint16_t data_length = header->data_length;  // This should be payload size only
    
    CHUNK_LOG(TAG, "Header - Chunk ID: %d", chunk_id);
    CHUNK_LOG(TAG, "Header - Data Length: %d bytes (payload only)", data_length);
    CHUNK_LOG(TAG, "Actual payload size: %d bytes", len - DATA_HEADER_SIZE);
    
    // Validate chunk parameters
    if (chunk_id >= expected_chunks_) {
        ESP_LOGE(TAG, "❌ INVALID CHUNK ID: %d (max: %lu)", chunk_id, expected_chunks_ - 1);
        send_transfer_error(ErrorCode::INVALID_CHUNK_ID);
        return;
    }
    
    // Check if chunk is within current request range (conditional detailed logging)
    if (chunk_id < current_request_start_ || chunk_id > current_request_end_) {
        CHUNK_LOG(TAG, "⚠️ Chunk %d is outside current request range [%d-%d]", 
                 chunk_id, current_request_start_, current_request_end_);
        CHUNK_LOG(TAG, "This might indicate out-of-order delivery or client error");
    } else {
        CHUNK_LOG(TAG, "✅ Chunk %d is within current request range [%d-%d]", 
                 chunk_id, current_request_start_, current_request_end_);
    }
    
    // The data_length field should match the actual payload size
    uint16_t actual_payload_size = len - DATA_HEADER_SIZE;
    
    // Size validation (conditional detailed logging)
    if (data_length != actual_payload_size) {
        CHUNK_LOG(TAG, "Data length mismatch: header=%d, actual_payload=%d, total_len=%d", 
                 data_length, actual_payload_size, len);
        CHUNK_LOG(TAG, "Expected: header.data_length == (total_len - %d)", DATA_HEADER_SIZE);
        CHUNK_LOG(TAG, "iOS may need to adjust data_length to match actual payload sent");
        
        // For now, use the actual received payload size to avoid blocking data
        CHUNK_LOG(TAG, "Using actual payload size (%d) instead of header value (%d)", 
                 actual_payload_size, data_length);
        data_length = actual_payload_size;
    }
    
    CHUNK_LOG(TAG, "✅ Size validation completed - using payload size: %d bytes", data_length);
    
    if (chunk_received_map_[chunk_id]) {
        CHUNK_LOG(TAG, "🔄 DUPLICATE CHUNK: %d (already received)", chunk_id);
        CHUNK_LOG(TAG, "This might indicate retransmission or client error");
        send_transfer_error(ErrorCode::DUPLICATE_CHUNK);
        return;
    }
    
    // Minimal always-on logging: single line per chunk received
    ESP_LOGI(TAG, "Chunk %d received", chunk_id);
    
    // Calculate offset in buffer
    uint32_t offset = chunk_id * chunk_size_;
    if (offset + data_length > total_size_) {
        ESP_LOGE(TAG, "❌ BUFFER OVERFLOW: chunk %d would exceed buffer", chunk_id);
        ESP_LOGE(TAG, "Offset: %lu, data_length: %d, total_size: %lu", 
                 offset, data_length, total_size_);
        send_transfer_error(ErrorCode::BUFFER_OVERFLOW);
        return;
    }
    
    CHUNK_LOG(TAG, "💾 Writing chunk %d to buffer offset %lu (%d bytes)", 
             chunk_id, offset, data_length);
    
    // Copy data to buffer
    memcpy(image_buffer_ + offset, data + DATA_HEADER_SIZE, data_length);
    chunk_received_map_[chunk_id] = true;
    received_size_ += data_length;
    
    // Performance optimization: increment counters instead of iterating arrays
    total_chunks_received_++;
    if (chunk_id >= current_request_start_ && chunk_id <= current_request_end_) {
        current_batch_received_++;
    }
    
    CHUNK_LOG(TAG, "✅ Chunk %d stored successfully. Total received: %lu bytes", 
             chunk_id, received_size_);
    
    status_ = Status::RECEIVING;
    
    // Fast transfer completion check using counter
    if (total_chunks_received_ >= expected_chunks_) {
        ESP_LOGI(TAG, "🎉 TRANSFER COMPLETE! Received all %lu chunks (%lu bytes)", 
                 expected_chunks_, received_size_);
        
        // Validate JPEG header
        if (validate_jpeg_header()) {
            ESP_LOGI(TAG, "✅ Valid JPEG header detected");
        } else {
            ESP_LOGW(TAG, "⚠️ Warning: Data does not appear to be JPEG format");
        }
        
        status_ = Status::COMPLETE;
        
        // Send completion acknowledgment
        if (send_transfer_complete_ack(received_size_)) {
            ESP_LOGI(TAG, "✅ Transfer complete ACK sent");
        } else {
            ESP_LOGE(TAG, "❌ Failed to send TRANSFER_COMPLETE_ACK");
        }
        
        // Invoke callback if registered
        if (image_callback_) {
            bool is_valid_jpeg = validate_jpeg_header();
            ESP_LOGI(TAG, "🔄 Invoking image transfer callback with %lu bytes", received_size_);
            image_callback_(image_buffer_, received_size_, is_valid_jpeg);
            ESP_LOGI(TAG, "✅ Image transfer callback completed");
        } else {
            ESP_LOGI(TAG, "ℹ️ No image transfer callback registered");
        }
        
        // Disconnect client after successful transfer to allow new connections
        ESP_LOGI(TAG, "🔌 Disconnecting client after successful transfer to allow new connections");
        esp_err_t disconnect_ret = esp_ble_gatts_close(get_gatts_if(), conn_id_);
        if (disconnect_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Client disconnect initiated successfully");
        } else {
            ESP_LOGE(TAG, "❌ Failed to disconnect client: %s", esp_err_to_name(disconnect_ret));
        }
    }
    else {
        // Fast batch completion check using counter
        uint16_t expected_batch_size = (current_request_end_ >= expected_chunks_) ? 
                                     (expected_chunks_ - current_request_start_) : 
                                     (current_request_end_ - current_request_start_ + 1);
        
        bool current_batch_complete = (current_batch_received_ >= expected_batch_size);
        
#ifdef CHUNK_LOGGING
        // Detailed batch progress reporting (now using fast counters)
        CHUNK_LOG(TAG, "=== BATCH PROGRESS ===");
        CHUNK_LOG(TAG, "Current batch: chunks %d-%d (%d chunks)", 
                 current_request_start_, current_request_end_, expected_batch_size);
        CHUNK_LOG(TAG, "Current batch progress: %d/%d received", 
                 current_batch_received_, expected_batch_size);
        CHUNK_LOG(TAG, "Current batch complete: %s", current_batch_complete ? "YES" : "NO");
        
        // Report overall progress (now using fast counter - no loop!)
        CHUNK_LOG(TAG, "Overall progress: %lu/%lu chunks (%.1f%%)", 
                 total_chunks_received_, expected_chunks_, 
                 (float)total_chunks_received_ / expected_chunks_ * 100.0f);
#endif
        
        // If current batch is complete, request next batch
        if (current_batch_complete && current_request_end_ + 1 < expected_chunks_) {
            uint16_t next_start = current_request_end_ + 1;
            uint16_t remaining_chunks = expected_chunks_ - next_start;
            uint16_t next_request_size = (remaining_chunks < chunks_per_request_) ? 
                                       remaining_chunks : chunks_per_request_;
            
            CHUNK_LOG(TAG, "🔄 Current batch complete, requesting next batch...");
            CHUNK_LOG(TAG, "Next request: chunks %d-%d (%d chunks)", 
                     next_start, next_start + next_request_size - 1, next_request_size);
            
            if (send_chunk_request(next_start, next_request_size)) {
                // Minimal always-on logging: next chunk request sent
                ESP_LOGI(TAG, "CHUNK_REQUEST sent: chunks %d-%d", next_start, next_start + next_request_size - 1);
            } else {
                ESP_LOGE(TAG, "❌ Failed to send next chunk request");
                send_transfer_error(ErrorCode::NOTIFICATION_SEND_FAILED);
                status_ = Status::ERROR;
            }
        }
    }
}

bool ImageService::send_control_notification(const ControlMessage& msg) {
    ESP_LOGI(TAG, "Attempting to send control notification: handle=%d, conn_id=%d, enabled=%d",
             control_char_handle_, conn_id_, control_notifications_enabled_);
    
    if (control_char_handle_ == 0) {
        ESP_LOGE(TAG, "Cannot send notification: control characteristic handle not set");
        return false;
    }
    
    if (!control_notifications_enabled_) {
        ESP_LOGW(TAG, "Cannot send notification: notifications not enabled by client (control_notify_handle_=%d)", 
                 control_notify_handle_);
        return false;
    }
    
    ESP_LOGI(TAG, "Sending control notification: cmd=0x%02X, seq=%d", 
             static_cast<uint8_t>(msg.command), msg.sequence_number);
    
    esp_err_t ret = esp_ble_gatts_send_indicate(get_gatts_if(), conn_id_, control_char_handle_,
                                              sizeof(msg), const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&msg)),
                                              false);  // Use notification, not indication
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send control notification: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "Control notification sent successfully");
    return true;
}


bool ImageService::send_chunk_request(uint16_t start_chunk, uint16_t num_chunks) {
    CHUNK_LOG(TAG, "=== CHUNK REQUEST ===");
    CHUNK_LOG(TAG, "Requesting chunks %d to %d (%d chunks total)", 
             start_chunk, start_chunk + num_chunks - 1, num_chunks);
    CHUNK_LOG(TAG, "Expected total chunks: %lu", expected_chunks_);
    
    ControlMessage msg = {};
    msg.command = static_cast<uint8_t>(CommandType::CHUNK_REQUEST);
    msg.sequence_number = ++sequence_number_;
    msg.param1 = (uint32_t)start_chunk;  // Explicit cast to ensure clean 32-bit value
    msg.param2 = (uint32_t)num_chunks;   // Explicit cast to ensure clean 32-bit value  
    msg.param3 = 0;
    
#ifdef CHUNK_LOGGING
    // Debug: Log the exact bytes being sent (conditional - performance intensive)
    CHUNK_LOG(TAG, "📤 CHUNK_REQUEST message details:");
    CHUNK_LOG(TAG, "   command: 0x%02X", static_cast<uint8_t>(msg.command));
    CHUNK_LOG(TAG, "   sequence: %d", msg.sequence_number);
    CHUNK_LOG(TAG, "   param1 (start_chunk): %lu (0x%08lX)", msg.param1, msg.param1);
    CHUNK_LOG(TAG, "   param2 (num_chunks): %lu (0x%08lX)", msg.param2, msg.param2);
    CHUNK_LOG(TAG, "   param3: %lu (0x%08lX)", msg.param3, msg.param3);
    
    // Debug: Dump the actual message bytes (very expensive - loop with formatting)
    uint8_t* msg_bytes = (uint8_t*)&msg;
    CHUNK_LOG(TAG, "📋 Raw message bytes (%d bytes):", sizeof(msg));
    for (size_t i = 0; i < sizeof(msg); i += 4) {
        if (i + 3 < sizeof(msg)) {
            CHUNK_LOG(TAG, "   [%02d-%02d]: 0x%02X 0x%02X 0x%02X 0x%02X", 
                     i, i+3, msg_bytes[i], msg_bytes[i+1], msg_bytes[i+2], msg_bytes[i+3]);
        } else {
            // Handle remaining bytes
            CHUNK_LOG(TAG, "   [%02d+]: remaining bytes", i);
        }
    }
#endif
    
    // Update request tracking
    current_request_start_ = start_chunk;
    current_request_end_ = start_chunk + num_chunks - 1;
    current_batch_received_ = 0;  // Reset batch counter for new request
    
    CHUNK_LOG(TAG, "Current request range: %d - %d", current_request_start_, current_request_end_);
    
    bool success = send_control_notification(msg);
    if (success) {
        status_ = Status::REQUESTING_CHUNKS;
        CHUNK_LOG(TAG, "✅ Chunk request sent successfully");
    } else {
        ESP_LOGE(TAG, "❌ Failed to send chunk request");
    }
    
    return success;
}

bool ImageService::send_transfer_complete_ack(uint32_t received_size) {
    ControlMessage msg = {};
    msg.command = static_cast<uint8_t>(CommandType::TRANSFER_COMPLETE_ACK);
    msg.sequence_number = ++sequence_number_;
    msg.param1 = received_size;
    msg.param2 = 0;
    msg.param3 = 0;
    
    return send_control_notification(msg);
}

bool ImageService::send_transfer_error(ErrorCode error_code) {
    ControlMessage msg = {};
    msg.command = static_cast<uint8_t>(CommandType::TRANSFER_ERROR);
    msg.sequence_number = ++sequence_number_;
    msg.param1 = static_cast<uint32_t>(error_code);
    msg.param2 = 0;
    msg.param3 = 0;
    
    ESP_LOGE(TAG, "Sending TRANSFER_ERROR: code=0x%02X", static_cast<uint32_t>(error_code));
    
    return send_control_notification(msg);
}

uint32_t ImageService::get_available_memory() const {
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

bool ImageService::is_transfer_complete() const {
    // Fast implementation using counter instead of array iteration
    return (total_chunks_received_ >= expected_chunks_) && (expected_chunks_ > 0);
}

void ImageService::create_data_characteristic() {
    /**
     * Create Data Channel 0 Characteristic (6E400010)
     * 
     * High-throughput data transmission for image chunks.
     * - UUID: CHAR_UUID_DATA_CHANNEL_0 - 128-bit UUID from specs
     * - Properties: WRITE_NO_RESPONSE | NOTIFY (high-speed transfer)
     * - Permissions: WRITE
     * - Usage: Image data chunks with [ChunkID][Length][Payload] format
     */
    ESP_LOGI(TAG, "Creating DATA characteristic...");
    esp_bt_uuid_t data_uuid;
    data_uuid.len = ESP_UUID_LEN_128;
    memcpy(data_uuid.uuid.uuid128, CHAR_UUID_DATA_CHANNEL_0, ESP_UUID_LEN_128);
    
    esp_gatt_char_prop_t data_props = ESP_GATT_CHAR_PROP_BIT_WRITE_NR | 
                                     ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    
    ESP_LOGI(TAG, "Data char props: WRITE_NR(0x%02X) | NOTIFY(0x%02X) = 0x%02X", 
             ESP_GATT_CHAR_PROP_BIT_WRITE_NR, ESP_GATT_CHAR_PROP_BIT_NOTIFY, data_props);
    
    // Configure attribute value with proper maximum length for ATT payload
    esp_attr_value_t attr_val = {
        .attr_max_len = MAX_ATT_PAYLOAD,  // 509 bytes (512 MTU - 3 ATT header)
        .attr_len = 0,                    // Initial length is 0
        .attr_value = nullptr             // No initial value
    };
    
    ESP_LOGI(TAG, "Data characteristic attr_max_len set to: %d bytes (MTU %d - ATT header %d)", 
             MAX_ATT_PAYLOAD, MAX_MTU_SIZE, ATT_HEADER_SIZE);
    
    // Update state to indicate we're waiting for data characteristic
    char_creation_state_ = CharCreationState::WAITING_FOR_DATA;
    
    esp_err_t ret = esp_ble_gatts_add_char(get_service_handle(), &data_uuid,
                                          ESP_GATT_PERM_WRITE,
                                          data_props,
                                          &attr_val, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CRITICAL: Failed to add data characteristic: %s (0x%x)", esp_err_to_name(ret), ret);
        return;
    }
    ESP_LOGI(TAG, "Data characteristic creation initiated successfully");
}