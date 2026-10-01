// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI
import Darwin
import AppKit
import ServiceManagement

func controlResult(_ request: [String: Any], endpoint: URL) throws -> [String: Any] {
    let response = try Ext4ControlClient.request(request, endpoint: endpoint)
    if let error = response["error"] as? [String: Any] {
        throw NSError(
            domain: NSPOSIXErrorDomain,
            code: (error["code"] as? NSNumber)?.intValue ?? Int(EIO),
            userInfo: [NSLocalizedDescriptionKey: error["message"] as? String ?? "Control command failed."]
        )
    }
    guard let result = response["result"] as? [String: Any] else {
        throw NSError(domain: NSPOSIXErrorDomain, code: Int(EPROTO), userInfo: nil)
    }
    return result
}

private struct MountedVolume: Identifiable {
    let endpoint: URL
    let volumeID: UUID
    let keys: [String]
    let loadedKeys: Int
    let details: String
    let retainReadState: Bool
    var id: String { endpoint.absoluteString }
}

@MainActor
private final class ControlModel: ObservableObject {
    @Published var volumes: [MountedVolume] = []
    @Published var status = "No mounted volumes discovered."
    @Published var busy = false
    @Published var deviceServiceStatus = "Checking…"

    func openExtensionSettings() {
        if !Ext4OpenFileSystemExtensionsSettings() {
            SMAppService.openSystemSettingsLoginItems()
        }
        status = "Open File System Extensions, enable Machlin ext4, then mount the volume."
    }

    func enableWriting() {
        do {
            try DeviceService.register()
            status = "Complete disk service setup, then remount the volume to enable writing."
        } catch {
            status = error.localizedDescription
        }
    }

    func refresh() {
        guard !busy else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let serviceStatus = DeviceService.status()
            var found: [MountedVolume] = []
            var failures: [String] = []
            let directory = FileManager.default.containerURL(
                forSecurityApplicationGroupIdentifier: Ext4AppGroup
            )
            if let directory {
                for endpoint in Ext4ControlClient.endpoints(inDirectory: directory) {
                    do {
                        let info = try controlResult(["command": "getInfo"], endpoint: endpoint)
                        let settings = try controlResult(["command": "getSettings"], endpoint: endpoint)
                        guard let identifier = info["volume"] as? String,
                              let volumeID = UUID(uuidString: identifier) else {
                            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EPROTO))
                        }
                        var keys: [String] = []
                        do {
                            keys = try Ext4KeyStore.keys(forVolume: volumeID)
                        } catch {
                            failures.append("Saved encryption keys are unavailable: " + error.localizedDescription)
                        }
                        let blockSize = (info["blockSize"] as? NSNumber)?.stringValue ?? "Unknown"
                        let blocks = (info["blocks"] as? NSNumber)?.stringValue ?? "Unknown"
                        let freeBlocks = (info["freeBlocks"] as? NSNumber)?.stringValue ?? "Unknown"
                        found.append(MountedVolume(
                            endpoint: endpoint,
                            volumeID: volumeID,
                            keys: keys,
                            loadedKeys: (info["loadedKeys"] as? NSNumber)?.intValue ?? 0,
                            details: "Mode: \(info["readOnly"] as? Bool == false ? "Read-write" : "Read-only")\nVolume: \(identifier)\nBlock size: \(blockSize) bytes\nBlocks: \(blocks), free: \(freeBlocks)",
                            retainReadState: settings["retainReadState"] as? Bool ?? true
                        ))
                    } catch {
                        failures.append(error.localizedDescription)
                    }
                }
            } else {
                failures.append("App Group container is unavailable. Check the app and extension provisioning profiles.")
            }
            let completed = found
            let message = failures.first ?? (found.isEmpty
                ? "Mount an ext4 volume, then refresh. Only active extension instances appear here."
                : "\(found.count) active volume(s).")
            DispatchQueue.main.async {
                self.deviceServiceStatus = serviceStatus
                self.volumes = completed
                self.status = message
                self.busy = false
            }
        }
    }

    func importKey(volume: MountedVolume) {
        guard !busy else { return }
        let panel = NSOpenPanel()
        panel.title = "Import an fscrypt master key"
        panel.message = "Select a file containing the 64-byte raw master key. Password-protected fscrypt key files are not supported."
        let descriptor = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        descriptor.placeholderString = "Leave empty for fscrypt v2"
        let accessory = NSStackView(views: [
            NSTextField(labelWithString: "fscrypt v1 descriptor (16 hexadecimal characters):"), descriptor
        ])
        accessory.orientation = .vertical
        accessory.alignment = .leading
        panel.accessoryView = accessory
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.resolvesAliases = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let v1Descriptor = descriptor.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        changeKeys {
            _ = try Ext4KeyStore.importKey(from: url, volume: volume.volumeID, v1Descriptor: v1Descriptor)
        }
    }

    func removeKey(_ key: String, volume: MountedVolume) {
        changeKeys {
            try Ext4KeyStore.removeKey(key, volume: volume.volumeID)
        }
    }

    private func changeKeys(_ operation: @escaping () throws -> Void) {
        guard !busy else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            var failure: String?
            do {
                try operation()
            } catch {
                failure = error.localizedDescription
            }
            let message = failure
            DispatchQueue.main.async {
                self.busy = false
                if let message {
                    self.status = message
                } else {
                    self.refresh()
                }
            }
        }
    }

    func command(_ command: String, volume: MountedVolume, arguments: [String: Any] = [:]) {
        guard !busy else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            var failure: String?
            do {
                _ = try controlResult(
                    ["command": command, "arguments": arguments], endpoint: volume.endpoint
                )
            } catch {
                failure = error.localizedDescription
            }
            let message = failure
            DispatchQueue.main.async {
                self.busy = false
                if let message {
                    self.status = message
                } else {
                    self.refresh()
                }
            }
        }
    }
}

struct MachlinExt4App: App {
    @StateObject private var model = ControlModel()

    var body: some Scene {
        WindowGroup("Machlin ext4") {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("Machlin ext4").font(.largeTitle)
                    Spacer()
                    Button("Refresh") { model.refresh() }.disabled(model.busy)
                }
                Text("Enable the extension in System Settings → General → Login Items & Extensions → File System Extensions.")
                HStack {
                    Button("Open extension settings…") { model.openExtensionSettings() }.disabled(model.busy)
                    Button("Enable disk writing…") { model.enableWriting() }.disabled(model.busy)
                    Text("Disk service: \(model.deviceServiceStatus)").foregroundStyle(.secondary)
                }
                Text(model.status).foregroundStyle(.secondary)
                ScrollView {
                    VStack(alignment: .leading, spacing: 24) {
                        ForEach(model.volumes) { volume in
                            VStack(alignment: .leading, spacing: 12) {
                                Text(volume.details).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                                Toggle("Retain validated read metadata", isOn: Binding(
                                    get: { volume.retainReadState },
                                    set: { model.command("setSettings", volume: volume, arguments: ["retainReadState": $0]) }
                                ))
                                Button("Release read metadata") {
                                    model.command("dropReadState", volume: volume)
                                }
                                DisclosureGroup("Encryption keys (\(volume.loadedKeys) loaded)") {
                                    VStack(alignment: .leading, spacing: 8) {
                                        Text("Keys are stored in Keychain. Import or removal takes effect after unmounting and mounting the volume again; removal does not lock the current mount.")
                                            .font(.caption)
                                        ForEach(volume.keys, id: \.self) { key in
                                            HStack {
                                                Text(key).font(.system(.caption, design: .monospaced))
                                                Button("Remove saved key") { model.removeKey(key, volume: volume) }
                                            }
                                        }
                                        Button("Import raw fscrypt key…") { model.importKey(volume: volume) }
                                    }
                                }
                            }
                            .disabled(model.busy)
                            Divider()
                        }
                    }
                }
                Text("Writing requires the approved disk service and writable media. Settings affect this mounted instance only.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(24)
            .frame(minWidth: 640, minHeight: 480)
            .onAppear { model.refresh() }
        }
    }
}
