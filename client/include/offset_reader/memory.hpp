#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace offset_reader {
inline constexpr std::size_t MaxRead = 65536;
inline constexpr std::uint64_t MaxUserAddress = 0x00007FFFFFFFFFFFULL;
enum class Backend { Driver, User };

class WindowsError : public std::runtime_error {
public:
    WindowsError(const std::string& context, DWORD code);
    DWORD Code() const noexcept { return code_; }
private:
    DWORD code_;
};

class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~Handle();
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    HANDLE Get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

std::string ToUtf8(const std::wstring& text);
std::wstring ToWide(const std::string& text, bool strict = true);
bool EqualName(const std::wstring& a, const std::wstring& b);
std::wstring LowerName(const std::wstring& value);
std::string Hex(std::uint64_t value, unsigned width = 0);
void CheckAddress(std::uint64_t address, std::size_t size, unsigned pointerSize);

struct ProcessInfo { DWORD pid; std::wstring name; };
struct ModuleInfo { std::wstring name; std::uint64_t base; DWORD size; std::wstring path; };
std::vector<ProcessInfo> Processes();
std::vector<ModuleInfo> Modules(DWORD pid);
DWORD SelectPid(const std::wstring& name);

struct DriverInfo { std::uint32_t version, maxRead, pointerSize, reserved; };
class DriverDevice {
public:
    DriverDevice();
    const DriverInfo& Info() const noexcept { return info_; }
    std::vector<std::uint8_t> Request(DWORD code, const void* input, DWORD inputSize, DWORD outputSize) const;
private:
    Handle handle_;
    DriverInfo info_{};
};

class ProcessMemory {
public:
    explicit ProcessMemory(DWORD pid, Backend backend = Backend::Driver,
                           const std::wstring& expectedName = {});
    ~ProcessMemory();
    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;
    std::vector<std::uint8_t> Read(std::uint64_t address, std::size_t size) const;
    template<class T> T ReadValue(std::uint64_t address) const {
        static_assert(std::is_trivially_copyable_v<T>, "ReadValue requires a trivially copyable type");
        const auto bytes = Read(address, sizeof(T));
        T value{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }
    DWORD Pid() const noexcept { return pid_; }
    unsigned PointerSize() const noexcept { return pointerSize_; }
    const std::wstring& Name() const noexcept { return name_; }
    bool IsRunning() const;
private:
    Handle process_;
    std::unique_ptr<DriverDevice> device_;
    Backend backend_;
    DWORD pid_;
    unsigned pointerSize_ = 0;
    std::wstring name_;
};
}

