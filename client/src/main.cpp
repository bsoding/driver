#include "offset_reader/profile.hpp"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <set>

using namespace offset_reader;
namespace {
std::atomic<bool> stopped{false};
BOOL WINAPI OnControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    stopped.store(true);
    return TRUE;
}
void Help() {
    std::cout << "OffsetReader - native C++ Windows memory reader\n\n"
        "  OffsetReader validate PROFILE\n"
        "  OffsetReader processes [--name TEXT]\n"
        "  OffsetReader modules PID\n"
        "  OffsetReader status\n"
        "  OffsetReader read-memory PID ADDRESS_OR_OFFSET [--module NAME] [--size BYTES]\n"
        "             [--type TYPE] [--backend driver|user] [--json]\n"
        "  OffsetReader read PROFILE [--pid PID] [--backend driver|user] [--watch SECONDS] [--json]\n\n"
        "The default backend is driver. --backend user explicitly uses ReadProcessMemory.\n"
        "read-memory uses an absolute address, or module base + offset with --module.\n"
        "Its default is 4 raw bytes; --type u32 (etc.) decodes a value instead.\n"
        "Polling intervals are 0.05 to 86400 seconds. Ctrl+C stops polling.\n";
}
DWORD Pid(const std::wstring& text) {
    const auto value = ParseInteger(ToUtf8(text), "PID").magnitude;
    if (!value || value > MAXDWORD) throw std::runtime_error("PID must be between 1 and 4294967295");
    return static_cast<DWORD>(value);
}
Backend ParseBackend(const std::wstring& text) {
    if (text != L"driver" && text != L"user") throw std::runtime_error("--backend must be driver or user");
    return text == L"driver" ? Backend::Driver : Backend::User;
}
int ReadMemoryCommand(const std::vector<std::wstring>& arguments) {
    if (arguments.size() < 3) throw std::runtime_error("Usage: OffsetReader read-memory PID ADDRESS_OR_OFFSET [options]");
    const auto pid = Pid(arguments[1]);
    Backend backend = Backend::Driver;
    bool asJson = false;
    std::optional<std::string> module;
    std::optional<std::uint64_t> size;
    std::string type = "bytes";
    std::set<std::wstring> seen;
    for (std::size_t i = 3; i < arguments.size(); ++i) {
        const auto& option = arguments[i];
        if (!seen.insert(option).second) throw std::runtime_error("Repeated option: " + ToUtf8(option));
        if (option == L"--json") { asJson = true; continue; }
        if (option != L"--module" && option != L"--size" && option != L"--type" && option != L"--backend")
            throw std::runtime_error("Unknown option: " + ToUtf8(option));
        if (++i == arguments.size()) throw std::runtime_error("Missing value for " + ToUtf8(option));
        if (option == L"--module") module = ToUtf8(arguments[i]);
        if (option == L"--size") size = ParseInteger(ToUtf8(arguments[i]), "--size").magnitude;
        if (option == L"--type") type = ToUtf8(arguments[i]);
        if (option == L"--backend") backend = ParseBackend(arguments[i]);
    }
    Json value{{"name", "memory"}, {"type", type}};
    if (module) {
        value["module"] = *module;
        value["offsets"] = Json::array({ToUtf8(arguments[2])});
    } else value["address"] = ToUtf8(arguments[2]);
    if (type == "bytes" || type == "utf8" || type == "utf16") value["length"] = size.value_or(4);
    else if (size) throw std::runtime_error("--size applies to bytes, utf8, and utf16; scalar types have a fixed size");
    const auto profile = ParseProfile({{"schema_version", 1}, {"process", {{"pid", pid}}},
        {"values", Json::array({value})}});
    const ProcessMemory memory(pid, backend);
    auto sample = ReadProfile(profile, memory).front();
    if (sample.contains("error")) throw std::runtime_error(sample["error"].get<std::string>());
    sample.erase("name");
    sample["pid"] = pid;
    sample["backend"] = backend == Backend::Driver ? "driver" : "user";
    sample["size"] = profile.values.front().ByteSize(memory.PointerSize());
    if (asJson) std::cout << sample.dump(-1, ' ', true) << '\n';
    else std::cout << "[" << sample["backend"].get<std::string>() << "] PID " << pid
        << " @ " << sample["address"].get<std::string>() << " (" << sample["size"] << " bytes, " << type << ") = "
        << (sample["value"].is_string() ? sample["value"].get<std::string>() : sample["value"].dump()) << '\n';
    return 0;
}
std::string Timestamp() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth
        << '-' << std::setw(2) << now.wDay << 'T' << std::setw(2) << now.wHour << ':'
        << std::setw(2) << now.wMinute << ':' << std::setw(2) << now.wSecond << '.'
        << std::setw(3) << now.wMilliseconds << "+00:00";
    return stream.str();
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        std::vector<std::wstring> arguments(argv + 1, argv + argc);
        if (arguments.empty() || arguments[0] == L"--help" || arguments[0] == L"-h") { Help(); return 0; }
        if (std::find(arguments.begin(), arguments.end(), L"--help") != arguments.end() ||
            std::find(arguments.begin(), arguments.end(), L"-h") != arguments.end()) { Help(); return 0; }
        const auto& command = arguments[0];
        if (command == L"validate") {
            if (arguments.size() != 2) throw std::runtime_error("Usage: OffsetReader validate PROFILE");
            const auto profile = LoadProfile(arguments[1]);
            std::cout << "Valid profile: " << profile.values.size() << " values, pointer_size="
                << (profile.pointerSize ? std::to_string(profile.pointerSize) : "auto") << '\n';
            return 0;
        }
        if (command == L"processes") {
            std::wstring filter;
            if (arguments.size() != 1) {
                if (arguments.size() != 3 || arguments[1] != L"--name") throw std::runtime_error("Usage: OffsetReader processes [--name TEXT]");
                filter = LowerName(arguments[2]);
            }
            for (const auto& process : Processes())
                if (filter.empty() || LowerName(process.name).find(filter) != std::wstring::npos)
                    std::cout << std::setw(7) << process.pid << "  " << ToUtf8(process.name) << '\n';
            return 0;
        }
        if (command == L"modules") {
            if (arguments.size() != 2) throw std::runtime_error("Usage: OffsetReader modules PID");
            for (const auto& module : Modules(Pid(arguments[1])))
                std::cout << Hex(module.base, 16) << "  " << std::setw(10) << module.size << "  " << ToUtf8(module.name) << '\n';
            return 0;
        }
        if (command == L"status") {
            if (arguments.size() != 1) throw std::runtime_error("Usage: OffsetReader status");
            const DriverDevice driver;
            std::cout << "Driver connected: protocol " << driver.Info().version << ", max read " << driver.Info().maxRead << " bytes\n";
            return 0;
        }
        if (command == L"read-memory") return ReadMemoryCommand(arguments);
        if (command != L"read" || arguments.size() < 2) throw std::runtime_error("Use OffsetReader --help for commands");
        std::optional<DWORD> pidOverride;
        Backend backend = Backend::Driver;
        std::optional<double> watch;
        bool asJson = false;
        std::set<std::wstring> seen;
        for (std::size_t i = 2; i < arguments.size(); ++i) {
            const auto& option = arguments[i];
            if (!seen.insert(option).second) throw std::runtime_error("Repeated option: " + ToUtf8(option));
            if (option == L"--json") { asJson = true; continue; }
            if (option != L"--pid" && option != L"--backend" && option != L"--watch") throw std::runtime_error("Unknown option: " + ToUtf8(option));
            if (++i == arguments.size()) throw std::runtime_error("Missing value for " + ToUtf8(option));
            if (option == L"--pid") pidOverride = Pid(arguments[i]);
            if (option == L"--backend") backend = ParseBackend(arguments[i]);
            if (option == L"--watch") {
                std::size_t used = 0;
                const auto value = std::stod(arguments[i], &used);
                if (used != arguments[i].size() || !std::isfinite(value) || value < 0.05 || value > 86400)
                    throw std::runtime_error("--watch must be between 0.05 and 86400 seconds");
                watch = value;
            }
        }
        const auto profile = LoadProfile(arguments[1]);
        DWORD pid = 0;
        if (pidOverride) pid = *pidOverride;
        else if (profile.pid) pid = *profile.pid;
        else pid = SelectPid(*profile.processName);
        const ProcessMemory memory(pid, backend, pidOverride ? L"" : profile.processName.value_or(L""));
        SetConsoleCtrlHandler(OnControl, TRUE);
        const auto backendName = backend == Backend::Driver ? "driver" : "user";
        while (!stopped.load()) {
            const auto start = std::chrono::steady_clock::now();
            if (!memory.IsRunning()) throw std::runtime_error("Target process exited");
            const auto values = ReadProfile(profile, memory);
            const Json sample{{"timestamp", Timestamp()}, {"backend", backendName}, {"pid", pid},
                {"process", ToUtf8(memory.Name())}, {"pointer_size", memory.PointerSize()}, {"values", values}};
            if (asJson) std::cout << sample.dump(-1, ' ', true) << '\n';
            else {
                std::cout << '[' << backendName << "] " << ToUtf8(memory.Name()) << " PID " << pid << " | " << sample["timestamp"].get<std::string>() << '\n';
                for (const auto& value : values) {
                    std::cout << "  " << value["name"].get<std::string>();
                    if (value.contains("error")) std::cout << ": ERROR " << value["error"].get<std::string>();
                    else {
                        std::cout << " = " << (value["value"].is_string() ? value["value"].get<std::string>() : value["value"].dump())
                            << "  (" << value["type"].get<std::string>() << " @ " << value["address"].get<std::string>() << ')';
                    }
                    std::cout << '\n';
                }
            }
            std::cout.flush();
            if (!watch) { SetConsoleCtrlHandler(OnControl, FALSE); return HasErrors(values) ? 2 : 0; }
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            auto milliseconds = static_cast<DWORD>(std::max(0.0, *watch - elapsed) * 1000);
            while (milliseconds && !stopped.load()) {
                const auto slice = std::min<DWORD>(milliseconds, 50);
                Sleep(slice); milliseconds -= slice;
            }
        }
        SetConsoleCtrlHandler(OnControl, FALSE);
        return 0;
    } catch (const std::exception& error) {
        SetConsoleCtrlHandler(OnControl, FALSE);
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

