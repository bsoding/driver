The native client API is an ordinary user-mode C++17 library. It communicates with the existing driver using the same version-1 IOCTL protocol. The default transport is the driver. Python is not involved.

**Read by PID and address without a profile.** Build with `./build.ps1 -ClientOnly`. The resulting native command-line program calls this API directly:

```powershell
.\dist\OffsetReader.exe read-memory 1234 0x0000012345678000 --size 16
.\dist\OffsetReader.exe read-memory 1234 0x123456 --module YourApp.exe --type u32 --json
```

Without `--module`, the second argument is an absolute virtual address. With `--module`, it is a signed byte offset from that module's current base address. Decimal and `0x` hexadecimal integers are accepted. The default result is four raw bytes in hexadecimal. `--type` accepts the same scalar, pointer, bytes, and string types as profiles. `--size` sets the byte count for bytes/strings (1 through 65,536); scalar types determine their own size. JSON output includes PID, backend, resolved address, byte count, type, and value. Reads exit 0 on success and 1 on failure. Use `--backend user` only when explicitly testing the ordinary Windows API; the default uses the driver and never falls back.

**Call it from another C++ project.** Add `client/include` to your include path and link `dist/OffsetReaderClient.lib`. Use an x64 MSVC build with the static runtime (`/MT`), matching the supplied library. Copy `dist/Client.compiler.pdb` alongside the library if you want its compiler debug information. You can alternatively compile `client/src/memory.cpp` directly in your application using your own runtime settings; this raw-memory API needs only Windows SDK headers and `kernel32.lib`.

```cpp
#include <offset_reader/memory.hpp>

std::uint32_t ReadCounter(const offset_reader::ProcessMemory& memory,
                          std::uint64_t address)
{
    return memory.ReadValue<std::uint32_t>(address);
}

void Inspect(DWORD pid, std::uint64_t counterAddress)
{
    // Reuse this object for subsequent reads from the same process.
    const offset_reader::ProcessMemory memory(pid);
    const auto counter = ReadCounter(memory, counterAddress);
    const auto bytes = memory.Read(counterAddress, 16);
    // Use counter and bytes in the rest of your application.
}
```

`ProcessMemory` owns its process/device handles and closes them when destroyed. It cannot be copied. Pass it by reference to other parts of your program. `ReadValue<T>` requires a trivially copyable type with the target application's layout. Raw pointers returned from another process are addresses to use in another read, not pointers to dereference locally. `PointerSize()` reports 4 or 8 for the target; `std::uintptr_t` in this x64 client remains 8 bytes even for an x86 target.

Driver calls require the signed driver to be installed and running, and the client to run elevated because of the device ACL. Windows must grant the client `PROCESS_VM_READ` and query access to the target. There is no automatic transport fallback. To explicitly use the ordinary Windows API for diagnostics, construct with `offset_reader::Backend::User`.

Errors throw `std::exception` subclasses. `WindowsError::Code()` exposes the underlying Win32 error code. Short reads fail instead of returning a partially initialized typed value. Each read is limited to 64 KiB and checked against the target pointer width and user-address range.

**Use existing JSON profiles.** Include `offset_reader/profile.hpp`, add `client/third_party` to the include path, and link the same library. If building from source, also compile `client/src/profile.cpp`. The vendored nlohmann JSON library is MIT licensed; retain its license when copying it.

```cpp
#include <offset_reader/profile.hpp>

offset_reader::Json ReadApplication(const std::filesystem::path& profilePath)
{
    const auto profile = offset_reader::LoadProfile(profilePath);
    const DWORD pid = profile.pid
        ? *profile.pid
        : offset_reader::SelectPid(*profile.processName);
    const offset_reader::ProcessMemory memory(
        pid, offset_reader::Backend::Driver,
        profile.processName.value_or(L""));
    return offset_reader::ReadProfile(profile, memory);
}
```

`ReadProfile` resolves module bases and pointer chains and returns an array of named values. Individual value failures have an `error` field; check `HasErrors()` before treating the sample as complete. Setup errors such as a missing process or mismatched target architecture throw exceptions. Module bases are refreshed on each `ReadProfile` call. Pointer-chain reads are separate operations and do not form an atomic snapshot.

The command-line executable is another consumer of this library. `--json` emits one sample object per line, so programs that prefer a subprocess interface can invoke `dist/OffsetReader.exe read PROFILE --json` and check its exit status.

The build also compiles [client/examples/read_value.cpp](../client/examples/read_value.cpp) to check that an independent translation unit can consume the public header. [tests/native_tests.cpp](../tests/native_tests.cpp) links the static library and verifies actual typed reads and complete profile reads against owned processes. Kernel-driver execution remains a separate VM validation step.

Code that already runs inside this driver can instead call the [internal kernel C++ function](KERNEL_INTEGRATION.md), after the existing request authorization step.
