// SPDX-License-Identifier: BSD-3-Clause
import SwiftUI

@main
struct MachlinExt4App: App {
    var body: some Scene {
        WindowGroup("Machlin ext4") {
            VStack(alignment: .leading, spacing: 16) {
                Text("Machlin ext4").font(.largeTitle)
                Text("Read ext4 disks on macOS.").font(.title2)
                Text("Enable Machlin ext4 in System Settings → General → Login Items & Extensions → File System Extensions.")
                Text("This development build supports clean ext4 volumes in read-only mode.")
                    .foregroundStyle(.secondary)
            }
            .padding(32)
            .frame(width: 520)
        }
        .windowResizability(.contentSize)
    }
}
