// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI
import Darwin

private func controlResult(_ request: [String: Any], endpoint: URL) throws -> [String: Any] {
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
    let details: String
    let retainReadState: Bool
    var id: String { endpoint.absoluteString }
}

@MainActor
private final class ControlModel: ObservableObject {
    @Published var volumes: [MountedVolume] = []
    @Published var status = "No mounted volumes discovered."
    @Published var busy = false

    func refresh() {
        guard !busy else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
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
                        let identifier = info["volume"] as? String ?? "Unknown volume"
                        let blockSize = (info["blockSize"] as? NSNumber)?.stringValue ?? "Unknown"
                        let blocks = (info["blocks"] as? NSNumber)?.stringValue ?? "Unknown"
                        let freeBlocks = (info["freeBlocks"] as? NSNumber)?.stringValue ?? "Unknown"
                        found.append(MountedVolume(
                            endpoint: endpoint,
                            details: "Volume: \(identifier)\nBlock size: \(blockSize) bytes\nBlocks: \(blocks), free: \(freeBlocks)",
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
                self.volumes = completed
                self.status = message
                self.busy = false
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

@main
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
                            }
                            .disabled(model.busy)
                            Divider()
                        }
                    }
                }
                Text("Block-device mounts currently use read-only mode. Settings affect this mounted instance only.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(24)
            .frame(minWidth: 640, minHeight: 480)
            .onAppear { model.refresh() }
        }
    }
}
