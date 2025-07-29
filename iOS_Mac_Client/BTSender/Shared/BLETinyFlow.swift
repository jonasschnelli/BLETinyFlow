// Copyright (c) 2025 Jonas Schnelli
// Distributed under the MIT software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
// created with the help of Claude AI

import Foundation
import CoreBluetooth
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

// MARK: - Protocol Constants

struct BLETinyFlowProtocol {
    static let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let controlCharacteristicUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    static let dataChannelUUID = CBUUID(string: "6E400010-B5A3-F393-E0A9-E50E24DCCA9E")
    
    static let maxMTU = 512
    static let maxFileSize = (65536*5)
    static let defaultMTU = 512
    static let controlMessageSize = 20
    static let attHeaderSize = 3  // ATT protocol header overhead
    static let dataHeaderSize = 4  // Our data packet header (chunkID + dataLength)
    static let maxDataPayload = maxMTU - attHeaderSize - dataHeaderSize  // 512 - 3 - 4 = 505
    static let deviceName = "ESP_GATTS_DEMO"
    static let chunkTransmissionDelayMicroseconds: UInt32 = 100000  // 100ms delay between chunks
}

// MARK: - Command Types

enum CommandType: UInt8 {
    case transferInit = 0x01
    case deviceInfo = 0x02
    case chunkRequest = 0x82
    case transferCompleteAck = 0x83
}

// MARK: - Message Structures

struct DeviceInfo {
    let deviceType: UInt8
    let batteryLevel: UInt8
    let width: UInt16
    let height: UInt16
    
    init(param1: UInt32, param2: UInt32) {
        // Extract device info from param1: device_type (8), battery_level (8), reserved (16)
        self.deviceType = UInt8(param1 & 0xFF)
        self.batteryLevel = UInt8((param1 >> 8) & 0xFF)
        
        // Extract dimensions from param2: width (16), height (16)
        self.width = UInt16(param2 & 0xFFFF)
        self.height = UInt16((param2 >> 16) & 0xFFFF)
    }
    
    var description: String {
        return "DeviceType: \(deviceType), Battery: \(batteryLevel)%, Dimensions: \(width)x\(height)"
    }
}

struct ControlMessage {
    let command: CommandType
    let sequenceNumber: UInt16
    let param1: UInt32
    let param2: UInt32
    let param3: UInt32
    
    func toData() -> Data {
        var data = Data(capacity: BLETinyFlowProtocol.controlMessageSize)
        data.append(command.rawValue)
        data.append(contentsOf: withUnsafeBytes(of: sequenceNumber.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: param1.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: param2.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: param3.littleEndian) { Array($0) })
        
        while data.count < BLETinyFlowProtocol.controlMessageSize {
            data.append(0)
        }
        
        return data
    }
    
    static func fromData(_ data: Data) -> ControlMessage? {
        guard data.count >= 15 else { return nil }
        
        let command = CommandType(rawValue: data[0])
        guard let cmd = command else { return nil }
        
        let seqNum = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 1, as: UInt16.self) }.littleEndian
        let p1 = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 3, as: UInt32.self) }.littleEndian
        let p2 = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 7, as: UInt32.self) }.littleEndian
        let p3 = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 11, as: UInt32.self) }.littleEndian
        
        return ControlMessage(command: cmd, sequenceNumber: seqNum, param1: p1, param2: p2, param3: p3)
    }
}

struct DataPacket {
    let chunkID: UInt16
    let dataLength: UInt16
    let data: Data
    
    func toData() -> Data {
        var packet = Data(capacity: BLETinyFlowProtocol.dataHeaderSize + data.count)
        packet.append(contentsOf: withUnsafeBytes(of: chunkID.littleEndian) { Array($0) })
        packet.append(contentsOf: withUnsafeBytes(of: dataLength.littleEndian) { Array($0) })
        packet.append(data)
        
        NSLog("[BTTransfer] DataPacket created - ChunkID: %d, HeaderDataLength: %d, ActualPayloadSize: %d, TotalPacketSize: %d (ATT header adds %d bytes)", 
              chunkID, dataLength, data.count, packet.count, BLETinyFlowProtocol.attHeaderSize)
        
        return packet
    }
}

// MARK: - Transfer State

enum TransferState: Equatable {
    case idle
    case connecting
    case sendingInit
    case waitingForChunkRequest
    case sendingData
    case waitingForComplete
    case completed
    case failed(Error)
    
    static func == (lhs: TransferState, rhs: TransferState) -> Bool {
        switch (lhs, rhs) {
        case (.idle, .idle),
             (.connecting, .connecting),
             (.sendingInit, .sendingInit),
             (.waitingForChunkRequest, .waitingForChunkRequest),
             (.sendingData, .sendingData),
             (.waitingForComplete, .waitingForComplete),
             (.completed, .completed):
            return true
        case (.failed, .failed):
            return true
        default:
            return false
        }
    }
}

// MARK: - Discovered Device

class DiscoveredDevice: ObservableObject {
    let peripheral: CBPeripheral
    let name: String
    let rssi: NSNumber
    let advertisementData: [String: Any]
    let discoveredAt: Date
    @Published var deviceInfo: DeviceInfo?
    
    init(peripheral: CBPeripheral, name: String, rssi: NSNumber, advertisementData: [String: Any], discoveredAt: Date) {
        self.peripheral = peripheral
        self.name = name
        self.rssi = rssi
        self.advertisementData = advertisementData
        self.discoveredAt = discoveredAt
        self.deviceInfo = nil
    }
    
    var displayName: String {
        return name.isEmpty ? "Unknown Device" : name
    }
    
    var identifier: String {
        return peripheral.identifier.uuidString
    }
    
    var displayNameWithInfo: String {
        guard let info = deviceInfo else { 
            NSLog("[BTTransfer] Device \(displayName) has no deviceInfo")
            return displayName 
        }
        let infoString = "\(displayName) • \(info.width)x\(info.height) • Type:\(info.deviceType) • \(info.batteryLevel)%"
        NSLog("[BTTransfer] Device \(displayName) info: \(infoString)")
        return infoString
    }
}

// MARK: - Bluetooth Manager Delegate

protocol BLETinyFlowManagerDelegate: AnyObject {
    func transferDidStart()
    func transferDidComplete()
    func transferDidComplete(fileSize: Int, duration: TimeInterval, throughput: Double)
    func transferDidFail(error: Error)
    func transferProgress(_ progress: Float)
    func devicesDiscovered(_ devices: [DiscoveredDevice])
    func scanningStateChanged(_ isScanning: Bool)
    func targetDevicesDiscovered(_ devices: [DiscoveredDevice])
    func deviceDidConnect(_ device: DiscoveredDevice)
    func deviceDidDisconnect(_ device: DiscoveredDevice?, error: Error?)
    func deviceConnectionDidFail(_ device: DiscoveredDevice, error: Error)
    func deviceInfoReceived(_ deviceInfo: DeviceInfo)
}

// MARK: - Bluetooth Transfer Manager

class BLETinyFlowManager: NSObject, ObservableObject {
    weak var delegate: BLETinyFlowManagerDelegate?
    
    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var controlCharacteristic: CBCharacteristic?
    private var dataCharacteristic: CBCharacteristic?
    private var controlNotificationsEnabled = false
    
    private var transferState: TransferState = .idle
    private var currentMTU: Int = BLETinyFlowProtocol.defaultMTU
    private var fileData: Data?
    private var sequenceNumber: UInt16 = 0
    private var chunkDelayMicroseconds: UInt32 = BLETinyFlowProtocol.chunkTransmissionDelayMicroseconds
    private var fileChunks: [Data] = []
    private var totalChunks: Int = 0
    private var transferStartTime: Date?
    private var transferFileSize: Int = 0
    private var chunkSendStartTime: Date?
    private var totalChunksSent: Int = 0
    private var totalBytesWritten: Int = 0
    
    private var connectionTimer: Timer?
    private var transferTimer: Timer?
    private var scanTimer: Timer?
    
    private var discoveredDevices: [String: DiscoveredDevice] = [:]
    private var isGeneralScanning = false
    private var targetDevices: [String: DiscoveredDevice] = [:]
    private var isTargetScanning = false
    private var connectedDevice: DiscoveredDevice?
    private var connectionState: ConnectionState = .disconnected
    private var currentDeviceInfo: DeviceInfo?
    
    enum ConnectionState {
        case disconnected
        case connecting
        case connected
    }
    
    override init() {
        NSLog("[BTTransfer] Initializing BluetoothTransferManager")
        super.init()
        
        centralManager = CBCentralManager(delegate: self, queue: nil)
        NSLog("[BTTransfer] Central manager created")
    }
    
    // Method to refresh UI with current target devices
    func refreshTargetDevicesUI() {
        let devices = Array(targetDevices.values).sorted { $0.rssi.intValue > $1.rssi.intValue }
        NSLog("[BTTransfer] Refreshing UI with \(devices.count) target devices")
        for device in devices {
            NSLog("[BTTransfer] - \(device.displayName) (\(device.identifier))")
        }
        delegate?.targetDevicesDiscovered(devices)
    }
    
    func debugCurrentState() {
        NSLog("[BTTransfer] DEBUG STATE:")
        NSLog("[BTTransfer] - isTargetScanning: \(isTargetScanning)")
        NSLog("[BTTransfer] - targetDevices count: \(targetDevices.count)")
        NSLog("[BTTransfer] - connectionState: \(connectionState)")
        NSLog("[BTTransfer] - centralManager state: \(centralManager.state.rawValue)")
        for device in targetDevices.values {
            NSLog("[BTTransfer] - Target device: \(device.displayName)")
        }
    }
    
    // MARK: - Public Methods
    
    func checkBluetoothStatus() -> String {
        let stateString: String
        switch centralManager.state {
        case .unknown: stateString = "unknown"
        case .resetting: stateString = "resetting"
        case .unsupported: stateString = "unsupported"
        case .unauthorized: stateString = "unauthorized"
        case .poweredOff: stateString = "poweredOff"
        case .poweredOn: stateString = "poweredOn"
        @unknown default: stateString = "unknown(\(centralManager.state.rawValue))"
        }
        NSLog("[BTTransfer] Bluetooth status check: \(stateString)")
        return stateString
    }
    
    func setChunkDelay(microseconds: UInt32) {
        chunkDelayMicroseconds = microseconds
        NSLog("[BTTransfer] Chunk delay set to %d microseconds", microseconds)
    }
    
    
    func startGeneralScan() {
        NSLog("[BTTransfer] Starting general BLE device scan")
        guard centralManager.state == .poweredOn else {
            NSLog("[BTTransfer] Cannot scan - Bluetooth not powered on")
            return
        }
        
        discoveredDevices.removeAll()
        isGeneralScanning = true
        delegate?.scanningStateChanged(true)
        
        centralManager.scanForPeripherals(withServices: nil, options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: false
        ])
        
        scanTimer = Timer.scheduledTimer(withTimeInterval: 10.0, repeats: false) { [weak self] _ in
            self?.stopGeneralScan()
        }
    }
    
    func stopGeneralScan() {
        NSLog("[BTTransfer] Stopping general scan, found \(discoveredDevices.count) devices")
        centralManager.stopScan()
        isGeneralScanning = false
        scanTimer?.invalidate()
        delegate?.scanningStateChanged(false)
        
        let devices = Array(discoveredDevices.values).sorted { $0.rssi.intValue > $1.rssi.intValue }
        delegate?.devicesDiscovered(devices)
    }
    
    func startTargetDeviceScan() {
        NSLog("[BTTransfer] Starting target device scan")
        guard centralManager.state == .poweredOn else {
            NSLog("[BTTransfer] Cannot scan - Bluetooth not powered on")
            // Retry after delay if Bluetooth is not ready yet
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
                self?.startTargetDeviceScan()
            }
            return
        }
        
        // Don't clear existing devices on restart
        isTargetScanning = true
        
        NSLog("[BTTransfer] Scanning for target service UUID devices")
        centralManager.scanForPeripherals(withServices: [BLETinyFlowProtocol.serviceUUID], options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: false
        ])
        
//        // Scan continuously without frequent restarts
//        scanTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: true) { [weak self] _ in
//            self?.refreshTargetDeviceScan()
//        }
    }
    
    private func refreshTargetDeviceScan() {
        NSLog("[BTTransfer] Refreshing target device scan - keeping \(targetDevices.count) existing devices")
        if isTargetScanning && centralManager.state == .poweredOn {
            // Don't stop/restart scan, just ensure it's still running
            // The scan should continue running automatically
            NSLog("[BTTransfer] Target scan refresh - scan should be continuous")
        }
    }
    
    func stopTargetDeviceScan() {
        NSLog("[BTTransfer] Stopping target device scan")
        if isTargetScanning {
            centralManager.stopScan()
            isTargetScanning = false
            scanTimer?.invalidate()
        }
    }
    
    func connectToDevice(_ device: DiscoveredDevice) {
        NSLog("[BTTransfer] Attempting to connect to device: \(device.displayName)")
        guard connectionState == .disconnected else {
            NSLog("[BTTransfer] Already connecting or connected")
            return
        }
        
        connectionState = .connecting
        peripheral = device.peripheral
        device.peripheral.delegate = self
        
        NSLog("[BTTransfer] Connecting to peripheral")
        centralManager.connect(device.peripheral, options: nil)
        
        // Stop target scanning while connecting
        stopTargetDeviceScan()
    }
    
    private func handleConnectionTimeout(_ device: DiscoveredDevice) {
        connectionState = .disconnected
        delegate?.deviceConnectionDidFail(device, error: TransferError.connectionTimeout)
        // Resume target scanning
        startTargetDeviceScan()
    }
    
    func disconnectFromDevice() {
        NSLog("[BTTransfer] Disconnecting from current device")
        if let peripheral = peripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        resetConnectionState()
        // Resume target scanning
        startTargetDeviceScan()
    }
    
    private func resetConnectionState() {
        connectionState = .disconnected
        connectedDevice = nil
        peripheral = nil
        controlCharacteristic = nil
        dataCharacteristic = nil
        controlNotificationsEnabled = false
        connectionTimer?.invalidate()
        transferTimer?.invalidate()
    }
    
    // Public getters for connection state
    func getConnectedDevice() -> DiscoveredDevice? {
        return connectedDevice
    }
    
    func isConnected() -> Bool {
        return connectionState == .connected
    }
    
    func getTargetDevices() -> [DiscoveredDevice] {
        return Array(targetDevices.values).sorted { $0.rssi.intValue > $1.rssi.intValue }
    }
    
    func getCurrentDeviceInfo() -> DeviceInfo? {
        return currentDeviceInfo
    }
    
    func transferFile(_ fileData: Data) {
        NSLog("[BTTransfer] Transfer requested for \(fileData.count) bytes")
        
        guard fileData.count <= BLETinyFlowProtocol.maxFileSize else {
            NSLog("[BTTransfer] File too large: \(fileData.count) bytes (max: \(BLETinyFlowProtocol.maxFileSize))")
            delegate?.transferDidFail(error: TransferError.fileTooLarge)
            return
        }
        
        guard connectionState == .connected else {
            NSLog("[BTTransfer] Transfer failed - no device connected")
            delegate?.transferDidFail(error: TransferError.notConnected)
            return
        }
        
        guard let peripheral = peripheral, peripheral.state == .connected else {
            NSLog("[BTTransfer] Transfer failed - peripheral not connected")
            delegate?.transferDidFail(error: TransferError.notConnected)
            return
        }
        
        guard controlCharacteristic != nil && dataCharacteristic != nil else {
            NSLog("[BTTransfer] Transfer failed - characteristics not ready")
            delegate?.transferDidFail(error: TransferError.notConnected)
            return
        }
        
        self.fileData = fileData
        transferFileSize = fileData.count
        transferStartTime = Date()
        totalChunksSent = 0
        totalBytesWritten = 0
        
        NSLog("[BTTransfer] Starting transfer to connected device")
        delegate?.transferDidStart()
        attemptTransfer()
    }
    
    private func attemptTransfer() {
        NSLog("[BTTransfer] Attempting transfer")
        guard let peripheral = peripheral,
              peripheral.state == .connected,
              let controlChar = controlCharacteristic,
              let fileData = fileData else {
            NSLog("[BTTransfer] Transfer failed - not connected")
            delegate?.transferDidFail(error: TransferError.notConnected)
            return
        }
        
        transferState = .sendingInit
        
        negotiateMTU()
        let chunkSize = currentMTU - BLETinyFlowProtocol.attHeaderSize - BLETinyFlowProtocol.dataHeaderSize
        let totalChunks = (fileData.count + chunkSize - 1) / chunkSize
        
        let initMessage = ControlMessage(
            command: .transferInit,
            sequenceNumber: nextSequenceNumber(),
            param1: UInt32(fileData.count),
            param2: UInt32(chunkSize),
            param3: UInt32(totalChunks)
        )
        
        // Pre-prepare chunks for sending when requested
        fileChunks = []
        var offset = 0
        while offset < fileData.count {
            let currentChunkSize = min(fileData.count - offset, chunkSize)
            let chunkData = fileData.subdata(in: offset..<(offset + currentChunkSize))
            fileChunks.append(chunkData)
            offset += currentChunkSize
        }
        self.totalChunks = totalChunks
        
        NSLog("[BTTransfer] Sending TRANSFER_INIT: fileSize=\(fileData.count), chunkSize=\(chunkSize), chunks=\(totalChunks)")
        peripheral.writeValue(initMessage.toData(), for: controlChar, type: .withResponse)
        
        transferState = .waitingForChunkRequest
        
        transferTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: false) { [weak self] _ in
            self?.handleTransferTimeout()
        }
    }
    
    func disconnect() {
        transferTimer?.invalidate()
        connectionTimer?.invalidate()
        
        if let peripheral = peripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        
        transferState = .idle
        fileData = nil
    }
    
    // MARK: - Private Methods
    
    private func nextSequenceNumber() -> UInt16 {
        sequenceNumber = sequenceNumber &+ 1
        return sequenceNumber
    }
    
    private func negotiateMTU() {
        guard let peripheral = peripheral else { return }
        
        let peripheralMaxMTUWithResponse = peripheral.maximumWriteValueLength(for: .withResponse)
        let peripheralMaxMTUWithoutResponse = peripheral.maximumWriteValueLength(for: .withoutResponse)
        
        // Use withoutResponse since that's what we're using for data transfer
        currentMTU = min(peripheralMaxMTUWithoutResponse, BLETinyFlowProtocol.maxMTU)
        NSLog("[BTTransfer] MTU Negotiation - WithResponse: %d, WithoutResponse: %d, ProtocolMax: %d, Using: %d", 
              peripheralMaxMTUWithResponse, peripheralMaxMTUWithoutResponse, BLETinyFlowProtocol.maxMTU, currentMTU)
    }
    
    private func sendRequestedChunks(startChunk: UInt32, numChunks: UInt32) {
        guard let dataChar = dataCharacteristic else {
            delegate?.transferDidFail(error: TransferError.notConnected)
            return
        }
        
        let endChunk = startChunk + numChunks - 1
        NSLog("[BTTransfer] Sending %d chunks starting from %d (range %d-%d)", numChunks, startChunk, startChunk, endChunk)
        transferState = .sendingData
        
        // Pre-calculate packets to reduce overhead in loop
        var packets: [(Data, UInt32)] = []
        for chunkID in startChunk...endChunk {
            guard chunkID < fileChunks.count else {
                NSLog("[BTTransfer] Requested chunk %d exceeds available chunks (%d)", chunkID, fileChunks.count)
                break
            }
            
            let chunkData = fileChunks[Int(chunkID)]
            let packet = DataPacket(
                chunkID: UInt16(chunkID),
                dataLength: UInt16(chunkData.count),
                data: chunkData
            )
            packets.append((packet.toData(), chunkID))
        }
        
        // Start timing this batch
        chunkSendStartTime = Date()
        let batchStartTime = Date()
        
        // Send packets in tight loop
        for (packetData, chunkID) in packets {
            let writeStartTime = Date()
            
            // Reduce logging frequency for performance
            if chunkID % 10 == 0 {
                NSLog("[BTTransfer] Sending chunk %d - Size: %d bytes", chunkID, packetData.count)
            }
            
            peripheral?.writeValue(packetData, for: dataChar, type: .withoutResponse)
            
            let writeTime = Date().timeIntervalSince(writeStartTime)
            totalChunksSent += 1
            totalBytesWritten += packetData.count
            
            // Log slow writes
            if writeTime > 0.001 { // > 1ms
                NSLog("[BTTransfer] SLOW WRITE: chunk %d took %.3fms", chunkID, writeTime * 1000)
            }
            
            // Update progress less frequently to reduce overhead
            if chunkID % 5 == 0 {
                let progress = Float(chunkID + 1) / Float(totalChunks)
                DispatchQueue.main.async { [weak self] in
                    self?.delegate?.transferProgress(progress)
                }
            }
            
            // Add delay to prevent overwhelming ESP32 receive buffer
            //usleep(chunkDelayMicroseconds)
        }
        
        // Log batch timing
        let batchTime = Date().timeIntervalSince(batchStartTime)
        let bytesInBatch = packets.reduce(0) { $0 + $1.0.count }
        let batchThroughput = Double(bytesInBatch) / 1024.0 / batchTime
        NSLog("[BTTransfer] Batch of %d chunks (%d bytes) sent in %.3fms (%.1f KB/s)", packets.count, bytesInBatch, batchTime * 1000, batchThroughput)
        
        // Log cumulative stats
        if let startTime = transferStartTime {
            let totalTime = Date().timeIntervalSince(startTime)
            let cumulativeThroughput = Double(totalBytesWritten) / 1024.0 / totalTime
            NSLog("[BTTransfer] Cumulative: %d chunks (%d bytes) in %.3fs (%.1f KB/s)", totalChunksSent, totalBytesWritten, totalTime, cumulativeThroughput)
        }
        
        NSLog("[BTTransfer] Finished sending requested chunks, waiting for next request or completion")
        DispatchQueue.main.async { [weak self] in
            self?.transferState = .waitingForChunkRequest
        }
    }
    
    private func handleTransferTimeout() {
        transferState = .failed(TransferError.timeout)
        delegate?.transferDidFail(error: TransferError.timeout)
    }
}

// MARK: - CBCentralManagerDelegate

extension BLETinyFlowManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let stateString: String
        switch central.state {
        case .unknown: stateString = "unknown"
        case .resetting: stateString = "resetting"
        case .unsupported: stateString = "unsupported"
        case .unauthorized: stateString = "unauthorized"
        case .poweredOff: stateString = "poweredOff"
        case .poweredOn: stateString = "poweredOn"
        @unknown default: stateString = "unknown(\(central.state.rawValue))"
        }
        
        NSLog("[BTTransfer] Central manager state changed to: \(stateString) (\(central.state.rawValue))")
        
        switch central.state {
        case .poweredOn:
            NSLog("[BTTransfer] Bluetooth LE is ready")
            if !isTargetScanning {
                NSLog("[BTTransfer] Auto-starting target device scan")
                startTargetDeviceScan()
            }
            break
        case .poweredOff:
            NSLog("[BTTransfer] Bluetooth is turned off")
            delegate?.transferDidFail(error: TransferError.bluetoothPoweredOff)
        case .unauthorized:
            NSLog("[BTTransfer] Bluetooth access not authorized")
            delegate?.transferDidFail(error: TransferError.bluetoothUnauthorized)
        case .unsupported:
            NSLog("[BTTransfer] Bluetooth LE not supported on this device")
            delegate?.transferDidFail(error: TransferError.bluetoothUnsupported)
        case .unknown, .resetting:
            NSLog("[BTTransfer] Bluetooth state transitioning: \(stateString)")
            break
        @unknown default:
            NSLog("[BTTransfer] Unknown Bluetooth state: \(central.state.rawValue)")
            delegate?.transferDidFail(error: TransferError.bluetoothUnavailable)
        }
    }
    
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let deviceName = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? ""
        NSLog("[BTTransfer] Discovered peripheral: '\(deviceName)' (\(peripheral.identifier)) RSSI: \(RSSI)")
        
        for (key, value) in advertisementData {
            NSLog("[BTTransfer]   AdData: \(key) = \(value)")
        }
        
        // Log manufacturer data if available
        if let manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
            NSLog("[BTTransfer]   Manufacturer Data: \(manufacturerData.map { String(format: "%02x", $0) }.joined(separator: " "))")
        }
        
        if isGeneralScanning {
            let device = DiscoveredDevice(
                peripheral: peripheral,
                name: deviceName,
                rssi: RSSI,
                advertisementData: advertisementData,
                discoveredAt: Date()
            )
            discoveredDevices[peripheral.identifier.uuidString] = device
            NSLog("[BTTransfer] Added device to general discovery list: '\(device.displayName)'")
        } else if isTargetScanning {
            let device = DiscoveredDevice(
                peripheral: peripheral,
                name: deviceName,
                rssi: RSSI,
                advertisementData: advertisementData,
                discoveredAt: Date()
            )
            targetDevices[peripheral.identifier.uuidString] = device
            NSLog("[BTTransfer] Added target device: '\(device.displayName)' (total: \(targetDevices.count))")
            
            // Notify delegate of updated target devices
            let devices = Array(targetDevices.values).sorted { $0.rssi.intValue > $1.rssi.intValue }
            NSLog("[BTTransfer] Notifying delegate of \(devices.count) target devices")
            delegate?.targetDevicesDiscovered(devices)
        } else {
            NSLog("[BTTransfer] Device discovered but not scanning: general=\(isGeneralScanning), target=\(isTargetScanning)")
        }
    }
    
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        NSLog("[BTTransfer] Connected to peripheral: \(peripheral.name ?? "Unknown")")
        connectionState = .connected
        
        // Find the connected device in our target devices
        if let device = targetDevices.values.first(where: { $0.peripheral.identifier == peripheral.identifier }) {
            connectedDevice = device
            delegate?.deviceDidConnect(device)
        }
        
        NSLog("[BTTransfer] Discovering services")
        peripheral.discoverServices([BLETinyFlowProtocol.serviceUUID])
        
        connectionTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: false) { [weak self] _ in
            NSLog("[BTTransfer] Service discovery timeout")
            if let device = self?.connectedDevice {
                self?.delegate?.deviceConnectionDidFail(device, error: TransferError.connectionTimeout)
            }
        }
    }
    
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        NSLog("[BTTransfer] Failed to connect to peripheral: \(error?.localizedDescription ?? "Unknown error")")
        
        if let device = targetDevices.values.first(where: { $0.peripheral.identifier == peripheral.identifier }) {
            delegate?.deviceConnectionDidFail(device, error: error ?? TransferError.connectionFailed)
        }
        
        resetConnectionState()
        // Resume target scanning
        startTargetDeviceScan()
    }
    
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        NSLog("[BTTransfer] Disconnected from peripheral: \(error?.localizedDescription ?? "No error")")
        
        let disconnectedDevice = connectedDevice
        delegate?.deviceDidDisconnect(disconnectedDevice, error: error)
        
        resetConnectionState()
        // Resume target scanning
        startTargetDeviceScan()
    }
}

// MARK: - CBPeripheralDelegate

extension BLETinyFlowManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        NSLog("[BTTransfer] Discovered services, error: \(error?.localizedDescription ?? "none")")
        guard let services = peripheral.services else { 
            NSLog("[BTTransfer] No services found")
            return 
        }
        
        NSLog("[BTTransfer] Found \(services.count) services")
        for service in services {
            NSLog("[BTTransfer] Service UUID: \(service.uuid)")
            if service.uuid == BLETinyFlowProtocol.serviceUUID {
                NSLog("[BTTransfer] Found target service, discovering characteristics")
                peripheral.discoverCharacteristics([BLETinyFlowProtocol.controlCharacteristicUUID, BLETinyFlowProtocol.dataChannelUUID], for: service)
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        NSLog("[BTTransfer] Discovered characteristics, error: \(error?.localizedDescription ?? "none")")
        guard let characteristics = service.characteristics else { 
            NSLog("[BTTransfer] No characteristics found")
            return 
        }
        
        NSLog("[BTTransfer] Found \(characteristics.count) characteristics")
        for characteristic in characteristics {
            NSLog("[BTTransfer] Characteristic UUID: \(characteristic.uuid)")
            if characteristic.uuid == BLETinyFlowProtocol.controlCharacteristicUUID {
                NSLog("[BTTransfer] Found control characteristic")
                NSLog("[BTTransfer] Control characteristic properties: \(characteristic.properties.rawValue)")
                NSLog("[BTTransfer] Control characteristic supports notify: \(characteristic.properties.contains(.notify))")
                NSLog("[BTTransfer] Control characteristic supports indicate: \(characteristic.properties.contains(.indicate))")
                controlCharacteristic = characteristic
                
                if characteristic.properties.contains(.notify) || characteristic.properties.contains(.indicate) {
                    NSLog("[BTTransfer] Control characteristic supports notifications")
                    NSLog("[BTTransfer] Immediately discovering descriptors for control characteristic...")
                    peripheral.discoverDescriptors(for: characteristic)
                } else {
                    NSLog("[BTTransfer] Control characteristic does not support notifications!")
                }
            } else if characteristic.uuid == BLETinyFlowProtocol.dataChannelUUID {
                NSLog("[BTTransfer] Found data characteristic")
                dataCharacteristic = characteristic
            }
        }
        
        NSLog("[BTTransfer] Characteristics discovery complete - control: \(controlCharacteristic != nil), data: \(dataCharacteristic != nil)")
        
        // Defer descriptor discovery to ensure characteristic discovery is fully complete
        if let controlChar = controlCharacteristic {
            if controlChar.properties.contains(.notify) || controlChar.properties.contains(.indicate) {
                NSLog("[BTTransfer] Waiting for notification enablement before starting transfer...")
                NSLog("[BTTransfer] Deferring descriptor discovery to ensure characteristics are ready...")
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    NSLog("[BTTransfer] Starting delayed descriptor discovery...")
                    self?.peripheral?.discoverDescriptors(for: controlChar)
                }
                
                // Add fallback timeout in case descriptor discovery fails
                DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                    guard let self = self else { return }
                    if !self.controlNotificationsEnabled && self.transferState == .connecting {
                        NSLog("[BTTransfer] Descriptor discovery timeout, attempting direct notification enable...")
                        self.peripheral?.setNotifyValue(true, for: controlChar)
                    }
                }
            } else {
                NSLog("[BTTransfer] Control characteristic doesn't support notifications, starting transfer directly")
                if dataCharacteristic != nil {
                    connectionTimer?.invalidate()
                    let mtu = peripheral.maximumWriteValueLength(for: .withResponse)
                    NSLog("[BTTransfer] Connection ready, MTU: \(mtu)")
                    
                    if transferState == .connecting && fileData != nil {
                        NSLog("[BTTransfer] Starting transfer without notifications")
                        attemptTransfer()
                    }
                }
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            NSLog("[BTTransfer] Write error: \(error.localizedDescription)")
            delegate?.transferDidFail(error: error)
            return
        }
        
        NSLog("[BTTransfer] Successfully wrote value to characteristic \(characteristic.uuid)")
        NSLog("[BTTransfer] Write confirmed - no size truncation reported by iOS")
    }
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverDescriptorsFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            NSLog("[BTTransfer] Failed to discover descriptors for characteristic \(characteristic.uuid): \(error.localizedDescription)")
            NSLog("[BTTransfer] Descriptor discovery error domain: \(error._domain)")
            NSLog("[BTTransfer] Descriptor discovery error code: \(error._code)")
            if characteristic.uuid == BLETinyFlowProtocol.controlCharacteristicUUID {
                NSLog("[BTTransfer] Attempting notification enable despite descriptor discovery failure...")
                peripheral.setNotifyValue(true, for: characteristic)
            }
            return
        }
        
        NSLog("[BTTransfer] Successfully discovered descriptors for characteristic \(characteristic.uuid)")
        if let descriptors = characteristic.descriptors {
            NSLog("[BTTransfer] Found \(descriptors.count) descriptors")
            for (index, descriptor) in descriptors.enumerated() {
                NSLog("[BTTransfer] Descriptor \(index): UUID \(descriptor.uuid)")
                if descriptor.uuid == CBUUID(string: CBUUIDClientCharacteristicConfigurationString) {
                    NSLog("[BTTransfer] ✅ Found CCCD (Client Characteristic Configuration Descriptor)")
                }
            }
        } else {
            NSLog("[BTTransfer] Descriptors array is nil for characteristic")
        }
        
        if characteristic.uuid == BLETinyFlowProtocol.controlCharacteristicUUID {
            if let descriptors = characteristic.descriptors, 
               descriptors.contains(where: { $0.uuid == CBUUID(string: CBUUIDClientCharacteristicConfigurationString) }) {
                NSLog("[BTTransfer] CCCD found, attempting to enable notifications...")
            } else {
                NSLog("[BTTransfer] CCCD not found, but ESP32 says it exists - attempting notification enable anyway...")
            }
            peripheral.setNotifyValue(true, for: characteristic)
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            NSLog("[BTTransfer] Failed to update notification state for characteristic \(characteristic.uuid): \(error.localizedDescription)")
            NSLog("[BTTransfer] Error domain: \(error._domain)")
            NSLog("[BTTransfer] Error code: \(error._code)")
            if let nsError = error as NSError? {
                NSLog("[BTTransfer] NSError userInfo: \(nsError.userInfo)")
            }
            NSLog("[BTTransfer] This usually means the CCCD descriptor is missing on the ESP32 side")
            NSLog("[BTTransfer] Attempting to proceed without notifications (will use polling mode)")
            
            if dataCharacteristic != nil {
                connectionTimer?.invalidate()
                let mtu = peripheral.maximumWriteValueLength(for: .withResponse)
                NSLog("[BTTransfer] Proceeding without notifications, MTU: \(mtu)")
                
                if transferState == .connecting && fileData != nil {
                    NSLog("[BTTransfer] Starting transfer without notifications")
                    attemptTransfer()
                }
            }
            return
        }
        
        if characteristic.uuid == BLETinyFlowProtocol.controlCharacteristicUUID {
            controlNotificationsEnabled = characteristic.isNotifying
            NSLog("[BTTransfer] Control characteristic notifications enabled: \(controlNotificationsEnabled)")
            
            if controlNotificationsEnabled && dataCharacteristic != nil {
                connectionTimer?.invalidate()
                
                let mtu = peripheral.maximumWriteValueLength(for: .withResponse)
                NSLog("[BTTransfer] Connection ready, MTU: \(mtu)")
                
                if transferState == .connecting && fileData != nil {
                    NSLog("[BTTransfer] Notifications enabled, starting pending transfer")
                    attemptTransfer()
                }
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        
        if characteristic.uuid == BLETinyFlowProtocol.controlCharacteristicUUID {
            handleControlMessage(data)
        }
    }
    
    private func handleControlMessage(_ data: Data) {
        NSLog("[BTTransfer] Received control message (\(data.count) bytes)")
        guard let message = ControlMessage.fromData(data) else {
            NSLog("[BTTransfer] Failed to parse control message")
            return
        }
        
        NSLog("[BTTransfer] Control command: \(message.command.rawValue)")
        switch message.command {
        case .deviceInfo:
            let deviceInfo = DeviceInfo(param1: message.param1, param2: message.param2)
            NSLog("[BTTransfer] Received DEVICE_INFO: \(deviceInfo.description)")
            currentDeviceInfo = deviceInfo
            
            // Update connected device with device info
            if let device = connectedDevice {
                NSLog("[BTTransfer] Updating connected device \(device.displayName) with device info")
                device.deviceInfo = deviceInfo
                NSLog("[BTTransfer] Device info set, calling delegate")
                delegate?.deviceInfoReceived(deviceInfo)
            } else {
                NSLog("[BTTransfer] No connected device to update with device info")
            }
            
        case .chunkRequest:
            NSLog("[BTTransfer] Received CHUNK_REQUEST: start=%d, numChunks=%d", message.param1, message.param2)
            if transferState == .waitingForChunkRequest || transferState == .sendingData {
                NSLog("[BTTransfer] Processing chunk request (current state: %@)", String(describing: transferState))
                DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                    self?.sendRequestedChunks(startChunk: message.param1, numChunks: message.param2)
                }
            } else {
                NSLog("[BTTransfer] Ignoring chunk request - wrong state: %@", String(describing: transferState))
            }
            
        case .transferCompleteAck:
            NSLog("[BTTransfer] Received TRANSFER_COMPLETE_ACK, transfer finished")
            if transferState == .waitingForChunkRequest || transferState == .sendingData {
                transferTimer?.invalidate()
                transferState = .completed
                
                if let startTime = transferStartTime {
                    let duration = Date().timeIntervalSince(startTime)
                    let throughputKBps = Double(transferFileSize) / 1024.0 / duration
                    NSLog("[BTTransfer] Transfer completed: %d bytes in %.2f seconds (%.2f KB/s)", transferFileSize, duration, throughputKBps)
                    delegate?.transferDidComplete(fileSize: transferFileSize, duration: duration, throughput: throughputKBps)
                } else {
                    delegate?.transferDidComplete()
                }
            }
            
        default:
            NSLog("[BTTransfer] Unknown control command: \(message.command.rawValue)")
            break
        }
    }
}

// MARK: - Error Types

enum TransferError: Error, LocalizedError {
    case bluetoothUnavailable
    case bluetoothUnsupported
    case bluetoothUnauthorized
    case bluetoothPoweredOff
    case notConnected
    case connectionFailed
    case connectionTimeout
    case timeout
    case fileTooLarge
    case deviceNotFound
    
    var errorDescription: String? {
        switch self {
        case .bluetoothUnavailable:
            return "Bluetooth is not available"
        case .bluetoothUnsupported:
            return "Bluetooth LE is not supported on this device. This app requires iPhone 4S or later, iPad 3rd gen or later, or Mac with Bluetooth LE support."
        case .bluetoothUnauthorized:
            return "Bluetooth access is not authorized. Please enable Bluetooth permissions in Settings."
        case .bluetoothPoweredOff:
            return "Bluetooth is turned off. Please enable Bluetooth in Settings."
        case .notConnected:
            return "No device connected. Make sure the receiving device is nearby and discoverable."
        case .connectionFailed:
            return "Failed to connect to device"
        case .connectionTimeout:
            return "Connection timed out"
        case .timeout:
            return "Transfer timed out"
        case .fileTooLarge:
            return "File is too large. Maximum size is 64 KB."
        case .deviceNotFound:
            return "ESP32 device not found. Make sure the device is powered on and nearby."
        }
    }
}
