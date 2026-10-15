#include "offset_reader/profile.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace offset_reader;
namespace {
struct Leaf {
    std::uint32_t counter = 123456789;
    float ratio = 3.25f;
    char label[64] = "OffsetReader demo";
    wchar_t wide[32] = L"Demo \u03A9";
    std::uint8_t enabled = 1;
};
struct Node { std::uint64_t marker; volatile Leaf* leaf; };
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        std::filesystem::path path = L"profiles/demo.generated.json";
        if (argc != 1) {
            if (argc != 3 || std::wstring(argv[1]) != L"--profile") throw std::runtime_error("Usage: OffsetReaderDemo [--profile PATH]");
            path = argv[2];
        }
        volatile Leaf leaf;
        volatile Node node{0xABCDEF, &leaf};
        volatile Node* volatile root = &node;
        const auto rootAddress = Hex(reinterpret_cast<std::uint64_t>(&root));
        const auto value = [&](const char* name, const char* type, std::size_t field, std::size_t length = 0) {
            Json spec{{"name", name}, {"type", type}, {"address", rootAddress},
                {"offsets", Json::array({0, offsetof(Node, leaf), field})}};
            if (length) spec["length"] = length;
            return spec;
        };
        wchar_t image[32768]{};
        if (!GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)))) throw WindowsError("Get demo image path", GetLastError());
        Json profile{{"schema_version", 1}, {"process", {{"pid", GetCurrentProcessId()}}},
            {"module", ToUtf8(std::filesystem::path(image).filename().wstring())}, {"pointer_size", "auto"},
            {"values", Json::array({value("counter", "u32", offsetof(Leaf, counter)),
                value("ratio", "f32", offsetof(Leaf, ratio)), value("label", "utf8", offsetof(Leaf, label), 64),
                value("wide", "utf16", offsetof(Leaf, wide), 64), value("enabled", "bool", offsetof(Leaf, enabled)),
                Json{{"name", "module_signature"}, {"type", "u16"}, {"offsets", Json::array({0})}}})}};
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        auto temporary = path; temporary += L".tmp";
        { std::ofstream stream(temporary, std::ios::binary); stream << profile.dump(2) << '\n'; if (!stream) throw std::runtime_error("Cannot write demo profile"); }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw WindowsError("Publish demo profile", GetLastError());
        std::cout << "PID " << GetCurrentProcessId() << " | profile " << ToUtf8(std::filesystem::absolute(path).wstring()) << '\n'
            << "Known values: counter=123456789, ratio=3.25, label=OffsetReader demo, wide=Demo Omega, enabled=true, module_signature=23117\n"
            << "Leave this process running while reading. Ctrl+C exits.\n" << std::flush;
        for (;;) {
            // Keep the complete pointer graph alive and observable to another process.
            MemoryBarrier();
            Sleep(1000);
        }
    } catch (const std::exception& error) { std::cerr << "Error: " << error.what() << '\n'; return 1; }
}
