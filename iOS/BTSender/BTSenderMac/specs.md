## BLE Service Design

### Primary Service UUID
```
Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
```

MTU Sized chunks

### Characteristics

#### 1. Control Characteristic (Write/Notify)
```
UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
Properties: WRITE, NOTIFY
Max Length: 20 bytes
Purpose: Command/control messages, status updates
```

#### 2. Data Characteristics (Write/Notify) - 8 Channels
```
Data Channel 0: 6E400010-B5A3-F393-E0A9-E50E24DCCA9E

Properties: WRITE_NO_RESPONSE, NOTIFY
Max Length: 512 bytes (ESP32S3 supports up to 512 byte MTU)
Purpose: data transmission
Extendability: multichannel data transmission (later)
```

## Protocol Specification

### Message Format

#### Control Messages (20 bytes max)
```
Byte 0: Command Type
Byte 1-2: Sequence Number (uint16_t)
Byte 3-6: Parameter 1 (uint32_t)
Byte 7-10: Parameter 2 (uint32_t)
Byte 11-14: Parameter 3 (uint32_t)
```

#### Data Messages (MTU limited)
```
MTU Calculation:
- Total MTU: 512 bytes
- ATT Header: 3 bytes
- Available for application: 509 bytes

Data Packet Format:
Byte 0-1: Chunk ID (uint16_t)
Byte 2-3: Data length in this packet (uint16_t)
Byte 4+: Data payload (up to 505 bytes = 509 - 4 byte header)
```

### Command Types

#### From iOS to ESP32
```
0x01: TRANSFER_INIT
      - Param1: Total file size
      - Param2: Chunk size (usually MTU - ATT_HEADER - DATA_HEADER = 512 - 3 - 4 = 505 bytes)
      - Param3: Number of chunks
```

#### From ESP32 to iOS
```
0x82: CHUNK_REQUEST
      - Param1: Starting chunk ID
      - Param2: Number of chunks requested
0x83: TRANSFER_COMPLETE_ACK
      - Param1: File size received
```

How it works:
1. iOS sends 0x01 TRANSFER_INIT to ESP on Control Characteristic
2. ESP sends 0x82 CHUNK_REQUEST to request n chunks (default: 20) starting from chunk 0
3. iOS sends the requested chunks as Data Messages on Data Characteristic
4. ESP processes received chunks and requests next batch with CHUNK_REQUEST until all chunks received
5. ESP sends 0x83 TRANSFER_COMPLETE_ACK after all chunks have been received
