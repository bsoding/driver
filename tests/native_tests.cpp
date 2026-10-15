#include "offset_reader/profile.hpp"
#include <winioctl.h>
#include "../driver/protocol.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace offset_reader;
namespace fs = std::filesystem;
namespace {
void Expect(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class F> void Throws(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected an error");
}
template<class T> std::vector<std::uint8_t> Bytes(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T)); std::memcpy(bytes.data(), &value, sizeof(T)); return bytes;
}
Json BaseProfile() {
    return {{"schema_version", 1}, {"process", {{"name", "Test.exe"}}}, {"module", "Test.exe"},
        {"values", Json::array({Json{{"name", "counter"}, {"type", "u32"}, {"offsets", Json::array({"0x100", "0x10", "0x4"})}}})}};
}
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') result.append(backslashes * 2 + 1, L'\\');
        else result.append(backslashes, L'\\');
        backslashes = 0; result += c;
    }
    result.append(backslashes * 2, L'\\'); result += L'"'; return result;
}
class Child {
public:
    explicit Child(Handle process) : process_(std::move(process)) {}
    Child(Child&&) = default;
    ~Child() { Stop(); }
    HANDLE Get() const { return process_.Get(); }
    void Stop() {
        if (process_ && WaitForSingleObject(process_.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_.Get(), 0); WaitForSingleObject(process_.Get(), 5000);
        }
    }
private: Handle process_;
};
Child Start(const fs::path& exe, const std::vector<std::wstring>& arguments, HANDLE output = nullptr) {
    std::wstring command = Quote(exe.wstring());
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    Handle nullOutput;
    if (!output) {
        nullOutput = Handle(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        output = nullOutput.Get();
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput.Get(); startup.hStdOutput = output; startup.hStdError = output;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        throw WindowsError("Start test child", GetLastError());
    Handle thread(info.hThread);
    return Child(Handle(info.hProcess));
}
struct CommandResult { DWORD code; std::string output; };
CommandResult Run(const fs::path& exe, const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) throw WindowsError("Create test pipe", GetLastError());
    Handle reader(rawRead), writer(rawWrite);
    if (!SetHandleInformation(reader.Get(), HANDLE_FLAG_INHERIT, 0)) throw WindowsError("Set test pipe inheritance", GetLastError());
    auto child = Start(exe, arguments, writer.Get());
    writer = Handle{};
    std::string output;
    std::thread drain([&] {
        char buffer[4096]; DWORD count = 0;
        while (ReadFile(reader.Get(), buffer, sizeof(buffer), &count, nullptr) && count)
            if (output.size() < 8 * 1024 * 1024) output.append(buffer, count);
    });
    const auto wait = WaitForSingleObject(child.Get(), 10000);
    if (wait != WAIT_OBJECT_0) child.Stop();
    drain.join();
    Expect(wait == WAIT_OBJECT_0, "Test command timed out");
    DWORD code = 0;
    if (!GetExitCodeProcess(child.Get(), &code)) throw WindowsError("Get test exit code", GetLastError());
    return {code, output};
}
struct Fixture {
    fs::path root, directory, profilePath, client;
    std::unique_ptr<Child> target;
    Profile profile;
    Fixture() {
        wchar_t image[32768]{};
        GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
        root = fs::path(image).parent_path().parent_path();
        fs::create_directories(root / L"build");
        const auto build = fs::canonical(root / L"build");
        directory = build / (L"native test - \u03A9 - " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Expect(fs::create_directory(directory), "Scratch directory already exists");
        profilePath = directory / L"demo profile \u03A9.json";
        client = root / L"dist/OffsetReader.exe";
        try {
            target = std::make_unique<Child>(Start(root / L"dist/OffsetReaderDemo.exe", {L"--profile", profilePath.wstring()}));
            const auto deadline = GetTickCount64() + 10000;
            while (!fs::exists(profilePath)) {
                Expect(WaitForSingleObject(target->Get(), 0) == WAIT_TIMEOUT && GetTickCount64() < deadline,
                    "Demo target failed to publish a profile");
                Sleep(10);
            }
            profile = LoadProfile(profilePath);
        } catch (...) { Clean(); throw; }
    }
    ~Fixture() { Clean(); }
    void Clean() {
        target.reset();
        std::error_code error;
        // Only remove the unique directory this fixture created inside build.
        if (!directory.empty() && directory.parent_path() == fs::weakly_canonical(root / L"build", error))
            fs::remove_all(directory, error);
    }
    fs::path Write(const wchar_t* name, const Json& data) const {
        const auto path = directory / name;
        std::ofstream stream(path, std::ios::binary); stream << data.dump(2);
        Expect(static_cast<bool>(stream), "Write test profile failed"); return path;
    }
};
Json KnownValues(const Json& rows) {
    Expect(!HasErrors(rows), rows.dump());
    Json values = Json::object(); for (const auto& row : rows) values[row["name"].get<std::string>()] = row["value"];
    return values;
}
const Json expectedValues{{"counter", 123456789}, {"ratio", 3.25}, {"label", "OffsetReader demo"},
    {"wide", "Demo \u03A9"}, {"enabled", true}, {"module_signature", 23117}};
struct Runner {
    unsigned passed = 0, failed = 0;
    template<class F> void Test(const char* name, F&& action) {
        try { action(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
    }
};
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool driverVm = argc == 2 && std::wstring(argv[1]) == L"--driver-vm";
    if (argc != 1 && !driverVm) { std::cerr << "Usage: OffsetReaderTests [--driver-vm]\n"; return 1; }
    try {
        if (driverVm) { const DriverDevice preflight; (void)preflight; }
        Runner runner;
        runner.Test("two-hop chain and case-insensitive modules", [] {
            const auto profile = ParseProfile(BaseProfile());
            const std::map<std::uint64_t, std::vector<std::uint8_t>> data{
                {0x1100, Bytes<std::uint64_t>(0x2000)}, {0x2010, Bytes<std::uint64_t>(0x3000)}, {0x3004, Bytes<std::uint32_t>(123456789)}};
            std::vector<std::pair<std::uint64_t, std::size_t>> visits;
            const auto rows = ReadValues(profile, 8, {{L"TEST.EXE", 0x1000}}, [&](auto address, auto size) {
                visits.emplace_back(address, size); return data.at(address);
            });
            Expect(rows[0]["value"] == 123456789, rows.dump());
            Expect(visits == std::vector<std::pair<std::uint64_t, std::size_t>>{{0x1100, 8}, {0x2010, 8}, {0x3004, 4}}, "Wrong pointer-chain dereference order");
        });
        runner.Test("x86 pointers and negative offsets", [] {
            auto data = BaseProfile(); data["values"][0]["offsets"] = Json::array({0, "-0x4"});
            const auto spec = ParseProfile(data).values[0];
            Expect(ResolveAddress(spec, 0x1000, 4, [](auto, auto) { return Bytes<std::uint32_t>(0x3000); }) == 0x2FFC, "Wrong signed offset result");
        });
        runner.Test("single offset does not dereference", [] {
            auto data = BaseProfile(); data["values"][0]["offsets"] = Json::array({"0x20"});
            Expect(ResolveAddress(ParseProfile(data).values[0], 0x1000, 8, [](auto, auto) -> std::vector<std::uint8_t> {
                throw std::runtime_error("Unexpected dereference"); }) == 0x1020, "Wrong direct address");
        });
        runner.Test("null, kernel, and short pointer reads rejected", [] {
            const auto spec = ParseProfile(BaseProfile()).values[0];
            Throws([&] { ResolveAddress(spec, 0x1000, 8, [](auto, auto) { return Bytes<std::uint64_t>(0); }); });
            Throws([&] { ResolveAddress(spec, 0x1000, 8, [](auto, auto) { return Bytes<std::uint64_t>(UINT64_MAX); }); });
            Throws([&] { ResolveAddress(spec, 0x1000, 8, [](auto, auto) { return std::vector<std::uint8_t>{0}; }); });
        });
        runner.Test("address boundaries and arithmetic wrap rejected", [] {
            Throws([] { CheckAddress(MaxUserAddress, 8, 8); });
            Throws([] { CheckAddress(0xFFFFFFFF, 8, 4); });
            auto data = BaseProfile(); data["values"][0]["offsets"] = Json::array({"0xffffffffffffffff"});
            const auto spec = ParseProfile(data).values[0];
            Throws([&] { ResolveAddress(spec, 0x1000, 8, [](auto, auto) { return std::vector<std::uint8_t>{}; }); });
        });
        runner.Test("malformed and ambiguous profiles rejected", [] {
            const std::vector<std::function<void(Json&)>> edits{
                [](auto& d) { d["schema_version"] = true; }, [](auto& d) { d["pointer_size"] = true; },
                [](auto& d) { d["process"]["pid"] = 1; }, [](auto& d) { d["typo"] = 1; },
                [](auto& d) { d["values"][0]["offsets"] = Json::array({true}); },
                [](auto& d) { d["values"][0]["type"] = "utf16"; d["values"][0]["length"] = 3; },
                [](auto& d) { d["values"][0]["type"] = "utf8"; d["values"][0]["length"] = 65537; },
                [](auto& d) { d["values"][0]["address"] = "0x1000"; d["values"][0]["module"] = "Other.dll"; },
                [](auto& d) { const auto duplicate = d["values"][0]; d["values"].push_back(duplicate); }
            };
            for (const auto& edit : edits) { auto data = BaseProfile(); edit(data); Throws([&] { ParseProfile(data); }); }
        });
        runner.Test("64-bit integer precision and overflow", [] {
            Expect(ParseInteger("0xffffffffffffffff", "integer").magnitude == UINT64_MAX, "Lost unsigned precision");
            Expect(ParseInteger("-0xffffffffffffffff", "offset", true).negative, "Lost negative offset");
            Throws([] { ParseInteger("18446744073709551616", "integer"); });
            ValueSpec spec; spec.type = "u64";
            Expect(DecodeValue(spec, Bytes(UINT64_MAX), 8).get<std::uint64_t>() == UINT64_MAX, "Lost decoded uint64 precision");
        });
        runner.Test("aligned UTF-16 and invalid UTF-8 replacement", [] {
            ValueSpec spec; spec.type = "utf16"; spec.length = 8;
            Expect(DecodeValue(spec, {0x41, 0, 0, 1, 0, 0, 0xff, 0xff}, 8) == "A\u0100", "Misaligned terminator");
            spec.type = "utf8"; spec.length = 3;
            Expect(DecodeValue(spec, {0xff, 'A', 0}, 8) == "\uFFFDA", "UTF-8 replacement failed");
        });
        runner.Test("nonfinite floats produce portable JSON", [] {
            ValueSpec spec; spec.type = "f64";
            Expect(DecodeValue(spec, Bytes(std::numeric_limits<double>::infinity()), 8) == "inf", "Infinity encoding");
            Expect(DecodeValue(spec, Bytes(std::numeric_limits<double>::quiet_NaN()), 8) == "nan", "NaN encoding");
        });
        runner.Test("read failures isolated per value", [] {
            auto data = BaseProfile(); data["values"] = Json::array({Json{{"name", "bad"}, {"type", "u32"}, {"address", "0x1000"}},
                Json{{"name", "good"}, {"type", "u32"}, {"address", "0x2000"}}});
            const auto rows = ReadValues(ParseProfile(data), 8, {}, [](auto address, auto) {
                if (address == 0x1000) throw std::runtime_error("unmapped"); return Bytes<std::uint32_t>(42);
            });
            Expect(rows[0].contains("error") && rows[1]["value"] == 42, "A failed value hid a successful value");
        });
        runner.Test("SDK typed read against owned process", [] {
            const std::uint64_t known = 0xFEDCBA9876543210;
            const ProcessMemory memory(GetCurrentProcessId(), Backend::User);
            Expect(memory.ReadValue<std::uint64_t>(reinterpret_cast<std::uint64_t>(&known)) == known, "Typed read failed");
            Throws([&] { memory.Read(1, 4); });
        });
        Fixture fixture;
        runner.Test("real child values, Unicode, modules and pointer graph", [&] {
            const ProcessMemory memory(*fixture.profile.pid, Backend::User);
            Expect(KnownValues(ReadProfile(fixture.profile, memory)) == expectedValues, "Demo values differ");
        });
        runner.Test("native CLI JSON with spaces and Unicode paths", [&] {
            const auto result = Run(fixture.client, {L"read", fixture.profilePath.wstring(), L"--backend", L"user", L"--json"});
            Expect(result.code == 0, result.output);
            const auto sample = Json::parse(result.output);
            Expect(sample["backend"] == "user" && sample["pid"] == *fixture.profile.pid && KnownValues(sample["values"]) == expectedValues, result.output);
        });
        const ProcessMemory directMemory(*fixture.profile.pid, Backend::User);
        const auto counterAddress = ResolveAddress(fixture.profile.values.front(), *fixture.profile.values.front().address,
            directMemory.PointerSize(), [&](auto address, auto size) { return directMemory.Read(address, size); });
        const auto pidText = std::to_wstring(*fixture.profile.pid);
        const auto addressText = ToWide(Hex(counterAddress));
        runner.Test("direct PID/address CLI returns typed and raw memory", [&] {
            auto result = Run(fixture.client, {L"read-memory", pidText, addressText, L"--type", L"u32", L"--backend", L"user", L"--json"});
            Expect(result.code == 0, result.output);
            const auto sample = Json::parse(result.output);
            Expect(sample["value"] == 123456789 && sample["size"] == 4 && sample["pid"] == *fixture.profile.pid &&
                sample["backend"] == "user" && sample["address"] == Hex(counterAddress), result.output);
            result = Run(fixture.client, {L"read-memory", pidText, addressText, L"--backend", L"user"});
            Expect(result.code == 0 && result.output.find("15 cd 5b 07") != std::string::npos, result.output);
        });
        runner.Test("direct module offsets resolve ASLR and signed offsets", [&] {
            auto result = Run(fixture.client, {L"read-memory", pidText, L"0x0", L"--module", L"offsetreaderdemo.EXE",
                L"--type", L"u16", L"--backend", L"user", L"--json"});
            Expect(result.code == 0 && Json::parse(result.output)["value"] == 23117, result.output);
            const auto base = GetModuleBases(*fixture.profile.pid).at(L"offsetreaderdemo.exe");
            const auto offset = counterAddress < base ? "-" + Hex(base - counterAddress) : Hex(counterAddress - base);
            result = Run(fixture.client, {L"read-memory", pidText, ToWide(offset), L"--module", L"OffsetReaderDemo.exe",
                L"--size", L"0x4", L"--backend", L"user", L"--json"});
            Expect(result.code == 0 && Json::parse(result.output)["value"] == "15 cd 5b 07", result.output);
        });
        runner.Test("direct string read and byte length", [&] {
            const auto& spec = fixture.profile.values.at(2);
            const auto address = ResolveAddress(spec, *spec.address, directMemory.PointerSize(),
                [&](auto source, auto size) { return directMemory.Read(source, size); });
            const auto result = Run(fixture.client, {L"read-memory", pidText, ToWide(Hex(address)), L"--type", L"utf8",
                L"--size", L"64", L"--backend", L"user", L"--json"});
            Expect(result.code == 0 && Json::parse(result.output)["value"] == "OffsetReader demo", result.output);
        });
        runner.Test("direct read rejects invalid inputs and inaccessible memory", [&] {
            const std::vector<std::vector<std::wstring>> invalid{
                {L"read-memory"}, {L"read-memory", L"-1", addressText},
                {L"read-memory", pidText, L"0"}, {L"read-memory", pidText, L"0xFFFF800000000000"},
                {L"read-memory", pidText, L"0x10000000000000000"},
                {L"read-memory", pidText, addressText, L"--size", L"0"},
                {L"read-memory", pidText, addressText, L"--size", L"65537"},
                {L"read-memory", pidText, addressText, L"--type", L"u32", L"--size", L"4"},
                {L"read-memory", pidText, addressText, L"--type", L"unsupported"},
                {L"read-memory", pidText, addressText, L"--backend", L"invalid"},
                {L"read-memory", pidText, addressText, L"--size"},
                {L"read-memory", pidText, addressText, L"--json", L"--json"},
                {L"read-memory", pidText, L"1", L"--backend", L"user"},
                {L"read-memory", pidText, L"0", L"--module", L"not-loaded.dll", L"--backend", L"user"},
                {L"read-memory", pidText, L"0xFFFFFFFFFFFFFFFF", L"--module", L"OffsetReaderDemo.exe", L"--backend", L"user"}
            };
            for (const auto& arguments : invalid) {
                const auto result = Run(fixture.client, arguments);
                Expect(result.code == 1 && result.output.find("Error:") != std::string::npos, result.output);
            }
        });
        runner.Test("profile PID override and process-name selection", [&] {
            auto data = Json::parse(std::ifstream(fixture.profilePath));
            data["process"] = {{"name", "OffsetReaderDemo.exe"}};
            const auto path = fixture.Write(L"named.json", data);
            auto result = Run(fixture.client, {L"read", path.wstring(), L"--backend", L"user", L"--json"});
            Expect(result.code == 0, result.output);
            data["process"] = {{"name", "does-not-exist.exe"}};
            const auto overridePath = fixture.Write(L"override.json", data);
            result = Run(fixture.client, {L"read", overridePath.wstring(), L"--pid", std::to_wstring(*fixture.profile.pid), L"--backend", L"user", L"--json"});
            Expect(result.code == 0, result.output);
        });
        runner.Test("native CLI validate, processes, and modules", [&] {
            Expect(Run(fixture.client, {L"validate", fixture.profilePath.wstring()}).code == 0, "validate failed");
            const auto list = Run(fixture.client, {L"processes", L"--name", L"OffsetReaderDemo"});
            Expect(list.code == 0 && list.output.find(std::to_string(*fixture.profile.pid)) != std::string::npos, list.output);
            const auto modules = Run(fixture.client, {L"modules", std::to_wstring(*fixture.profile.pid)});
            Expect(modules.code == 0 && modules.output.find("OffsetReaderDemo.exe") != std::string::npos, modules.output);
        });
        runner.Test("native CLI partial errors and invalid options", [&] {
            auto data = Json::parse(std::ifstream(fixture.profilePath));
            data["values"].push_back({{"name", "unmapped"}, {"type", "u32"}, {"address", 1}});
            const auto path = fixture.Write(L"partial.json", data);
            const auto result = Run(fixture.client, {L"read", path.wstring(), L"--backend", L"user", L"--json"});
            Expect(result.code == 2 && Json::parse(result.output)["values"].back().contains("error"), result.output);
            for (const auto& interval : {L"nan", L"0", L"1oops"})
                Expect(Run(fixture.client, {L"read", fixture.profilePath.wstring(), L"--watch", interval}).code == 1, "Invalid watch accepted");
        });
        runner.Test("pointer-size mismatch rejected", [&] {
            auto wrong = fixture.profile; wrong.pointerSize = 4;
            const ProcessMemory memory(*fixture.profile.pid, Backend::User);
            Throws([&] { ReadProfile(wrong, memory); });
        });
        runner.Test("watch emits multiple independent JSON samples", [&] {
            const auto log = fixture.directory / L"watch.jsonl";
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            Handle output(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security, CREATE_ALWAYS, 0, nullptr));
            Expect(static_cast<bool>(output), "Cannot create watch output file");
            auto child = Start(fixture.client, {L"read", fixture.profilePath.wstring(), L"--backend", L"user", L"--json", L"--watch", L"0.05"}, output.Get());
            output = Handle{};
            const auto deadline = GetTickCount64() + 5000;
            unsigned count = 0;
            while (GetTickCount64() < deadline) {
                count = 0;
                std::ifstream stream(log); std::string line;
                while (std::getline(stream, line)) {
                    const auto sample = Json::parse(line, nullptr, false);
                    if (!sample.is_discarded()) {
                        Expect(sample["backend"] == "user" && KnownValues(sample["values"]) == expectedValues, "Invalid polling sample");
                        ++count;
                    }
                }
                if (count >= 2) break;
                Sleep(20);
            }
            child.Stop();
            Expect(count >= 2, "Watch did not emit two complete samples");
        });
        bool loaded = true;
        try { const DriverDevice device; (void)device; } catch (const WindowsError& error) {
            if (error.Code() == ERROR_FILE_NOT_FOUND || error.Code() == ERROR_PATH_NOT_FOUND) loaded = false;
            else if (!driverVm) std::cout << "NOTE driver connection unavailable: " << error.what() << '\n';
        }
        if (!loaded) runner.Test("missing driver does not silently use user-mode reads", [&] {
            const auto result = Run(fixture.client, {L"read", fixture.profilePath.wstring(), L"--json"});
            Expect(result.code == 1 && result.output.find("Open OffsetReader") != std::string::npos, result.output);
            const auto direct = Run(fixture.client, {L"read-memory", pidText, addressText, L"--type", L"u32", L"--json"});
            Expect(direct.code == 1 && direct.output.find("Open OffsetReader") != std::string::npos, direct.output);
        });
        else std::cout << "SKIP missing-driver assertion (device is present or availability is inconclusive)\n";
        if (driverVm) {
            runner.Test("driver reads match independently known values", [&] {
                const ProcessMemory memory(*fixture.profile.pid);
                Expect(KnownValues(ReadProfile(fixture.profile, memory)) == expectedValues, "Driver values differ");
                Throws([&] { memory.Read(1, 4); });
                const auto result = Run(fixture.client, {L"read-memory", pidText, addressText, L"--type", L"u32", L"--json"});
                Expect(result.code == 0 && Json::parse(result.output)["backend"] == "driver" &&
                    Json::parse(result.output)["value"] == 123456789, result.output);
            });
            runner.Test("driver denies query-only target handle", [&] {
                Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *fixture.profile.pid));
                Expect(static_cast<bool>(query), "Open query-only handle failed");
                const OR_READ_REQUEST request{OR_PROTOCOL_VERSION, 0, reinterpret_cast<ULONGLONG>(query.Get()), 0x10000, 4, 0};
                const DriverDevice device;
                try { device.Request(IOCTL_OR_READ, &request, sizeof(request), 4); }
                catch (const WindowsError& error) { Expect(error.Code() == ERROR_ACCESS_DENIED, error.what()); return; }
                throw std::runtime_error("Query-only handle was accepted");
            });
            runner.Test("driver rejects malformed IOCTLs", [&] {
                Handle process(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *fixture.profile.pid));
                Expect(static_cast<bool>(process), "Open test process failed");
                const OR_READ_REQUEST good{OR_PROTOCOL_VERSION, 0, reinterpret_cast<ULONGLONG>(process.Get()), 0x10000, 4, 0};
                const DriverDevice device;
                std::vector<OR_READ_REQUEST> invalid;
                auto bad = good; bad.Version = 99; invalid.push_back(bad);
                bad = good; bad.Reserved = 1; invalid.push_back(bad);
                bad = good; bad.Reserved2 = 1; invalid.push_back(bad);
                bad = good; bad.Size = 0; invalid.push_back(bad);
                bad = good; bad.Size = OR_MAX_READ_SIZE + 1; invalid.push_back(bad);
                bad = good; bad.ProcessHandle = 0; invalid.push_back(bad);
                bad = good; bad.ProcessHandle = UINT64_MAX; invalid.push_back(bad);
                bad = good; bad.Address = 0; invalid.push_back(bad);
                bad = good; bad.Address = 0xFFFF800000000000; invalid.push_back(bad);
                bad = good; bad.Address = UINT64_MAX - 1; invalid.push_back(bad);
                for (const auto& request : invalid) Throws([&] { device.Request(IOCTL_OR_READ, &request, sizeof(request), 4); });
                Throws([&] { device.Request(IOCTL_OR_READ, &good, 31, 4); });
                Throws([&] { device.Request(IOCTL_OR_READ, nullptr, 0, 4); });
                Throws([&] { device.Request(IOCTL_OR_READ, &good, sizeof(good), 1); });
                Throws([&] { device.Request(IOCTL_OR_INFO + 8, nullptr, 0, 16); });
                Throws([&] { device.Request(IOCTL_OR_INFO, nullptr, 0, 1); });
            });
        }
        std::cout << runner.passed << " passed, " << runner.failed << " failed\n";
        return runner.failed ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << "Setup error: " << error.what() << '\n'; return 1; }
}
