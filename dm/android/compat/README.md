# Apple API compatibility for the Android host

The host was written against macOS / iOS. On Android, this folder is first on the include path and provides the subset of
Apple's system APIs the host uses (Mach time and memory calls, QoS, libkern cache control, dispatch semaphores, malloc zones,
CommonCrypto's SHA-256), implemented with Linux / bionic in `compat.c`. Headers carry Apple's names so shared host code needs no
`#ifdef` for these; behaviour that has no Linux counterpart (thread suspension, Mach regions, `vm_remap`) returns `KERN_FAILURE`
and the callers' existing failure paths take over. Frameworks (AudioToolbox, VideoToolbox, Metal) are not imitated here: their
callers have Android code of their own.
