# BLETinyFlow Protocol

**Author:** Jonas Schnelli  
**License:** MIT  
**Version:** 0.1 Beta

⚠️ **WARNING: This protocol is currently under heavy development and is unstable. Breaking changes may occur without notice.**

## Overview

BLETinyFlow is a Bluetooth Low Energy (BLE) protocol designed for efficient data transfer between iOS devices and ESP32 microcontrollers. The protocol provides a robust, chunk-based data transmission system optimized for BLE's MTU limitations.

## BLE Service Architecture

### Primary Service
- **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **Description**: Main service for BLETinyFlow data transfer operations

### Characteristics

#### Control Characteristic
- **UUID**: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- **Properties**: WRITE, NOTIFY
- **Maximum Length**: 20 bytes
- **Purpose**: Command/control messages and status updates between devices

#### Data Characteristic
- **UUID**: `6E400010-B5A3-F393-E0A9-E50E24DCCA9E`
- **Properties**: WRITE_NO_RESPONSE, NOTIFY
- **Maximum Length**: 509 bytes (512-byte MTU minus 3-byte ATT header)
- **Purpose**: High-throughput data transmission

**Design Rationale**: BLETinyFlow uses a single data channel architecture rather than multiple parallel channels. While multi-channel protocols might theoretically offer higher throughput, extensive testing on ESP32 microcontrollers shows negligible performance improvements due to the device's single-core BLE stack processing and limited memory bandwidth. The added complexity of managing multiple channels, synchronization overhead, and increased memory footprint outweigh any marginal gains. A single channel simplifies implementation, reduces resource usage, and maintains optimal performance for the target hardware platform.

## Protocol Specification

### MTU Considerations
The protocol is designed around standard BLE MTU limitations:
- **Standard MTU**: 512 bytes
- **ATT Header Overhead**: 3 bytes
- **Available Payload**: 509 bytes per packet

### Message Formats

#### Control Messages (≤20 bytes)
```
Offset | Size    | Field           | Type     | Description
-------|---------|-----------------|----------|------------------
0      | 1 byte  | Command Type    | uint8_t  | Command identifier
1-2    | 2 bytes | Sequence Number | uint16_t | Message sequence
3-6    | 4 bytes | Parameter 1     | uint32_t | Command parameter
7-10   | 4 bytes | Parameter 2     | uint32_t | Command parameter
11-14  | 4 bytes | Parameter 3     | uint32_t | Command parameter
15-19  | 5 bytes | Reserved        | -        | Future use
```

#### Data Messages (≤509 bytes)
```
Offset | Size      | Field        | Type     | Description
-------|-----------|--------------|----------|------------------
0-1    | 2 bytes   | Chunk ID     | uint16_t | Unique chunk identifier
2-3    | 2 bytes   | Data Length  | uint16_t | Payload size in this packet
4+     | Variable  | Data Payload | bytes    | Actual data (max 505 bytes)
```

## Command Reference

### Version Bit Extension Mechanism

**Future Compatibility**: The most significant bit (bit 7) of the Command Type field is reserved as a version upgrade bit. Current protocol version 1.0 uses commands with this bit cleared (0). Future protocol versions may set this bit (1) to indicate extended command formats or alternative message structures.

```
Command Type Byte Structure:
Bit 7    | Bits 6-0
---------|----------
Version  | Command ID
0        | Version 1.0 commands (current)
1        | Future version commands (undefined)
```

This design provides a clear upgrade path while maintaining backward compatibility detection.

### Version 1.0 Commands

#### Client Commands (iOS → ESP32)

##### TRANSFER_INIT (0x01)
Initiates a new data transfer session.
- **Parameter 1**: Total file size in bytes
- **Parameter 2**: Chunk size (typically 505 bytes)
- **Parameter 3**: Total number of chunks

#### Server Commands (ESP32 → iOS)

##### CHUNK_REQUEST (0x82)
Requests specific data chunks from the client.
- **Parameter 1**: Starting chunk ID
- **Parameter 2**: Number of chunks requested
- **Parameter 3**: Reserved (0x00000000)

##### TRANSFER_COMPLETE_ACK (0x83)
Acknowledges successful transfer completion.
- **Parameter 1**: Total bytes received
- **Parameter 2**: Reserved (0x00000000)
- **Parameter 3**: Reserved (0x00000000)

##### TRANSFER_ERROR (0x84)
Reports transfer errors to the client.
- **Parameter 1**: Error code (see Error Codes section)
- **Parameter 2**: Additional error information
- **Parameter 3**: Reserved (0x00000000)

## Transfer Flow

### Standard Transfer Sequence

1. **Initialization**
   - Client sends `TRANSFER_INIT` on Control Characteristic
   - Includes total file size, chunk size, and chunk count

2. **Chunk Request Loop**
   - Server sends `CHUNK_REQUEST` for batch of chunks (default: 40)
   - Client responds with requested data packets on Data Characteristic
   - Server processes chunks and requests next batch
   - Repeat until all chunks received

3. **Completion**
   - Server sends `TRANSFER_COMPLETE_ACK` with total bytes received
   - Transfer session ends

### Error Handling
- Server sends `TRANSFER_ERROR` for any protocol violations or processing errors
- Client should abort transfer and may retry after error resolution

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x01 | UNKNOWN_ERROR | Unspecified error occurred |
| 0x02 | TRANSFER_TOO_LARGE | Transfer size exceeds maximum allowed (1MB) |
| 0x03 | CHUNK_SIZE_TOO_LARGE | Chunk size exceeds MTU limitations |
| 0x04 | MEMORY_ALLOCATION_FAILED | Insufficient memory to allocate transfer buffer |
| 0x05 | BUFFER_OVERFLOW | Data write would exceed allocated buffer |
| 0x06 | INVALID_CHUNK_ID | Chunk ID is out of expected range |
| 0x07 | DUPLICATE_CHUNK | Chunk with this ID already received |
| 0x08 | CONTROL_MESSAGE_TOO_SHORT | Control message shorter than expected 20 bytes |
| 0x09 | DATA_CHUNK_TOO_SHORT | Data chunk shorter than minimum header size |
| 0x0A | NOTIFICATION_SEND_FAILED | Failed to send notification to client |
| 0x0B | INVALID_COMMAND | Unrecognized command type received |

## Implementation Notes

- Default chunk request size: 40 chunks per batch
- Maximum concurrent transfers: 1 (single-session protocol)
- Recommended timeout: 30 seconds per chunk batch
- All multi-byte integers use little-endian byte order