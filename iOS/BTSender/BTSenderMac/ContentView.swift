//
//  ContentView.swift
//  BTSenderMac
//
//  Created by Jonas Schnelli on 7/26/25.
//

import SwiftUI
import CoreBluetooth

class ContentViewModel: ObservableObject {
    let transferManager = BluetoothTransferManager()
    @Published var transferStatus = "Ready to transfer"
    @Published var progress: Float = 0.0
    @Published var discoveredDevices: [DiscoveredDevice] = []
    @Published var isScanning = false
    @Published var showDeviceList = false
    
    init() {
        transferManager.delegate = self
    }
    
    func sendJPEGFile() {
        guard let jpegData = createSampleJPEGData() else {
            transferStatus = "Failed to create sample data"
            return
        }
        transferManager.transferFile(jpegData)
    }
    
    func startDeviceScan() {
        discoveredDevices.removeAll()
        transferManager.startGeneralScan()
    }
    
    func checkBluetoothStatus() {
        let status = transferManager.checkBluetoothStatus()
        transferStatus = "Bluetooth status: \(status)"
    }
    
    private func createSampleJPEGData() -> Data? {
        let size = CGSize(width: 1800, height: 1000)
        
        #if canImport(UIKit)
        UIGraphicsBeginImageContext(size)
        defer { UIGraphicsEndImageContext() }
        
        guard let context = UIGraphicsGetCurrentContext() else { return nil }
        
        // Create gradient
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let colors = [
            UIColor.blue.cgColor,
            UIColor.purple.cgColor,
            UIColor.red.cgColor
        ] as CFArray
        
        let locations: [CGFloat] = [0.0, 0.5, 1.0]
        
        guard let gradient = CGGradient(colorsSpace: colorSpace,
                                       colors: colors,
                                       locations: locations) else { return nil }
        
        // Draw gradient (linear from top to bottom)
        let startPoint = CGPoint(x: size.width / 2, y: 0)
        let endPoint = CGPoint(x: size.width / 2, y: size.height)
        
        context.drawLinearGradient(gradient,
                                  start: startPoint,
                                  end: endPoint,
                                  options: [])
        
        guard let image = UIGraphicsGetImageFromCurrentImageContext() else { return nil }
        return image.jpegData(compressionQuality: 0.8)
        
        #elseif canImport(AppKit)
        let image = NSImage(size: size)
        image.lockFocus()
        NSColor.blue.setFill()
        NSRect(origin: .zero, size: size).fill()
        image.unlockFocus()
        
        guard let tiffData = image.tiffRepresentation,
              let bitmapRep = NSBitmapImageRep(data: tiffData) else { return nil }
        
        return bitmapRep.representation(using: .jpeg, properties: [.compressionFactor: 0.8])
        
        #else
        return nil
        #endif
    }
}

extension ContentViewModel: BluetoothTransferDelegate {
    func transferDidStart() {
        NSLog("[BTTransfer] UI: Transfer started")
        DispatchQueue.main.async {
            self.transferStatus = "Scanning for devices..."
            self.progress = 0.0
        }
    }
    
    func transferDidComplete() {
        NSLog("[BTTransfer] UI: Transfer completed")
        DispatchQueue.main.async {
            self.transferStatus = "Transfer completed successfully"
            self.progress = 1.0
        }
    }
    
    func transferDidComplete(fileSize: Int, duration: TimeInterval, throughput: Double) {
        NSLog("[BTTransfer] UI: Transfer completed with stats")
        DispatchQueue.main.async {
            self.transferStatus = String(format: "Transfer completed!\n%d bytes in %.1fs (%.1f KB/s)", fileSize, duration, throughput)
            self.progress = 1.0
        }
    }
    
    func transferDidFail(error: Error) {
        NSLog("[BTTransfer] UI: Transfer failed - \(error.localizedDescription)")
        DispatchQueue.main.async {
            self.transferStatus = "Transfer failed: \(error.localizedDescription)"
            self.progress = 0.0
        }
    }
    
    func transferProgress(_ progress: Float) {
        NSLog("[BTTransfer] UI: Progress \(Int(progress * 100))%")
        DispatchQueue.main.async {
            self.progress = progress
            self.transferStatus = "Transferring... \(Int(progress * 100))%"
        }
    }
    
    func devicesDiscovered(_ devices: [DiscoveredDevice]) {
        NSLog("[BTTransfer] UI: Discovered \(devices.count) devices")
        DispatchQueue.main.async {
            self.discoveredDevices = devices
            self.showDeviceList = true
        }
    }
    
    func scanningStateChanged(_ isScanning: Bool) {
        NSLog("[BTTransfer] UI: Scanning state changed to \(isScanning)")
        DispatchQueue.main.async {
            self.isScanning = isScanning
        }
    }
}

struct ContentView: View {
    @StateObject private var viewModel = ContentViewModel()
    
    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "wifi.router")
                .imageScale(.large)
                .foregroundStyle(.tint)
                .font(.system(size: 48))
            
            Text("Bluetooth File Sender")
                .font(.title)
                .fontWeight(.bold)
            
            Text("macOS")
                .font(.caption)
                .foregroundColor(.secondary)
            
            Divider()
            
            VStack(spacing: 12) {
                Text(viewModel.transferStatus)
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(minHeight: 40)
                
                if viewModel.progress > 0 {
                    ProgressView(value: viewModel.progress)
                        .progressViewStyle(LinearProgressViewStyle())
                        .frame(maxWidth: 300)
                    Text("\(Int(viewModel.progress * 100))%")
                        .font(.caption)
                        .fontWeight(.medium)
                }
            }
            
            VStack(spacing: 12) {
                Button("Send JPEG File") {
                    viewModel.sendJPEGFile()
                }
                .buttonStyle(.borderedProminent)
                .disabled(viewModel.transferStatus.contains("Transfer") && !viewModel.transferStatus.contains("failed"))
                .controlSize(.large)
                
                HStack(spacing: 12) {
                    Button(viewModel.isScanning ? "Scanning..." : "Scan Devices") {
                        viewModel.startDeviceScan()
                    }
                    .buttonStyle(.bordered)
                    .disabled(viewModel.isScanning)
                    .controlSize(.large)
                    
                    Button("Check BT Status") {
                        viewModel.checkBluetoothStatus()
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.large)
                }
            }
            
            if viewModel.isScanning {
                HStack {
                    ProgressView()
                        .scaleEffect(0.8)
                    Text("Scanning for BLE devices...")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(30)
        .frame(minWidth: 500, minHeight: 350)
        .sheet(isPresented: $viewModel.showDeviceList) {
            DeviceListView(devices: viewModel.discoveredDevices)
        }
    }
}

struct DeviceListView: View {
    let devices: [DiscoveredDevice]
    @Environment(\.dismiss) private var dismiss
    
    var body: some View {
        NavigationView {
            VStack {
                if devices.isEmpty {
                    ContentUnavailableView(
                        "No Devices Found",
                        systemImage: "antenna.radiowaves.left.and.right",
                        description: Text("No BLE devices were discovered during the scan.")
                    )
                } else {
                    List(devices, id: \.identifier) { device in
                        DeviceRowView(device: device)
                    }
                }
            }
            .navigationTitle("Discovered BLE Devices (\(devices.count))")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button("Close") {
                        dismiss()
                    }
                }
            }
        }
        .frame(minWidth: 600, minHeight: 400)
    }
}

struct DeviceRowView: View {
    let device: DiscoveredDevice
    
    private var isESP32: Bool {
        device.displayName.contains("ESP32") || device.displayName.contains("ESP")
    }
    
    private var hasTargetService: Bool {
        if let serviceUUIDs = device.advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] {
            let targetUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
            return serviceUUIDs.contains(targetUUID)
        }
        return false
    }
    
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.displayName)
                        .font(.headline)
                        .foregroundColor(isESP32 ? .orange : .primary)
                    
                    Text(device.identifier)
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .textSelection(.enabled)
                }
                
                Spacer()
                
                VStack(alignment: .trailing, spacing: 2) {
                    Text("RSSI: \(device.rssi) dBm")
                        .font(.caption)
                        .foregroundColor(rssiColor)
                    
                    if hasTargetService {
                        Label("Target Service", systemImage: "checkmark.circle.fill")
                            .font(.caption)
                            .foregroundColor(.green)
                    }
                    
                    if isESP32 {
                        Label("ESP32", systemImage: "cpu")
                            .font(.caption)
                            .foregroundColor(.orange)
                    }
                }
            }
            
            if !device.advertisementData.isEmpty {
                DisclosureGroup("Advertisement Data") {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(device.advertisementData.keys.sorted()), id: \.self) { key in
                            HStack {
                                Text(key)
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                Spacer()
                                Text("\(device.advertisementData[key] ?? "nil")")
                                    .font(.caption)
                                    .textSelection(.enabled)
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }
                .font(.caption)
            }
        }
        .padding(.vertical, 4)
    }
    
    private var rssiColor: Color {
        let rssi = device.rssi.intValue
        if rssi > -50 { return .green }
        else if rssi > -70 { return .orange }
        else { return .red }
    }
}

#Preview {
    ContentView()
}
