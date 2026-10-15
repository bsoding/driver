Research snapshot: October 15, 2026. This review covers upstream documentation, license files, and selected Microsoft KMDF sample code. No third-party binaries were downloaded, loaded, or tested. Claims about supplied signed drivers are upstream descriptions, not locally verified signatures or compatibility results.

**Can the caller run in the kernel?**

A driver can execute native routines and create system threads that run entirely in kernel mode. Windows documents these as kernel threads without a user-mode context. An existing Python script or ordinary Windows executable cannot simply be switched into that execution environment; the required logic would have to become driver code. See Microsoft's [system-thread documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-pscreatesystemthread).

Our memory-copy operation already executes in the driver. The native C++ client selects the target, opens a process handle, optionally loads profiles, looks up module bases, resolves pointer chains, and presents values. The driver validates the caller's read permission and performs a bounded copy through its [internal C++ function](KERNEL_INTEGRATION.md). These roles are visible in [OffsetReader.cpp](../driver/OffsetReader.cpp), [memory_reader.cpp](../driver/memory_reader.cpp), [profile.cpp](../client/src/profile.cpp), and [memory.cpp](../client/src/memory.cpp). The earlier Python client has been replaced.

Kernel execution is not a promise of invisibility or stability. Microsoft notes that driver defects can affect the entire OS, and recommends considering an application or service when a driver is unnecessary. Keeping parsing, presentation, and general application logic outside the driver is the recommendation for this project. See the [driver security checklist](https://learn.microsoft.com/en-us/windows-hardware/drivers/driversecurity/driver-security-checklist#confirm-that-a-kernel-driver-is-required).

**Projects reviewed**

| Project | What it offers | Fit for this project | Published source license |
| --- | --- | --- | --- |
| [WinPmem](https://github.com/Velocidex/WinPmem) | A Windows physical-memory acquisition driver, raw dumps, a read device interface, and acquisition utilities. | Relevant for forensic acquisition; not a direct replacement for process/module/offset profiles. | [Apache 2.0](https://github.com/Velocidex/WinPmem/blob/master/LICENSE) |
| [MemProcFS](https://github.com/ufrisk/MemProcFS) | A memory-analysis library and virtual filesystem, process/virtual-memory APIs, and bindings for several languages. | The broadest ready-made application integration option reviewed. Its library is not a kernel replacement for a Python caller. | [AGPL 3.0](https://github.com/ufrisk/MemProcFS/blob/master/LICENSE); dependencies can have other terms. |
| [LeechCore](https://github.com/ufrisk/LeechCore) | A physical-memory acquisition library with C/C++, Python, and C# APIs, supporting files and several acquisition sources. | A lower-level acquisition layer used by MemProcFS; additional logic is needed for process-level abstractions. | [GPL 3.0](https://github.com/ufrisk/LeechCore/blob/master/LICENSE) |
| [Microsoft KMDF IOCTL sample](https://github.com/microsoft/Windows-driver-samples/tree/main/general/ioctl/kmdf) | A non-PnP control-device example demonstrating driver/client communication and different IOCTL buffer methods. | The closest structural reference to our current KMDF control driver. It is not an application-memory reader. | [Microsoft Public License](https://github.com/microsoft/Windows-driver-samples/blob/main/LICENSE) |

WinPmem's README advertises Windows 7–10 and x86/x64 support, and describes supplied signed drivers. That description does not establish compatibility with this Windows 11 machine. Its documented architecture deliberately leaves more complex imaging work in user space. See the [upstream description](https://github.com/Velocidex/WinPmem#description).

MemProcFS can analyze memory dumps or use supported live acquisition sources. It exposes its functionality through application APIs without requiring the virtual filesystem to be mounted. Its documented [C API](https://github.com/ufrisk/MemProcFS/wiki/API_C) includes process/module lookup and scatter reads. Those are useful integration and performance features, rather than evidence of invisible execution.

LeechCore's Windows library is a DLL loaded by another application. WinPmem is one supported acquisition source; LeechCore is not itself a drop-in kernel driver. See its [upstream overview](https://github.com/ufrisk/LeechCore#the-leechcore-physical-memory-acquisition-library).

The Microsoft sample illustrates an important buffered-I/O rule: input and output can share the same system buffer. Our implementation already saves and validates the request fields before filling the output. The sample is a reference, not a reason to copy its demonstration permissions into a memory-reading interface. See the [sample source](https://github.com/microsoft/Windows-driver-samples/blob/main/general/ioctl/kmdf/sys/nonpnp.c).

**Integration decision**

No source or binary from these reviewed acquisition projects has been incorporated. The reviewed acquisition stacks expose a substantially broader memory surface than this project's permission-checked process reader. Replacing the driver would expand scope and require fresh signature, compatibility, access-control, and runtime validation. The native C++ client separately uses the MIT-licensed nlohmann JSON parser, documented in [client/third_party/README.md](../client/third_party/README.md).

For the existing offset-reader use case, retain the small driver and its explicit client API. A cleaner language wrapper or batching can be evaluated as ordinary integration work. For a separate forensic-analysis application, MemProcFS is a candidate to evaluate through its documented APIs, beginning with memory dumps. These are architectural recommendations, not implementations or benchmark results.

No process hiding, hidden kernel worker, monitoring bypass, or driver-signing bypass has been added. These projects were reviewed for documented acquisition and analysis features. Their documentation does not establish a universal invisibility guarantee.

Licenses were recorded to make any later reuse reviewable. No assumption that private use permits ignoring upstream terms was used, and no third-party source was copied into the project.

The existing compiled driver remains a development artifact. This research did not establish that it or any reviewed driver is safe to load on the everyday host. The earlier local checks and outstanding VM tests are recorded in [VERIFICATION.md](../VERIFICATION.md).

