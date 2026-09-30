// SPDX-License-Identifier: BSD-3-Clause
import Foundation
import FSKit
import Darwin

/// Runs inside the signed app so command-line and GUI clients use the same sandbox,
/// App Group, Keychain identity and authenticated control transport.
enum ControlCommand {
    private static let usage = """
    Usage: Machlin ext4 --control modules
           Machlin ext4 --control list
           Machlin ext4 --control request ENDPOINT COMMAND [ARGUMENTS_JSON]
           Machlin ext4 --control keys VOLUME_UUID
           Machlin ext4 --control import-key VOLUME_UUID FILE_OR_DASH [V1_DESCRIPTOR]
           Machlin ext4 --control remove-key VOLUME_UUID KEY_IDENTIFIER
    Use '-' to read exactly 64 binary bytes and EOF from stdin. File paths must
    already be readable inside the app sandbox. Key changes take
    effect on the next mount. Output never includes master keys or IPC tokens.
    """

    private static func writeJSON(_ value: Any, to handle: FileHandle = .standardOutput) throws {
        var data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
        data.append(0x0a)
        try handle.write(contentsOf: data)
    }

    private static func fail(_ error: Error) -> Never {
        let error = error as NSError
        try? writeJSON([
            "error": ["domain": error.domain, "code": error.code,
                      "message": error.localizedDescription]
        ], to: .standardError)
        exit(EXIT_FAILURE)
    }

    private static func endpoints() throws -> [URL] {
        guard let directory = FileManager.default.containerURL(
            forSecurityApplicationGroupIdentifier: Ext4AppGroup
        ) else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EACCES), userInfo: [
                NSLocalizedDescriptionKey: "App Group container is unavailable."
            ])
        }
        return Ext4ControlClient.endpoints(inDirectory: directory).sorted { $0.path < $1.path }
    }

    private static func volume(_ value: String) throws -> UUID {
        guard let result = UUID(uuidString: value) else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL), userInfo: [
                NSLocalizedDescriptionKey: "Invalid volume UUID."
            ])
        }
        return result
    }

    static func run(_ arguments: [String]) -> Never {
        guard let command = arguments.first else {
            fputs(usage + "\n", stderr)
            exit(EX_USAGE)
        }
        do {
            let result: Any
            switch (command, arguments.count) {
            case ("modules", 1):
                // Keep the main run loop free for FSKit's asynchronous completion.
                DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
                    fail(NSError(domain: NSPOSIXErrorDomain, code: Int(ETIMEDOUT)))
                }
                FSClient.shared.fetchInstalledExtensions { modules, error in
                    if let error { fail(error) }
                    guard let modules else {
                        fail(NSError(domain: NSPOSIXErrorDomain, code: Int(EIO)))
                    }
                    do {
                        try writeJSON(modules.filter {
                            $0.bundleIdentifier == "org.machlin.ext4.filesystem"
                        }.map {
                            ["bundleIdentifier": $0.bundleIdentifier,
                             "enabled": $0.isEnabled, "path": $0.url.path] as [String: Any]
                        })
                    } catch { fail(error) }
                    exit(EXIT_SUCCESS)
                }
                dispatchMain()
            case ("list", 1):
                result = try endpoints().map { endpoint in
                    ["endpoint": endpoint.lastPathComponent,
                     "info": try controlResult(["command": "getInfo"], endpoint: endpoint),
                     "settings": try controlResult(["command": "getSettings"], endpoint: endpoint)
                    ] as [String: Any]
                }
            case ("request", 3), ("request", 4):
                guard let endpoint = try endpoints().first(where: {
                    $0.lastPathComponent == arguments[1]
                }) else {
                    throw NSError(domain: NSPOSIXErrorDomain, code: Int(ENOENT))
                }
                var request: [String: Any] = ["command": arguments[2]]
                if arguments.count == 4 {
                    guard arguments[3].utf8.count <= 4096,
                          let values = try JSONSerialization.jsonObject(
                            with: Data(arguments[3].utf8)
                          ) as? [String: Any] else {
                        throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL))
                    }
                    request["arguments"] = values
                }
                result = try controlResult(request, endpoint: endpoint)
            case ("keys", 2):
                result = ["keys": try Ext4KeyStore.keys(forVolume: volume(arguments[1]))]
            case ("import-key", 3), ("import-key", 4):
                let identifier: String
                let volumeID = try volume(arguments[1])
                let descriptor = arguments.count == 4 ? arguments[3] : ""
                if arguments[2] == "-" {
                    guard isatty(STDIN_FILENO) == 0 else {
                        throw NSError(domain: NSPOSIXErrorDomain, code: Int(EINVAL), userInfo: [
                            NSLocalizedDescriptionKey: "Redirect a binary key file into stdin."
                        ])
                    }
                    identifier = try Ext4KeyStore.importKey(
                        fromFileDescriptor: STDIN_FILENO, volume: volumeID,
                        v1Descriptor: descriptor
                    )
                } else {
                    identifier = try Ext4KeyStore.importKey(
                        from: URL(fileURLWithPath: arguments[2]), volume: volumeID,
                        v1Descriptor: descriptor
                    )
                }
                result = ["identifier": identifier, "appliesOnNextMount": true]
            case ("remove-key", 3):
                try Ext4KeyStore.removeKey(arguments[2], volume: volume(arguments[1]))
                result = ["removed": true, "appliesOnNextMount": true]
            default:
                fputs(usage + "\n", stderr)
                exit(EX_USAGE)
            }
            try writeJSON(result)
            exit(EXIT_SUCCESS)
        } catch { fail(error) }
    }
}

@main
@MainActor
enum MachlinExt4Main {
    static func main() {
        if CommandLine.arguments.dropFirst().first == "--control" {
            ControlCommand.run(Array(CommandLine.arguments.dropFirst(2)))
        }
        MachlinExt4App.main()
    }
}
