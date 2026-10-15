OffsetReader is a Windows x64 KMDF driver with a native C++ client and reusable C++ library for configurable application memory reads. Each application has a JSON profile with an executable name or PID, module names, offsets, pointer chains, and value types. Changing a profile does not require rebuilding the driver. Running, building, testing, and packaging the project do not require Python.

The compiled development driver is **[dist/OffsetReader.sys](dist/OffsetReader.sys)**. It is unsigned. It has been compiled, but has not been loaded or tested in kernel mode. The profile engine and explicit user-mode transport have been tested against a real demo process. See [VERIFICATION.md](VERIFICATION.md) for the evidence and remaining checks.

[dist/OffsetReader.exe](dist/OffsetReader.exe) is the native command-line client. The repository includes that executable and the unsigned driver. Building creates the demo, tests, C++ library, and debug symbols. Running `.\scripts\package.ps1` after a complete build creates `dist/OffsetReader-starter.zip` with those artifacts, source, docs, and setup scripts, excluding the local toolchain download.

To integrate this into another C++ application, start with [docs/CPP_INTEGRATION.md](docs/CPP_INTEGRATION.md), the [public API](client/include/offset_reader/memory.hpp), and `dist/OffsetReaderClient.lib` after building. Other code inside the same driver can call the [internal C++ read function](docs/KERNEL_INTEGRATION.md).

[docs/DRIVER_RESEARCH.md](docs/DRIVER_RESEARCH.md) compares WinPmem, MemProcFS, LeechCore, and Microsoft's KMDF IOCTL sample, including their fit, licenses, and the distinction between kernel execution and process hiding.

The driver checks a process handle already opened by the reader with `PROCESS_VM_READ`. Windows process permissions still apply. Its device interface allows administrators and SYSTEM; reads are limited to user virtual addresses and 64 KiB per request. The design follows Microsoft's [handle validation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obreferenceobjectbyhandle) and [driver access constraints](https://learn.microsoft.com/en-us/windows-hardware/drivers/driversecurity/driver-security-dev-best-practices).

**Read directly by PID and offset.** With the signed driver running, use an elevated terminal:

```powershell
# Absolute virtual address; return 16 bytes as hex.
.\dist\OffsetReader.exe read-memory 1234 0x0000012345678000 --size 16

# Current module base + offset; return an unsigned 32-bit value.
.\dist\OffsetReader.exe read-memory 1234 0x123456 --module YourApp.exe --type u32 --json
```

Replace the example PID and address/offset with your target's actual values. Without `--module`, the address is absolute. With `--module`, the offset is added to the module's current base, accommodating ASLR. The default is four raw bytes; `--type` decodes a value using the types listed below. Byte/string reads accept `--size` from 1 to 65,536 bytes. The command returns the value to standard output, or a JSON object with `--json`, and exits 0 on success or 1 on failure. It needs no profile file and never falls back from the driver to user-mode reads.

**Try the native client.** Use Windows 10 version 1709 or newer, or Windows 11, on x64. The supplied executables include the C++ runtime statically. Run these commands from this folder after `./build.ps1 -ClientOnly` creates the demo and tests.

In one terminal, start the included target and leave it running:

```powershell
.\dist\OffsetReaderDemo.exe
```

In a second terminal:

```powershell
.\dist\OffsetReader.exe read profiles\demo.generated.json --backend user
```

Expected values include `counter = 123456789`, `ratio = 3.25`, `label = OffsetReader demo`, a Unicode label, and `module_signature = 23117` (the executable's MZ header). The generated profile belongs to that specific demo run. This command explicitly uses `ReadProcessMemory`; it verifies the profiles before the kernel driver is installed. The default backend is `driver`, and a driver connection failure returns an error.

**Configure your applications.** Copy [profiles/example.json](profiles/example.json) for each application and replace the example offsets with real offsets for the build you are inspecting. They are placeholders, and the reader does not discover offsets automatically.

```json
{
  "schema_version": 1,
  "process": { "name": "YourApp.exe" },
  "module": "YourApp.exe",
  "pointer_size": "auto",
  "values": [
    { "name": "counter", "type": "u32", "offsets": ["0x123456"] },
    { "name": "nested_value", "type": "f32", "offsets": ["0x234560", "0x10", "0x28"] }
  ]
}
```

A single offset reads at `moduleBase + offset`. A chain `[a, b, c]` reads a pointer at `moduleBase + a`, adds `b`, reads the next pointer, adds `c`, then reads the final value. The last address is not dereferenced again. Pointer width is detected from the target process, so x86 and x64 applications can use the same x64 driver. Set `pointer_size` to `4` or `8` if you want a mismatch to be reported explicitly. Decimal integers, hex strings, and negative offsets such as `"-0x10"` are supported.

Use a value's `module` field to select a different DLL. Use `address: "0x..."` instead of a module to start at an absolute address; offsets are optional in that case. Module bases are looked up at runtime to accommodate ASLR. Absolute addresses and application offsets can change after a restart or update. Pointer chains are separate reads, so data that changes concurrently is not an atomic snapshot.

| Type | Meaning |
| --- | --- |
| `i8`, `i16`, `i32`, `i64` | Signed integer |
| `u8`, `u16`, `u32`, `u64` | Unsigned integer |
| `f32`, `f64` | Floating point |
| `bool` | One byte, zero/nonzero |
| `pointer` | Target-width pointer, displayed in hex |
| `bytes` | Hex bytes; requires `length` in bytes |
| `utf8`, `utf16` | Null-terminated text within `length` bytes; UTF-16 length must be even |

Useful commands:

```powershell
.\dist\OffsetReader.exe validate profiles\example.json
.\dist\OffsetReader.exe processes --name YourApp
.\dist\OffsetReader.exe modules 1234
.\dist\OffsetReader.exe read profiles\your-app.json --pid 1234 --backend user --json
.\dist\OffsetReader.exe read profiles\your-app.json --watch 0.25
```

Use `{ "process": { "pid": 1234 } }` in a profile for a fixed PID, or override any profile with `--pid`. If several processes share an executable name, the reader asks you to choose a PID. `--json` emits one JSON object per sample and includes the selected backend. Unreadable values contain an `error`; they are not replaced with zero. A one-shot read exits `0` on success, `2` if any configured value fails, and `1` for setup/configuration errors. Polling accepts intervals from 0.05 to 86400 seconds. Ctrl+C stops polling.

**Build the C++ client and driver.** Install Visual Studio C++ tools and Windows SDK build 26100 if needed. Building only the native client requires no WDK. The JSON dependency is vendored at a pinned version with its [MIT license and hashes](client/third_party/README.md).

```powershell
.\build.ps1 -Check
.\build.ps1 -ClientOnly -Test
```

To also build the kernel driver, the build can download Microsoft's pinned [WDK NuGet package](https://www.nuget.org/packages/Microsoft.Windows.WDK.x64/10.0.26100.6584) into `.tools`; the archive's SHA-256 is checked before extraction. On this machine, that local WDK is already present.

```powershell
.\build.ps1 -RestoreWdk
```

Subsequent complete builds use `./build.ps1`; `-DriverOnly` builds only the driver. Native output includes `OffsetReader.exe`, `OffsetReaderDemo.exe`, `OffsetReaderTests.exe`, and `OffsetReaderClient.lib` in `dist`. The driver output is `dist/OffsetReader.sys` and its debug symbols. It targets KMDF 1.15 and Windows 10/11 x64. Building does not install or start it.

Use `./build.ps1 -Analyze` to run MSVC analysis on the driver source as part of the build. External WDK/SDK header implementations are excluded from that analysis. Native client source builds with `/W4 /WX` and a static C++ runtime (`/MT`).

**Load it in a Windows test VM.** Kernel code can crash the test system, so use a VM snapshot for development. Windows x64 requires driver signing. Microsoft's [test-signing documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) describes enabling test mode and the required restart. Test signing and Secure Boot configuration belong to the VM. No boot settings, trusted certificates, services, or security settings have been changed by this project build.

For a disposable Windows test VM, the following is a concrete signing path. Run in an elevated PowerShell inside the VM after copying the project there and installing Windows SDK signing tools. These commands create a test certificate, trust its public certificate inside that VM, and sign the driver:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=OffsetReader Test' -CertStoreLocation Cert:\LocalMachine\My
New-Item -ItemType Directory -Force .\build | Out-Null
Export-Certificate -Cert $cert -FilePath .\build\OffsetReader-Test.cer
Import-Certificate -FilePath .\build\OffsetReader-Test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\build\OffsetReader-Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
$signTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
& $signTool sign /v /fd SHA256 /sm /s My /sha1 $cert.Thumbprint .\dist\OffsetReader.sys
```

Enable test mode inside that VM using `bcdedit /set testsigning on`, then restart the VM. Secure Boot policy can block that setting; configure the VM's firmware as described by Microsoft. After the restart, these commands install a demand-start service and load the signed driver:

```powershell
.\scripts\manage-driver.ps1 -Action Install
.\scripts\manage-driver.ps1 -Action Start
.\dist\OffsetReader.exe status
.\dist\OffsetReaderTests.exe --driver-vm
```

With the demo target running, read it through the actual driver:

```powershell
.\dist\OffsetReader.exe read profiles\demo.generated.json --json
```

To unload and remove the development service, close readers first:

```powershell
.\scripts\manage-driver.ps1 -Action Stop
.\scripts\manage-driver.ps1 -Action Remove
```

You can restore your VM snapshot after testing, or disable test mode with `bcdedit /set testsigning off` and restart. Public distribution requires the applicable [Microsoft driver-signing process](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy); this build is a development artifact.

**Check the source.** [driver/OffsetReader.cpp](driver/OffsetReader.cpp) contains the KMDF control device and caller-context handle validation. [driver/memory_reader.cpp](driver/memory_reader.cpp) implements the reusable internal C++ read function. [driver/protocol.h](driver/protocol.h) defines the unchanged IOCTL wire format. [client/src/profile.cpp](client/src/profile.cpp) implements profiles and pointer chains; [client/src/memory.cpp](client/src/memory.cpp) implements Windows process/module discovery and the two explicit transports. [client/src/main.cpp](client/src/main.cpp) is the native CLI entry point.

```powershell
.\dist\OffsetReaderTests.exe
```

The separate VM smoke suite additionally checks kernel reads, denied query-only process handles, malformed requests, buffer sizes, and invalid addresses. It requires an already loaded driver and performs no driver installation itself. It does not replace Driver Verifier, HVCI testing, or broader driver validation.

