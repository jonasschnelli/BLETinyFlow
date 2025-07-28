/****************************************************************************
*
* ESP_BLE_SERVER - ESP32 BLE GATT Server for Image Transfer
* 
* This application provides a BLE GATT server that can receive image data
* via Bluetooth Low Energy. It uses an object-oriented design to make it
* easy to extend with additional services and characteristics.
*
****************************************************************************/

#include <cstdio>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

// BLE Server components
#include <ble_server.h>
#include <image_service.h>

#define TAG "ESP_BLE_SERVER"

// Device configuration
static const char* DEVICE_NAME = "ESP_BLE_SERVER";

// Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const uint8_t service_uuid[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
};

// Global reference to image service for callback cleanup
static ImageService* g_image_service = nullptr;

// Image transfer completion callback function
void on_image_transfer_complete(const uint8_t* image_data, uint32_t size, bool is_valid_jpeg) {
    ESP_LOGI(TAG, "=== IMAGE TRANSFER COMPLETED ===");
    ESP_LOGI(TAG, "Received image: %lu bytes", size);
    ESP_LOGI(TAG, "JPEG validation: %s", is_valid_jpeg ? "VALID" : "INVALID");
    
    // Memory monitoring - before processing
    uint32_t free_heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t free_internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_spiram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    
    ESP_LOGI(TAG, "📊 MEMORY STATUS BEFORE PROCESSING:");
    ESP_LOGI(TAG, "  Free heap (total): %lu bytes (%.1f KB)", free_heap_before, free_heap_before / 1024.0f);
    ESP_LOGI(TAG, "  Free internal RAM: %lu bytes (%.1f KB)", free_internal_before, free_internal_before / 1024.0f);
    ESP_LOGI(TAG, "  Free SPIRAM: %lu bytes (%.1f KB)", free_spiram_before, free_spiram_before / 1024.0f);
    ESP_LOGI(TAG, "  Minimum free heap ever: %lu bytes (%.1f KB)", min_free_heap, min_free_heap / 1024.0f);
    
    if (image_data && size > 0) {
        // Log first few bytes for verification
        ESP_LOGI(TAG, "First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                 image_data[0], image_data[1], image_data[2], image_data[3],
                 image_data[4], image_data[5], image_data[6], image_data[7]);
        
        // Here you can add custom image processing logic:
        // - Save to file system (SPIFFS/LittleFS)
        // - Process the image data
        // - Forward to another system component
        // - Analyze image properties
        
        ESP_LOGI(TAG, "Image processing completed successfully");
    } else {
        ESP_LOGW(TAG, "Invalid image data received");
    }
    
    // CRITICAL: Release the image buffer to prevent memory leaks and use-after-free
    if (g_image_service) {
        g_image_service->release_image_buffer();
        ESP_LOGI(TAG, "Image buffer released");
    } else {
        ESP_LOGE(TAG, "Cannot release image buffer - service reference not available");
    }
    
    // Memory monitoring - after buffer release
    uint32_t free_heap_after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t free_internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_spiram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free_heap_final = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    
    ESP_LOGI(TAG, "📊 MEMORY STATUS AFTER CLEANUP:");
    ESP_LOGI(TAG, "  Free heap (total): %lu bytes (%.1f KB)", free_heap_after, free_heap_after / 1024.0f);
    ESP_LOGI(TAG, "  Free internal RAM: %lu bytes (%.1f KB)", free_internal_after, free_internal_after / 1024.0f);
    ESP_LOGI(TAG, "  Free SPIRAM: %lu bytes (%.1f KB)", free_spiram_after, free_spiram_after / 1024.0f);
    ESP_LOGI(TAG, "  Minimum free heap ever: %lu bytes (%.1f KB)", min_free_heap_final, min_free_heap_final / 1024.0f);
    
    // Memory difference analysis
    int32_t heap_diff = (int32_t)free_heap_after - (int32_t)free_heap_before;
    int32_t internal_diff = (int32_t)free_internal_after - (int32_t)free_internal_before;
    int32_t spiram_diff = (int32_t)free_spiram_after - (int32_t)free_spiram_before;
    
    ESP_LOGI(TAG, "📈 MEMORY CHANGE ANALYSIS:");
    ESP_LOGI(TAG, "  Heap difference: %ld bytes (%.1f KB) %s", 
             heap_diff, heap_diff / 1024.0f, heap_diff >= 0 ? "✅ RECOVERED" : "❌ LEAKED");
    ESP_LOGI(TAG, "  Internal RAM difference: %ld bytes (%.1f KB) %s", 
             internal_diff, internal_diff / 1024.0f, internal_diff >= 0 ? "✅" : "❌");
    ESP_LOGI(TAG, "  SPIRAM difference: %ld bytes (%.1f KB) %s", 
             spiram_diff, spiram_diff / 1024.0f, spiram_diff >= 0 ? "✅" : "❌");
    
    // Expected memory recovery (image buffer size should be freed)
    if (heap_diff >= (int32_t)size * 0.9) {  // Allow 10% tolerance
        ESP_LOGI(TAG, "✅ MEMORY LEAK CHECK PASSED: Expected ~%lu bytes recovered, got %ld bytes", size, heap_diff);
    } else {
        ESP_LOGW(TAG, "⚠️  POTENTIAL MEMORY LEAK: Expected ~%lu bytes recovered, only got %ld bytes", size, heap_diff);
    }
    
    ESP_LOGI(TAG, "=== IMAGE CALLBACK FINISHED ===");
}

extern "C" {
    void app_main(void);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP_BLE_SERVER Application");
    
    //esp_log_level_set("ImageService",ESP_LOG_NONE); 

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Check for CI pipeline configuration
    #if CONFIG_EXAMPLE_CI_PIPELINE_ID
    // Note: This would need to be adapted for the new structure
    // if CI pipeline naming is required
    #endif

    // Create and configure BLE server
    BLEServer ble_server;
    
    // Add image service
    auto image_service = std::make_unique<ImageService>();
    
    // Store global reference for callback cleanup (before moving ownership)
    g_image_service = image_service.get();
    
    // Register image transfer completion callback
    image_service->set_image_transfer_callback(on_image_transfer_complete);
    ESP_LOGI(TAG, "Image transfer callback registered");
    
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
    
    // Main application loop - could be extended for additional functionality
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Optional: Log current status periodically
        // This could be expanded to monitor transfer status, memory usage, etc.
    }
}