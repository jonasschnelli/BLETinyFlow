// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

#pragma once

#include "gatt_service.h"
#include "esp_log.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"
#include <cinttypes>

/**
 * @brief ImageService - GATT Service for Image Transfer over BLE
 * 
 * This service implements the protocol specification for image transfer over BLE
 * using control and data characteristics with proper acknowledgment flow.
 * 
 * GATT Service Structure:
 * - Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * - Characteristics:
 *   └── Control (6E400002) - Bidirectional control messages (WRITE/NOTIFY)
 *   └── Data Channel 0 (6E400010) - Data transmission (WRITE_NO_RESPONSE/NOTIFY)
 * 
 * Protocol Flow:
 * 1. iOS → ESP: TRANSFER_INIT (total size, chunk size, num chunks)
 * 2. ESP → iOS: CHUNK_REQUEST (starting chunk, num chunks to send)
 * 3. iOS → ESP: Data chunks (for requested range)
 * 4. ESP → iOS: CHUNK_REQUEST (next batch) - repeat until all chunks received
 * 5. ESP → iOS: TRANSFER_COMPLETE_ACK (received size)
 * 
 * Error Handling:
 * - ESP → iOS: TRANSFER_ERROR (error code) - sent when any error occurs
 * 
 * Message Formats:
 * - Control: [Command][Seq#][Param1][Param2][Param3] (20 bytes)
 * - Data: [ChunkID][Length][Payload] (up to 512 bytes)
 */
class ImageService : public GATTService {
public:
    static constexpr uint16_t APP_ID = 0;
    static constexpr uint16_t NUM_HANDLES = 15;
    
    // Image transfer completion callback
    // IMPORTANT: User must call release_image_buffer() when done processing image_data
    // to prevent memory leaks and use-after-free conditions
    typedef void (*ImageTransferCallback)(const uint8_t* image_data, uint32_t size, bool is_valid_jpeg);
    
    // ==================== GATT CHARACTERISTIC DEFINITIONS ====================
    // Protocol-compliant characteristic UUIDs as specified in specs.md
    
    /** 
     * @brief Control Characteristic UUID (6E400002)
     * 
     * Properties: WRITE, NOTIFY
     * Permissions: WRITE
     * Purpose: Bidirectional control messages for transfer coordination
     * Max Length: 20 bytes
     * Usage: TRANSFER_INIT, CHUNK_REQUEST, TRANSFER_COMPLETE_ACK, TRANSFER_ERROR
     */
    static constexpr uint8_t CHAR_UUID_CONTROL[ESP_UUID_LEN_128] = {
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
    };
    
    /**
     * @brief Data Channel 0 Characteristic UUID (6E400010)
     * 
     * Properties: WRITE_NO_RESPONSE, NOTIFY
     * Permissions: WRITE
     * Purpose: High-throughput data transmission
     * Max Length: 512 bytes (ESP32S3 MTU support)
     * Usage: Image data chunks with header [ChunkID][Length][Payload]
     */
    static constexpr uint8_t CHAR_UUID_DATA_CHANNEL_0[ESP_UUID_LEN_128] = {
        0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5, 0x10, 0x00, 0x40, 0x6e
    };
    // ========================================================================
    
    static constexpr uint32_t MAX_TRANSFER_SIZE = (1024 * 1024);  // 1MB max transfer
    static constexpr uint16_t MAX_MTU_SIZE = 512;                    // Total MTU size
    static constexpr uint8_t ATT_HEADER_SIZE = 3;                   // BLE ATT protocol header
    static constexpr uint16_t MAX_ATT_PAYLOAD = MAX_MTU_SIZE - ATT_HEADER_SIZE; // 509 bytes
    static constexpr uint8_t CONTROL_MSG_SIZE = 20;
    static constexpr uint8_t DATA_HEADER_SIZE = 4;
    static constexpr uint16_t MAX_DATA_PAYLOAD = MAX_ATT_PAYLOAD - DATA_HEADER_SIZE; // 505 bytes
    
    // Chunk request configuration
    static constexpr uint16_t DEFAULT_CHUNKS_PER_REQUEST = 40;
    
    // Protocol Command Types
    enum class CommandType : uint8_t {
        // From iOS to ESP32
        TRANSFER_INIT = 0x01,
        
        // From ESP32 to iOS  
        CHUNK_REQUEST = 0x82,
        TRANSFER_COMPLETE_ACK = 0x83,
        TRANSFER_ERROR = 0x84
    };
    
    // Error codes for TRANSFER_ERROR command
    enum class ErrorCode : uint32_t {
        UNKNOWN_ERROR = 0x01,
        TRANSFER_TOO_LARGE = 0x02,
        CHUNK_SIZE_TOO_LARGE = 0x03,
        MEMORY_ALLOCATION_FAILED = 0x04,
        BUFFER_OVERFLOW = 0x05,
        INVALID_CHUNK_ID = 0x06,
        DUPLICATE_CHUNK = 0x07,
        CONTROL_MESSAGE_TOO_SHORT = 0x08,
        DATA_CHUNK_TOO_SHORT = 0x09,
        NOTIFICATION_SEND_FAILED = 0x0A,
        INVALID_COMMAND = 0x0B
    };
    
    // Transfer status
    enum class Status {
        IDLE = 0,
        INIT_RECEIVED = 1,
        REQUESTING_CHUNKS = 2,
        RECEIVING = 3,
        COMPLETE = 4,
        ERROR = 5
    };
    
    // Protocol Message Structures
    struct ControlMessage {
        uint8_t command;        // Changed to uint8_t for explicit 1-byte size
        uint16_t sequence_number;
        uint32_t param1;
        uint32_t param2;
        uint32_t param3;
        uint8_t reserved[5];    // Pad to 20 bytes total (1+2+4+4+4+5=20)
    } __attribute__((packed));
    
    struct DataChunkHeader {
        uint16_t chunk_id;
        uint16_t data_length;
    } __attribute__((packed));
    
    ImageService();
    ~ImageService() override;
    
    // GATTService interface
    void handle_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) override;
    void init(esp_gatt_if_t gatts_if) override;
    
    // Protocol methods
    void reset_transfer();
    Status get_status() const { return status_; }
    uint32_t get_received_size() const { return received_size_; }
    uint32_t get_total_size() const { return total_size_; }
    uint32_t get_expected_chunks() const { return expected_chunks_; }
    const uint8_t* get_image_buffer() const { return image_buffer_; }
    
    // Connection management
    void set_connection_id(uint16_t conn_id) { conn_id_ = conn_id; }
    uint16_t get_connection_id() const { return conn_id_; }
    void set_mtu(uint16_t mtu) { mtu_ = mtu; }
    uint16_t get_mtu() const { return mtu_; }
    
    // Callback management
    void set_image_transfer_callback(ImageTransferCallback callback) { image_callback_ = callback; }
    
    // Buffer management - MUST be called by user after processing image in callback
    void release_image_buffer();
    
    // Notification methods
    bool send_control_notification(const ControlMessage& msg);
    bool send_chunk_request(uint16_t start_chunk, uint16_t num_chunks);
    bool send_transfer_complete_ack(uint32_t received_size);
    bool send_transfer_error(ErrorCode error_code);
    
private:
    // Service configuration
    uint16_t control_char_handle_;
    uint16_t data_char_handle_;
    uint16_t control_notify_handle_;
    uint16_t data_notify_handle_;
    uint16_t conn_id_;
    uint16_t mtu_;
    
    // Notification state
    bool control_notifications_enabled_;
    bool data_notifications_enabled_;
    
    // Handle assignment tracking
    int char_count_;
    int descr_count_;
    
    // Characteristic creation state tracking
    enum class CharCreationState {
        WAITING_FOR_CONTROL = 0,
        WAITING_FOR_CONTROL_CCCD = 1,
        WAITING_FOR_DATA = 2,
        BOTH_CREATED = 3
    };
    CharCreationState char_creation_state_;
    
    // Protocol state
    Status status_;
    uint16_t sequence_number_;
    
    // Transfer parameters (from TRANSFER_INIT)
    uint32_t total_size_;
    uint32_t chunk_size_;
    uint32_t expected_chunks_;
    
    // Transfer state
    uint8_t* image_buffer_;
    uint32_t received_size_;
    uint16_t next_expected_chunk_;
    bool* chunk_received_map_;  // Track which chunks have been received
    
    // Chunk request state
    uint16_t current_request_start_;  // First chunk ID in current request
    uint16_t current_request_end_;    // Last chunk ID in current request
    uint16_t chunks_per_request_;     // Number of chunks to request at once
    
    // Performance optimization counters
    uint32_t total_chunks_received_;  // Fast counter instead of array iteration
    uint16_t current_batch_received_; // Fast counter for current batch
    
    // Callback for image transfer completion
    ImageTransferCallback image_callback_;
    
    // Event handlers
    void handle_reg_event(esp_ble_gatts_cb_param_t *param);
    void handle_create_event(esp_ble_gatts_cb_param_t *param);
    void handle_add_char_event(esp_ble_gatts_cb_param_t *param);
    void handle_add_char_descr_event(esp_ble_gatts_cb_param_t *param);
    void handle_write_event(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
    void handle_connect_event(esp_ble_gatts_cb_param_t *param);
    void handle_disconnect_event(esp_ble_gatts_cb_param_t *param);
    void handle_mtu_event(esp_ble_gatts_cb_param_t *param);
    
    // Protocol message handlers
    void handle_control_message(esp_gatt_if_t gatts_if, const uint8_t* data, uint16_t len);
    void handle_data_chunk(const uint8_t* data, uint16_t len);
    void handle_transfer_init(const ControlMessage& msg);
    
    // Helper methods
    bool validate_jpeg_header() const;
    uint32_t get_available_memory() const;
    bool is_transfer_complete() const;
    void create_data_characteristic();
};