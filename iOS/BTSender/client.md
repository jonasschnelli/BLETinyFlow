# BLE JPEG Transfer Client Protocol

This document describes the protocol for sending JPEG images to the ESP32 GATT server via Bluetooth Low Energy.

## Service Overview

The ESP32 implements a custom GATT service for receiving JPEG images through chunked transfers using BLE prepared writes.

### Service UUID
- **Image Service UUID**: `0x00DD`

### Characteristics

#### Image Data Characteristic
- **UUID**: `0xDD01`
- **Properties**: Write, Write Without Response
- **Permissions**: Write
- **Purpose**: Receives JPEG image data in chunks

## Transfer Protocol

### 1. Connection Setup
1. Scan for and connect to the ESP32 device (advertised as "ESP_GATTS_DEMO")
2. Discover services and locate the Image Service (`0x00DD`)
3. Discover characteristics and locate the Image Data characteristic (`0xDD01`)

### 2. MTU Negotiation
- The server supports MTU up to 500 bytes
- Negotiate the highest possible MTU to optimize transfer speed
- Default MTU is 23 bytes if negotiation fails

### 3. Image Transfer Process

#### Overview
JPEG images are transferred using BLE's **Prepared Write** mechanism, which allows sending large data in chunks and ensures atomic completion.

#### Step-by-Step Process

1. **Prepare Write Requests**
   - Split the JPEG file into chunks that fit within (MTU - 5) bytes
   - Send each chunk using `ATT_PREPARE_WRITE_REQ` to characteristic `0xDD01`
   - Include proper offset for each chunk
   - Server will respond with `ATT_PREPARE_WRITE_RSP` for each chunk

2. **Execute Write Request**
   - After all chunks are sent, send `ATT_EXECUTE_WRITE_REQ`
   - This commits the entire transfer atomically
   - Server responds with `ATT_EXECUTE_WRITE_RSP`

#### Chunk Structure
```
Chunk Size: (MTU - 5) bytes maximum
- ATT header: 1 byte
- Prepare Write header: 4 bytes (handle + offset)
- Data payload: (MTU - 5) bytes
```

#### Example Transfer Flow
```
1. ATT_PREPARE_WRITE_REQ: handle=0xDD01, offset=0, data=[first chunk]
2. ATT_PREPARE_WRITE_REQ: handle=0xDD01, offset=chunk_size, data=[second chunk]
3. ... (continue for all chunks)
4. ATT_EXECUTE_WRITE_REQ: execute_flag=0x01 (execute)
```

## Implementation Details

### Maximum File Size
- **Buffer Limit**: 64 KB (65,536 bytes)
- Files larger than 64 KB will be rejected by the server

### JPEG Validation
- Server performs basic JPEG validation by checking magic bytes
- Valid JPEG files must start with `0xFF 0xD8`
- Warning logged if data doesn't appear to be JPEG format
- Transfer still completes successfully even if not valid JPEG

### Error Handling

#### Server Error Responses
- **Size Limit Exceeded**: Transfer aborted if data exceeds 64 KB
- **Memory Allocation Failure**: Server logs error, transfer fails
- **Invalid Offset**: Standard BLE error codes returned

#### Transfer Cancellation
- Send `ATT_EXECUTE_WRITE_REQ` with execute_flag=0x00 to cancel
- Server will clean up allocated buffers

### Transfer States
The server tracks the following states:
- `IDLE` (0): No active transfer
- `RECEIVING` (1): Transfer in progress
- `COMPLETE` (2): Transfer completed successfully
- `ERROR` (3): Transfer failed

## Sample Client Implementation (Pseudocode)

```python
def send_jpeg_to_esp32(jpeg_file_path):
    # 1. Connect to device
    device = scan_and_connect("ESP_GATTS_DEMO")
    
    # 2. Discover services
    image_service = discover_service(device, "0x00DD")
    data_char = discover_characteristic(image_service, "0xDD01")
    
    # 3. Negotiate MTU
    mtu = negotiate_mtu(device, 500)
    chunk_size = mtu - 5
    
    # 4. Read JPEG file
    with open(jpeg_file_path, 'rb') as f:
        jpeg_data = f.read()
    
    if len(jpeg_data) > 65536:
        raise Exception("File too large (max 64KB)")
    
    # 5. Send chunks via prepared writes
    offset = 0
    while offset < len(jpeg_data):
        chunk = jpeg_data[offset:offset + chunk_size]
        prepare_write_request(data_char, offset, chunk)
        offset += len(chunk)
    
    # 6. Execute write to commit transfer
    execute_write_request(data_char, execute=True)
    
    print(f"Transfer complete: {len(jpeg_data)} bytes sent")
```

## Debugging and Monitoring

### Server Logs
The ESP32 provides detailed logging during transfers:
- Connection events
- Chunk reception with offset and length
- Transfer completion status
- JPEG validation results
- Error conditions

### Log Examples
```
I GATTS_DEMO: Starting image transfer
I GATTS_DEMO: Image chunk: offset 0, length 495
I GATTS_DEMO: Image chunk: offset 495, length 495
I GATTS_DEMO: Image transfer completed, total size: 12450 bytes
I GATTS_DEMO: Valid JPEG header detected
I GATTS_DEMO: Transfer complete - Status: 2, Size: 12450 bytes
```

## Testing Recommendations

### Test Cases
1. **Small JPEG** (< 1KB): Test basic functionality
2. **Medium JPEG** (10-30KB): Test chunked transfer
3. **Large JPEG** (near 64KB): Test size limits
4. **Oversized file** (> 64KB): Test error handling
5. **Non-JPEG file**: Test validation warnings
6. **Connection interruption**: Test cleanup behavior

### Tools
- **nRF Connect** (mobile app): Manual testing and debugging
- **BlueZ** (Linux): Command-line testing
- **Core Bluetooth** (iOS): Native app development
- **Android BLE APIs**: Native app development

## Notes and Limitations

- Only one image can be stored at a time (new transfers overwrite previous)
- No compression or optimization applied to JPEG data
- Transfer is not resumable if interrupted
- No progress indication characteristic available
- Server does not provide transfer status notifications to client
- Stored image is only in RAM (lost on device reset)

## Future Enhancements

Potential improvements for the protocol:
- Status characteristic for transfer progress
- Multiple image storage slots
- File metadata exchange (filename, size)
- Transfer resume capability
- Persistent storage to flash memory