#pragma once
#include "memory.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>

namespace offset_reader {
using Json = nlohmann::json;
struct Offset { bool negative = false; std::uint64_t magnitude = 0; };
struct ValueSpec {
    std::string name, type;
    std::vector<Offset> offsets;
    std::optional<std::wstring> module;
    std::optional<std::uint64_t> address;
    std::size_t length = 0;
    std::size_t ByteSize(unsigned pointerSize) const;
};
struct Profile {
    std::optional<std::wstring> processName, module;
    std::optional<DWORD> pid;
    unsigned pointerSize = 0; // zero means auto
    std::vector<ValueSpec> values;
};
using ReadFunction = std::function<std::vector<std::uint8_t>(std::uint64_t, std::size_t)>;
using ModuleBases = std::map<std::wstring, std::uint64_t>;
Offset ParseInteger(const Json& value, const std::string& label, bool allowNegative = false);
Profile ParseProfile(const Json& data);
Profile LoadProfile(const std::filesystem::path& path);
ModuleBases GetModuleBases(DWORD pid);
std::uint64_t ResolveAddress(const ValueSpec& spec, std::uint64_t base, unsigned pointerSize,
                             const ReadFunction& read);
Json DecodeValue(const ValueSpec& spec, const std::vector<std::uint8_t>& data, unsigned pointerSize);
Json ReadValues(const Profile& profile, unsigned pointerSize, const ModuleBases& modules,
                const ReadFunction& read);
Json ReadProfile(const Profile& profile, const ProcessMemory& memory);
bool HasErrors(const Json& values);
}

