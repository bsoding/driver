The driver's existing copy operation is available to other C++ translation units in the same driver through [driver/memory_reader.hpp](../driver/memory_reader.hpp):

```cpp
#include "memory_reader.hpp"

// Call at PASSIVE_LEVEL, after the existing request has been authorized.
SIZE_T bytesRead = 0;
const NTSTATUS status = offset_reader::kernel::ReadMemory(
    authorizedProcess, address, nonpagedOutput, outputSize, &bytesRead);
if (NT_SUCCESS(status)) {
    // All outputSize bytes are available in nonpagedOutput.
}
```

`authorizedProcess` is a borrowed, referenced `PEPROCESS`. In this driver, `PrepareReadForCaller` resolves the requesting application's handle in its originating process, using `ObReferenceObjectByHandle` with `UserMode` and `PROCESS_VM_READ`. Windows checks the caller's granted access before the request is queued. Keep that reference alive until every synchronous use of `ReadMemory` has returned. The helper neither acquires nor releases the reference. [Microsoft's handle validation documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obreferenceobjectbyhandle) explains this access check.

The helper runs at `PASSIVE_LEVEL`, accepts 1 through 65,536 bytes, and rejects null, kernel, overflowing, and user-limit-crossing source ranges. The destination must be valid writable nonpaged system memory for the entire range, and `BytesRead` must be a valid kernel pointer. KMDF's buffered output already supplies the required destination for the IOCTL path. Other callers must supply their own suitable buffer; a user-mode pointer is not a substitute. These destination requirements come from [MmCopyMemory](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmcopymemory).

`BytesRead` is zero on failure. Full copies return `STATUS_SUCCESS` and the exact requested count. Failed or short copies clear the destination before returning; buffers are not touched when initial argument or address validation fails. Process attachment covers only the bounded copy and is detached in an SEH `__finally` block. A referenced process object does not guarantee that its address space remains readable: exit, unmapping, and concurrent writes can still cause failures or changing values.

[driver/OffsetReader.cpp](../driver/OffsetReader.cpp) calls this helper from the existing IOCTL handler. It retains the caller-context permission check, administrator/SYSTEM device ACL, request cleanup, and version-1 wire protocol. The internal function is an ordinary linked C++ function within the driver image; it adds no device command or exported driver entry point.

Build with `./build.ps1 -DriverOnly -Analyze`. The script compiles both kernel sources as C++17 with MSVC `/kernel`, `/GR-`, and `/W4 /WX`, and links the implementation into `OffsetReader.sys`. It also compiles [driver/examples/read_value.cpp](../driver/examples/read_value.cpp) as an independent consumer of the header. No standard library, C++ runtime, allocation, RTTI, or C++ exception handling is used in the kernel code. See Microsoft's [/kernel documentation](https://learn.microsoft.com/en-us/cpp/build/reference/kernel-create-kernel-mode-binary?view=msvc-170) for the supported C++ subset.

For a user-mode C++ caller, use [CPP_INTEGRATION.md](CPP_INTEGRATION.md) instead. Build and source-analysis results are separate from live kernel validation; see [VERIFICATION.md](../VERIFICATION.md).
