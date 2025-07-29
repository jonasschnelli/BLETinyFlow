//
//  ContentView.swift
//  BTSender
//
//  Created by Jonas Schnelli on 7/26/25.
//

import SwiftUI
import CoreBluetooth
import PhotosUI
import Photos

class ContentViewModel: ObservableObject {
    let transferManager = BLETinyFlowManager()
    @Published var transferStatus = "Ready to transfer"
    @Published var progress: Float = 0.0
    @Published var discoveredDevices: [DiscoveredDevice] = []
    @Published var isScanning = false
    @Published var showDeviceList = false
    @Published var targetDevices: [DiscoveredDevice] = []
    @Published var connectedDevice: DiscoveredDevice?
    @Published var connectionStatus = "Scanning for devices..."
    @Published var showTransferComplete = false
    
    init() {
        transferManager.delegate = self
        // Request immediate refresh of target devices in case scan already started
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.transferManager.refreshTargetDevicesUI()
        }
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
    
    func connectToDevice(_ device: DiscoveredDevice) {
        transferManager.connectToDevice(device)
    }
    
    func disconnectFromDevice() {
        transferManager.disconnectFromDevice()
    }
    
    func checkBluetoothStatus() {
        let status = transferManager.checkBluetoothStatus()
        transferStatus = "Bluetooth status: \(status)"
    }
    
    func debugManagerState() {
        transferManager.debugCurrentState()
        transferManager.refreshTargetDevicesUI()
    }
    
    func resetTransferState() {
        transferStatus = "Ready to transfer"
        progress = 0.0
        showTransferComplete = false
    }
    
    func sendSelectedPhoto(_ image: UIImage) {
        guard let jpegData = processSelectedPhoto(image) else {
            transferStatus = "Failed to process selected photo"
            return
        }
        transferManager.transferFile(jpegData)
    }
    
    func processSelectedPhotoForPreview(_ image: UIImage) -> Data? {
        return processSelectedPhoto(image)
    }
    
    func sendProcessedImageData(_ imageData: Data) {
        transferManager.transferFile(imageData)
    }
    
    private func processSelectedPhoto(_ image: UIImage) -> Data? {
        #if canImport(UIKit)
        // Use device screen dimensions if available, otherwise use default
        let targetSize: CGSize
        if let deviceInfo = transferManager.getCurrentDeviceInfo() {
            let deviceWidth = CGFloat(deviceInfo.width)
            let deviceHeight = CGFloat(deviceInfo.height)
            targetSize = calculateTargetSize(from: image.size, targetWidth: deviceWidth, targetHeight: deviceHeight)
            NSLog("[BTTransfer] Using device dimensions: \(deviceWidth)x\(deviceHeight)")
        } else {
            // Fallback to default sizing
            let maxDimension: CGFloat = 800
            targetSize = calculateTargetSize(from: image.size, maxDimension: maxDimension)
            NSLog("[BTTransfer] Using fallback dimensions")
        }
        
        // Create grayscale color space
        guard let colorSpace = CGColorSpace(name: CGColorSpace.genericGrayGamma2_2) else { return nil }
        
        // Create bitmap context for grayscale image (8 bits per component, 1 component)
        guard let context = CGContext(
            data: nil,
            width: Int(targetSize.width),
            height: Int(targetSize.height),
            bitsPerComponent: 8,
            bytesPerRow: Int(targetSize.width),
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.none.rawValue
        ) else { return nil }
        
        // Draw the original image into the grayscale context
        guard let cgImage = image.cgImage else { return nil }
        context.draw(cgImage, in: CGRect(origin: .zero, size: targetSize))
        
        // Create grayscale CGImage from context
        guard let grayscaleCGImage = context.makeImage() else { return nil }
        
        // Convert back to UIImage
        let grayscaleImage = UIImage(cgImage: grayscaleCGImage)
        
        // Start with high quality and reduce if needed
        var compressionQuality: CGFloat = 0.8
        var jpegData = grayscaleImage.jpegData(compressionQuality: compressionQuality)
        
        // Reduce quality until we're under the size limit
        while let data = jpegData, data.count > BLETinyFlowProtocol.maxFileSize && compressionQuality > 0.1 {
            compressionQuality -= 0.1
            jpegData = grayscaleImage.jpegData(compressionQuality: compressionQuality)
        }
        
        return jpegData
        #else
        return nil
        #endif
    }
    
    private func calculateTargetSize(from originalSize: CGSize, maxDimension: CGFloat) -> CGSize {
        let aspectRatio = originalSize.width / originalSize.height
        
        if originalSize.width > originalSize.height {
            // Landscape
            let targetWidth = min(originalSize.width, maxDimension)
            let targetHeight = targetWidth / aspectRatio
            return CGSize(width: targetWidth, height: targetHeight)
        } else {
            // Portrait or square
            let targetHeight = min(originalSize.height, maxDimension)
            let targetWidth = targetHeight * aspectRatio
            return CGSize(width: targetWidth, height: targetHeight)
        }
    }
    
    private func calculateTargetSize(from originalSize: CGSize, targetWidth: CGFloat, targetHeight: CGFloat) -> CGSize {
        let originalAspectRatio = originalSize.width / originalSize.height
        let targetAspectRatio = targetWidth / targetHeight
        
        if originalAspectRatio > targetAspectRatio {
            // Original is wider, fit to width
            let scaledHeight = targetWidth / originalAspectRatio
            return CGSize(width: targetWidth, height: scaledHeight)
        } else {
            // Original is taller, fit to height
            let scaledWidth = targetHeight * originalAspectRatio
            return CGSize(width: scaledWidth, height: targetHeight)
        }
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

extension ContentViewModel: BLETinyFlowManagerDelegate {
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
            self.showTransferComplete = true
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
    
    func targetDevicesDiscovered(_ devices: [DiscoveredDevice]) {
        NSLog("[BTTransfer] UI: Target devices discovered: \(devices.count)")
        for device in devices {
            NSLog("[BTTransfer] UI: - Device: \(device.displayName) (\(device.identifier))")
        }
        DispatchQueue.main.async {
            self.targetDevices = devices
            NSLog("[BTTransfer] UI: Updated target devices array to \(devices.count) items")
            if devices.isEmpty {
                self.connectionStatus = "No target devices found"
            } else {
                self.connectionStatus = "Found \(devices.count) device(s)"
            }
        }
    }
    
    func deviceDidConnect(_ device: DiscoveredDevice) {
        NSLog("[BTTransfer] UI: Device connected: \(device.displayName)")
        DispatchQueue.main.async {
            self.connectedDevice = device
            self.connectionStatus = "Connected to \(device.displayName)"
        }
    }
    
    func deviceDidDisconnect(_ device: DiscoveredDevice?, error: Error?) {
        NSLog("[BTTransfer] UI: Device disconnected")
        DispatchQueue.main.async {
            self.connectedDevice = nil
            if let error = error {
                self.connectionStatus = "Disconnected: \(error.localizedDescription)"
            } else {
                self.connectionStatus = "Disconnected"
            }
        }
    }
    
    func deviceConnectionDidFail(_ device: DiscoveredDevice, error: Error) {
        NSLog("[BTTransfer] UI: Connection failed: \(error.localizedDescription)")
        DispatchQueue.main.async {
            self.connectionStatus = "Connection failed: \(error.localizedDescription)"
        }
    }
    
    func deviceInfoReceived(_ deviceInfo: DeviceInfo) {
        NSLog("[BTTransfer] UI: Device info received: \(deviceInfo.description)")
        DispatchQueue.main.async {
            // Force UI update
            self.objectWillChange.send()
        }
    }
}

struct ContentView: View {
    @StateObject private var viewModel = ContentViewModel()
    @State private var isPhotoPickerPresented = false
    @State private var selectedPhoto: UIImage?
    
    var body: some View {
        NavigationView {
            VStack(spacing: 20) {
                Image(systemName: "wifi.router")
                    .imageScale(.large)
                    .foregroundStyle(.tint)
                    .font(.system(size: 48))
                
                Text("TinyFlow Image Upload")
                    .font(.title)
                    .fontWeight(.bold)
                
                Divider()
                
                // Connection Status Section
                VStack(spacing: 8) {
                    Text("Connection Status")
                        .font(.headline)
                    
                    Text(viewModel.connectionStatus)
                        .font(.subheadline)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                    
                    if let connectedDevice = viewModel.connectedDevice {
                        HStack {
                            Label(connectedDevice.displayNameWithInfo, systemImage: "checkmark.circle.fill")
                                .font(.caption)
                                .foregroundColor(.green)
                            
                            Button("Disconnect") {
                                viewModel.disconnectFromDevice()
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        }
                    }
                }
                
                // Target Devices Section
                if !viewModel.targetDevices.isEmpty && viewModel.connectedDevice == nil {
                    VStack(spacing: 8) {
                        Text("Available Devices")
                            .font(.headline)
                        
                        ScrollView {
                            LazyVStack(spacing: 4) {
                                ForEach(viewModel.targetDevices, id: \.identifier) { device in
                                    TargetDeviceRowView(
                                        device: device,
                                        isConnected: viewModel.connectedDevice?.identifier == device.identifier,
                                        onConnect: { viewModel.connectToDevice(device) }
                                    )
                                }
                            }
                        }
                        .frame(maxHeight: 150)
                    }
                }
                
                Divider()
                
                VStack(spacing: 12) {
                    Text(viewModel.transferStatus)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                        .frame(minHeight: 40)
                    
                    if viewModel.progress > 0 && !viewModel.showTransferComplete {
                        ProgressView(value: viewModel.progress)
                            .progressViewStyle(LinearProgressViewStyle())
                            .frame(maxWidth: 300)
                        Text("\(Int(viewModel.progress * 100))%")
                            .font(.caption)
                            .fontWeight(.medium)
                    }
                    
                    if viewModel.showTransferComplete {
                        Button("OK") {
                            viewModel.resetTransferState()
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                    }
                }
                
                VStack(spacing: 12) {
                    HStack(spacing: 12) {
                        Button("Send JPEG File") {
                            viewModel.sendJPEGFile()
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(viewModel.connectedDevice == nil || viewModel.showTransferComplete || (viewModel.transferStatus.contains("Transfer") && !viewModel.transferStatus.contains("failed")))
                        .controlSize(.large)
                        
                        Button("Send Photo") {
                            isPhotoPickerPresented = true
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(viewModel.connectedDevice == nil || viewModel.showTransferComplete || (viewModel.transferStatus.contains("Transfer") && !viewModel.transferStatus.contains("failed")))
                        .controlSize(.large)
                    }
                    
                    HStack(spacing: 12) {
                        Button(viewModel.isScanning ? "Scanning..." : "Scan All Devices") {
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
                        
                        Button("Debug") {
                            viewModel.debugManagerState()
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    }.hidden()
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
            .navigationTitle("BLE TinyFlow")
            .navigationBarTitleDisplayMode(.inline)
            .sheet(isPresented: $viewModel.showDeviceList) {
                DeviceListView(devices: viewModel.discoveredDevices)
            }
            .sheet(isPresented: $isPhotoPickerPresented) {
                PhotoPickerView(selectedPhoto: $selectedPhoto, viewModel: viewModel)
            }
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
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Close") {
                        dismiss()
                    }
                }
            }
        }
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
                    Text(device.displayNameWithInfo)
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

struct TargetDeviceRowView: View {
    let device: DiscoveredDevice
    let isConnected: Bool
    let onConnect: () -> Void
    
    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(device.displayNameWithInfo)
                    .font(.subheadline)
                    .fontWeight(.medium)
                
                Text("RSSI: \(device.rssi) dBm")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            Spacer()
            
            if isConnected {
                Label("Connected", systemImage: "checkmark.circle.fill")
                    .font(.caption)
                    .foregroundColor(.green)
            } else {
                Button("Connect") {
                    onConnect()
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 8)
        .background(Color(.systemGray6))
        .cornerRadius(8)
    }
}

struct PhotoPickerView: View {
    @Binding var selectedPhoto: UIImage?
    let viewModel: ContentViewModel
    @Environment(\.dismiss) private var dismiss
    @State private var selectedItem: PhotosPickerItem?
    @State private var processedImageData: Data?
    @State private var processedImage: UIImage?
    @State private var isProcessing = false
    @State private var imageMetadata: ImageMetadata?
    
    struct ImageMetadata {
        let originalSize: CGSize
        let processedSize: CGSize
        let fileSize: Int
        let compressionQuality: String
    }
    
    var body: some View {
        NavigationView {
            VStack(spacing: 20) {
                Text("Select a Photo")
                    .font(.title2)
                    .fontWeight(.semibold)
                
                PhotosPicker(selection: $selectedItem, matching: .images) {
                    Label("Choose Photo from Library", systemImage: "photo.on.rectangle")
                        .font(.headline)
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity)
                        .padding()
                        .background(Color.blue)
                        .cornerRadius(12)
                }
                .onChange(of: selectedItem) { _, newItem in
                    Task {
                        if let data = try? await newItem?.loadTransferable(type: Data.self),
                           let image = UIImage(data: data) {
                            selectedPhoto = image
                            await processImage(image)
                        }
                    }
                }
                
                if isProcessing {
                    VStack(spacing: 12) {
                        ProgressView()
                        Text("Processing image...")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    .padding()
                }
                
                if let processedImage = processedImage,
                   let metadata = imageMetadata,
                   let processedData = processedImageData {
                    
                    VStack(spacing: 16) {
                        Text("Processed Image Preview")
                            .font(.headline)
                            .foregroundColor(.primary)
                        
                        // Processed image thumbnail
                        Image(uiImage: processedImage)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .frame(maxHeight: 200)
                            .cornerRadius(8)
                            .overlay(
                                RoundedRectangle(cornerRadius: 8)
                                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
                            )
                        
                        // Image metadata
                        VStack(spacing: 8) {
                            HStack {
                                Text("Dimensions:")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                Spacer()
                                Text("\(Int(metadata.processedSize.width)) × \(Int(metadata.processedSize.height))")
                                    .font(.caption)
                                    .fontWeight(.medium)
                            }
                            
                            HStack {
                                Text("File Size:")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                Spacer()
                                Text(formatFileSize(metadata.fileSize))
                                    .font(.caption)
                                    .fontWeight(.medium)
                            }
                            
                            HStack {
                                Text("Format:")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                Spacer()
                                Text("Grayscale JPEG")
                                    .font(.caption)
                                    .fontWeight(.medium)
                            }
                        }
                        .padding()
                        .background(Color(.systemGray6))
                        .cornerRadius(8)
                        
                        // Send button
                        Button("Send to Device") {
                            viewModel.sendProcessedImageData(processedData)
                            dismiss()
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                        .disabled(viewModel.connectedDevice == nil || viewModel.showTransferComplete || (viewModel.transferStatus.contains("Transfer") && !viewModel.transferStatus.contains("failed")))
                    }
                }
                
                Spacer()
            }
            .padding()
            .navigationTitle("Photo Picker")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
            }
        }
    }
    
    private func processImage(_ image: UIImage) async {
        isProcessing = true
        
        // Use the existing image processing logic
        let originalSize = image.size
        
        await MainActor.run {
            if let processedData = viewModel.processSelectedPhotoForPreview(image) {
                // Create UIImage from processed data to display
                if let processed = UIImage(data: processedData) {
                    self.processedImage = processed
                    self.processedImageData = processedData
                    self.imageMetadata = ImageMetadata(
                        originalSize: originalSize,
                        processedSize: processed.size,
                        fileSize: processedData.count,
                        compressionQuality: "Optimized"
                    )
                }
            }
            self.isProcessing = false
        }
    }
    
    private func formatFileSize(_ bytes: Int) -> String {
        let formatter = ByteCountFormatter()
        formatter.allowedUnits = [.useKB, .useMB]
        formatter.countStyle = .file
        return formatter.string(fromByteCount: Int64(bytes))
    }
}

#Preview {
    ContentView()
}
