// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI
import ServiceManagement
import Darwin

// ServiceManagement requires an unsandboxed installer for an unsandboxed daemon.
// This separate utility only manages that service; the control app stays sandboxed.
private enum DiskService {
    static let service = SMAppService.daemon(plistName: "group.org.machlin.ext4.device-barrier.plist")

    static var status: String {
        switch service.status {
        case .enabled: return "enabled"
        case .requiresApproval: return "requiresApproval"
        case .notRegistered: return "notRegistered"
        case .notFound: return "notFound"
        @unknown default: return "unknown"
        }
    }

    static func register() throws {
        do {
            try service.register()
        } catch {
            // Registration can report EPERM while awaiting normal system approval.
            guard service.status == .requiresApproval else { throw error }
        }
    }
}

private struct SetupView: View {
    @State private var status = DiskService.status

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Enable ext4 disk writing").font(.title)
            Text("The disk service synchronizes device caches so ext4 can commit writes safely. Approve the service in System Settings, then remount the volume.")
            Text(status).foregroundStyle(.secondary)
            HStack {
                Button("Enable service") {
                    do {
                        try DiskService.register()
                        status = DiskService.status
                        if DiskService.service.status == .requiresApproval {
                            SMAppService.openSystemSettingsLoginItems()
                        }
                    } catch {
                        status = error.localizedDescription
                    }
                }
                Button("Open System Settings") { SMAppService.openSystemSettingsLoginItems() }
                Button("Refresh") { status = DiskService.status }
            }
        }
        .padding(24)
        .frame(width: 500)
    }
}

private struct SetupApp: App {
    var body: some Scene {
        WindowGroup("Machlin ext4 Disk Service") { SetupView() }
    }
}

@main
enum DeviceSetup {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        if arguments.isEmpty {
            SetupApp.main()
            return
        }
        do {
            guard arguments.count == 1 else {
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL))
            }
            switch arguments[0] {
            case "--register": try DiskService.register()
            case "--unregister": try DiskService.service.unregister()
            case "--status": break
            default: throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL))
            }
            let data = try JSONSerialization.data(withJSONObject: ["status": DiskService.status])
            FileHandle.standardOutput.write(data + Data([10]))
        } catch {
            let value = error as NSError
            let data = try? JSONSerialization.data(withJSONObject: [
                "error": ["domain": value.domain, "code": value.code, "message": value.localizedDescription]
            ])
            if let data { FileHandle.standardError.write(data + Data([10])) }
            exit(EXIT_FAILURE)
        }
    }
}
