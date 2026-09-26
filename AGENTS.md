# Machlin ext4 development

This repository is the single source of truth for the standalone ext4 filesystem.
The workspace lab policy applies to identity, source history, safe VM operation,
English source text and C formatting. This repository explicitly owns ext4 code,
including its kext adapter; LXNU algorithms and hooks remain in the XNU fork.

Read README.md, docs/ARCHITECTURE.md and docs/ACCEPTANCE.md before changes. Keep
the portable core independent of Foundation, Swift, FSKit, libc allocation,
Darwin errno numbers, vnode internals and LXNU. Shared algorithms compile into
separate userspace and kernel binaries. Do not grow a replacement XNU page cache.

Use named disk fields and ABI constants, bounded arithmetic and explicit little-
or big-endian decoding. Every disk byte is untrusted. Unsupported features must
be identified, never silently approximated. Read-only support must not replay a
journal or otherwise write to the device. Writes require a verified ordering and
durability contract, transactional metadata ownership and recovery tests.

Use the provided .clang-format with the selected Xcode clang-format. C declarations
belong at block starts, before statements; retain initializer side-effect order.
Use braces and one statement per line. Core allocations are explicit environment
operations; avoid large kernel-stack arrays and unbounded recursion.

The main agent owns architecture, filesystem contracts, implementation, test
design, diagnosis, review and final acceptance. Delegate routine prepared builds,
fixtures, tests and log collection to GPT-6 Luna (gpt-6-luna). Delegate VM and
computer operations to GPT-6 Sol (gpt-6-sol). Give exact commands, directories,
limits and success criteria. Only one worker may operate each VM. Keep expensive
builds serialized and use bounded fixture sizes until scale is the test subject.

Standalone driver commands may run from this repository. Machlin build, VM and
acceptance commands run from the absolute lab directory. Use only dedicated
disposable VMs for kernel and installed FSKit tests. Host boot policy, kernel,
NVRAM and system files remain outside scope.

Use the personal GitHub account, repository-local author/committer and signing
configuration. Verify identity before commits and the authenticated account
before GitHub mutations. Remotes use git@github-personal.com. Work on development;
main is a verified baseline. Never copy another repository's .git directory.
Keep generated images, binaries, dependency checkouts and credentials ignored.

Document actual evidence separately for core tests, FSKit builds, stock macOS
mounts, kext builds, custom boots and LXNU semantics. A pass in one layer is not
a pass in another. Full completion requires the acceptance matrix; an unsupported
contract or rejected image is not automatically a passed filesystem feature.
