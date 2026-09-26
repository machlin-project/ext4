// SPDX-License-Identifier: BSD-3-Clause
import ExtensionFoundation
import FSKit

@main
struct Ext4Extension: UnaryFileSystemExtension {
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
        Ext4FileSystem()
    }
}
