#include "offset_reader/memory.hpp"
#include <TlHelp32.h>
#include <winioctl.h>
#include "../../driver/protocol.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace offset_reader {
static_assert(sizeof(void*) == 8, "Build the client for x64");
static_assert(MaxRead == OR_MAX_READ_SIZE);
static_assert(sizeof(DriverInfo) == sizeof(OR_INFO));

std::wstring ToWide(const std::string& text, bool strict) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::runtime_error("Text exceeds Windows conversion limits");
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int count = MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw WindowsError("Decode UTF-8", GetLastError());
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()), result.data(), count))
        throw WindowsError("Decode UTF-8", GetLastError());
    return result;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::runtime_error("Text exceeds Windows conversion limits");
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Encode UTF-8 failed");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr))
        throw std::runtime_error("Encode UTF-8 failed");
    return result;
}

static std::string ErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (!count) return "Windows error " + std::to_string(code);
    const std::wstring text(buffer, count);
    LocalFree(buffer);
    auto result = ToUtf8(text);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) result.pop_back();
    return result;
}
WindowsError::WindowsError(const std::string& context, DWORD code)
    : std::runtime_error(context + ": " + ErrorText(code) + " (" + std::to_string(code) + ")"), code_(code) {}

Handle::~Handle() { if (*this) CloseHandle(handle_); }
Handle::Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
Handle& Handle::operator=(Handle&& other) noexcept {
    if (this != &other) {
        if (*this) CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}
bool EqualName(const std::wstring& a, const std::wstring& b) {
    if (a.size() > INT_MAX || b.size() > INT_MAX) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::wstring LowerName(const std::wstring& value) {
    if (value.empty()) return {};
    if (value.size() > INT_MAX) throw std::runtime_error("Name is too long");
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count) throw WindowsError("Normalize name", GetLastError());
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr, 0)) throw WindowsError("Normalize name", GetLastError());
    return result;
}
std::string Hex(std::uint64_t value, unsigned width) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << value;
    return stream.str();
}
void CheckAddress(std::uint64_t address, std::size_t size, unsigned pointerSize) {
    if (pointerSize != 4 && pointerSize != 8) throw std::runtime_error("Pointer size must be 4 or 8");
    const auto limit = pointerSize == 4 ? 0xFFFFFFFFULL : MaxUserAddress;
    if (!size || size > MaxRead || !address || address > limit || size - 1 > limit - address)
        throw std::runtime_error("Invalid user memory range: " + Hex(address) + ", size=" + std::to_string(size));
}
static Handle Snapshot(DWORD flags, DWORD pid) {
    for (unsigned attempt = 0; attempt != 8; ++attempt) {
        Handle result(CreateToolhelp32Snapshot(flags, pid));
        if (result) return result;
        const auto error = GetLastError();
        if (error != ERROR_BAD_LENGTH || attempt == 7) throw WindowsError("Create process/module snapshot", error);
        Sleep(10);
    }
    throw std::runtime_error("Snapshot retry limit reached");
}
std::vector<ProcessInfo> Processes() {
    const auto snapshot = Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<ProcessInfo> result;
    auto more = Process32FirstW(snapshot.Get(), &entry);
    while (more) {
        result.push_back({entry.th32ProcessID, entry.szExeFile});
        more = Process32NextW(snapshot.Get(), &entry);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) throw WindowsError("Enumerate processes", GetLastError());
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        const auto left = LowerName(a.name), right = LowerName(b.name);
        return left == right ? a.pid < b.pid : left < right;
    });
    return result;
}
std::vector<ModuleInfo> Modules(DWORD pid) {
    const auto snapshot = Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<ModuleInfo> result;
    auto more = Module32FirstW(snapshot.Get(), &entry);
    while (more) {
        result.push_back({entry.szModule, reinterpret_cast<std::uint64_t>(entry.modBaseAddr), entry.modBaseSize, entry.szExePath});
        more = Module32NextW(snapshot.Get(), &entry);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) throw WindowsError("Enumerate modules", GetLastError());
    return result;
}
DWORD SelectPid(const std::wstring& name) {
    std::vector<DWORD> matches;
    for (const auto& process : Processes()) if (EqualName(process.name, name)) matches.push_back(process.pid);
    if (matches.empty()) throw std::runtime_error("Process is not running: " + ToUtf8(name));
    if (matches.size() != 1) {
        std::string message = "Multiple processes named " + ToUtf8(name) + "; select --pid:";
        for (const auto pid : matches) message += " " + std::to_string(pid);
        throw std::runtime_error(message);
    }
    return matches.front();
}
DriverDevice::DriverDevice()
    : handle_(CreateFileW(L"\\\\.\\OffsetReader", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)) {
    if (!handle_) throw WindowsError("Open OffsetReader (run elevated and start the signed driver)", GetLastError());
    const auto response = Request(IOCTL_OR_INFO, nullptr, 0, sizeof(OR_INFO));
    std::memcpy(&info_, response.data(), sizeof(info_));
    if (info_.version != OR_PROTOCOL_VERSION || info_.maxRead != MaxRead || info_.pointerSize != 8 || info_.reserved)
        throw std::runtime_error("Driver protocol mismatch; use client and driver from the same project");
}
std::vector<std::uint8_t> DriverDevice::Request(DWORD code, const void* input, DWORD inputSize, DWORD outputSize) const {
    if (outputSize > MaxRead) throw std::runtime_error("Driver output exceeds 64 KiB");
    std::vector<std::uint8_t> output(outputSize);
    DWORD returned = 0;
    if (!DeviceIoControl(handle_.Get(), code, const_cast<void*>(input), inputSize, output.data(), outputSize, &returned, nullptr)) {
        const auto error = GetLastError();
        throw WindowsError("Driver request " + Hex(code), error);
    }
    if (returned != outputSize) throw std::runtime_error("Short driver response: " + std::to_string(returned) + "/" + std::to_string(outputSize));
    return output;
}
ProcessMemory::ProcessMemory(DWORD pid, Backend backend, const std::wstring& expectedName)
    : process_(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)), backend_(backend), pid_(pid) {
    if (!process_) {
        const auto error = GetLastError();
        throw WindowsError("Open PID " + std::to_string(pid) + " with read access", error);
    }
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process_.Get(), 0, image.data(), &length)) throw WindowsError("Query process image", GetLastError());
    image.resize(length);
    name_ = image.substr(image.find_last_of(L"\\/") + 1);
    if (!expectedName.empty() && !EqualName(expectedName, name_)) throw std::runtime_error("PID changed: expected " + ToUtf8(expectedName) + ", found " + ToUtf8(name_));
    USHORT machine = 0, nativeMachine = 0;
    if (!IsWow64Process2(process_.Get(), &machine, &nativeMachine)) throw WindowsError("Detect target architecture", GetLastError());
    if (nativeMachine != IMAGE_FILE_MACHINE_AMD64 || (machine != IMAGE_FILE_MACHINE_UNKNOWN && machine != IMAGE_FILE_MACHINE_I386))
        throw std::runtime_error("Use Windows x64 with x64 or x86 target applications");
    pointerSize_ = machine == IMAGE_FILE_MACHINE_I386 ? 4 : 8;
    if (backend == Backend::Driver) device_ = std::make_unique<DriverDevice>();
}
ProcessMemory::~ProcessMemory() = default;
std::vector<std::uint8_t> ProcessMemory::Read(std::uint64_t address, std::size_t size) const {
    CheckAddress(address, size, pointerSize_);
    if (device_) {
        const OR_READ_REQUEST request{OR_PROTOCOL_VERSION, 0, reinterpret_cast<ULONGLONG>(process_.Get()), address,
            static_cast<ULONG>(size), 0};
        return device_->Request(IOCTL_OR_READ, &request, sizeof(request), static_cast<DWORD>(size));
    }
    if (backend_ != Backend::User) throw std::runtime_error("Driver transport is unavailable");
    std::vector<std::uint8_t> output(size);
    SIZE_T transferred = 0;
    if (!ReadProcessMemory(process_.Get(), reinterpret_cast<const void*>(address), output.data(), size, &transferred)) {
        const auto error = GetLastError();
        throw WindowsError("Read " + std::to_string(size) + " bytes at " + Hex(address), error);
    }
    if (transferred != size) throw std::runtime_error("Short user-mode read at " + Hex(address));
    return output;
}
bool ProcessMemory::IsRunning() const {
    DWORD code = 0;
    if (!GetExitCodeProcess(process_.Get(), &code)) throw WindowsError("Query process exit status", GetLastError());
    return code == STILL_ACTIVE;
}
}

