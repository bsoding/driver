#include "offset_reader/profile.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace offset_reader {
static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
Offset ParseInteger(const Json& value, const std::string& label, bool allowNegative) {
    Offset result;
    if (value.is_number_unsigned()) result.magnitude = value.get<std::uint64_t>();
    else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        result.negative = number < 0;
        result.magnitude = number < 0 ? static_cast<std::uint64_t>(-(number + 1)) + 1 : static_cast<std::uint64_t>(number);
    } else if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::size_t start = 0;
        if (!text.empty() && (text[0] == '-' || text[0] == '+')) { result.negative = text[0] == '-'; ++start; }
        int base = 10;
        if (text.size() >= start + 2 && text[start] == '0' && (text[start + 1] == 'x' || text[start + 1] == 'X')) {
            base = 16; start += 2;
        }
        Require(start < text.size(), label + ": invalid integer");
        const auto parsed = std::from_chars(text.data() + start, text.data() + text.size(), result.magnitude, base);
        Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), label + ": invalid or overflowing integer");
    } else throw std::runtime_error(label + ": expected an integer or hex string");
    if (!result.magnitude) result.negative = false;
    Require(allowNegative || !result.negative, label + ": must be nonnegative");
    return result;
}
static void Keys(const Json& object, std::initializer_list<const char*> allowed, const std::string& label) {
    Require(object.is_object(), label + ": expected an object");
    const std::set<std::string> names(allowed.begin(), allowed.end());
    for (auto it = object.begin(); it != object.end(); ++it)
        Require(names.count(it.key()) != 0, label + ": unknown field " + it.key());
}
static std::string Name(const Json& value, const std::string& label) {
    Require(value.is_string(), label + ": expected a nonempty string");
    auto name = value.get<std::string>();
    Require(!name.empty() && name.find_first_not_of(" \r\n\t") != std::string::npos && name.find('\0') == std::string::npos,
        label + ": expected a nonempty name without NUL characters");
    return name;
}
static bool VariableType(const std::string& type) { return type == "bytes" || type == "utf8" || type == "utf16"; }
std::size_t ValueSpec::ByteSize(unsigned pointerSize) const {
    if (type == "pointer") return pointerSize;
    if (type == "bool" || type == "u8" || type == "i8") return 1;
    if (type == "u16" || type == "i16") return 2;
    if (type == "u32" || type == "i32" || type == "f32") return 4;
    if (type == "u64" || type == "i64" || type == "f64") return 8;
    if (VariableType(type)) return length;
    throw std::runtime_error("Unsupported value type: " + type);
}
Profile ParseProfile(const Json& data) {
    Keys(data, {"schema_version", "process", "module", "pointer_size", "values"}, "profile");
    Require(data.contains("schema_version") && data["schema_version"].is_number_integer() && data["schema_version"] == 1,
        "schema_version must be 1");
    Require(data.contains("process"), "process is required");
    const auto& process = data["process"];
    Keys(process, {"name", "pid"}, "process");
    Require(process.size() == 1, "process must contain exactly one of name or pid");
    Profile profile;
    if (process.contains("name")) profile.processName = ToWide(Name(process["name"], "process.name"));
    else {
        const auto pid = ParseInteger(process["pid"], "process.pid").magnitude;
        Require(pid > 0 && pid <= MAXDWORD, "process.pid must be between 1 and 4294967295");
        profile.pid = static_cast<DWORD>(pid);
    }
    if (data.contains("module")) profile.module = ToWide(Name(data["module"], "module"));
    if (data.contains("pointer_size") && data["pointer_size"] != "auto") {
        const auto& size = data["pointer_size"];
        Require(size.is_number_integer() && (size == 4 || size == 8), "pointer_size must be 4, 8, or auto");
        profile.pointerSize = size.get<unsigned>();
    }
    Require(data.contains("values") && data["values"].is_array() && !data["values"].empty() && data["values"].size() <= 1024,
        "values must contain 1 to 1024 entries");
    std::set<std::string> used;
    for (const auto& entry : data["values"]) {
        Keys(entry, {"name", "type", "offsets", "module", "address", "length"}, "value");
        Require(entry.contains("name") && entry.contains("type"), "Each value needs name and type");
        ValueSpec spec;
        spec.name = Name(entry["name"], "value.name");
        Require(used.insert(spec.name).second, "Duplicate value name: " + spec.name);
        spec.type = Name(entry["type"], spec.name + ".type");
        if (entry.contains("module")) spec.module = ToWide(Name(entry["module"], spec.name + ".module"));
        if (entry.contains("address")) {
            spec.address = ParseInteger(entry["address"], spec.name + ".address").magnitude;
            CheckAddress(*spec.address, 1, 8);
        }
        Require(!(spec.address && spec.module), spec.name + ": choose address or module, not both");
        Require(spec.address || spec.module || profile.module, spec.name + ": needs an address or module");
        if (entry.contains("offsets")) {
            const auto& offsets = entry["offsets"];
            Require(offsets.is_array() && offsets.size() <= 32, spec.name + ": offsets must be an array of at most 32 entries");
            for (const auto& offset : offsets) spec.offsets.push_back(ParseInteger(offset, spec.name + ".offsets", true));
        }
        Require(!spec.offsets.empty() || spec.address, spec.name + ": module reads need at least one offset");
        if (VariableType(spec.type)) {
            Require(entry.contains("length"), spec.name + ": type needs length in bytes");
            const auto length = ParseInteger(entry["length"], spec.name + ".length").magnitude;
            Require(length > 0 && length <= MaxRead && (spec.type != "utf16" || length % 2 == 0), spec.name + ": invalid byte length");
            spec.length = static_cast<std::size_t>(length);
        } else Require(!entry.contains("length"), spec.name + ": length is only used for bytes and strings");
        (void)spec.ByteSize(8); // rejects unknown scalar types
        profile.values.push_back(std::move(spec));
    }
    return profile;
}
Profile LoadProfile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream), "Cannot open profile: " + ToUtf8(path.wstring()));
    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    Require(length >= 0 && length <= 4 * 1024 * 1024, "Profile exceeds 4 MiB");
    stream.seekg(0);
    try { return ParseProfile(Json::parse(stream)); }
    catch (const Json::exception& error) { throw std::runtime_error(std::string("Invalid profile JSON: ") + error.what()); }
}
ModuleBases GetModuleBases(DWORD pid) {
    ModuleBases result;
    for (const auto& module : Modules(pid)) {
        const auto key = LowerName(module.name);
        Require(result.emplace(key, module.base).second, "Ambiguous module: " + ToUtf8(module.name));
    }
    return result;
}
static std::uint64_t AddOffset(std::uint64_t address, Offset offset) {
    if (offset.negative) {
        Require(offset.magnitude <= address, "Pointer arithmetic underflow");
        return address - offset.magnitude;
    }
    Require(offset.magnitude <= UINT64_MAX - address, "Pointer arithmetic overflow");
    return address + offset.magnitude;
}
static std::vector<std::uint8_t> ExactRead(const ReadFunction& read, std::uint64_t address, std::size_t size, unsigned pointerSize) {
    CheckAddress(address, size, pointerSize);
    auto bytes = read(address, size);
    Require(bytes.size() == size, "Short read at " + Hex(address) + ": expected " + std::to_string(size) + " bytes");
    return bytes;
}
static std::uint64_t UnsignedBytes(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}
std::uint64_t ResolveAddress(const ValueSpec& spec, std::uint64_t base, unsigned pointerSize, const ReadFunction& read) {
    auto address = base;
    if (!spec.offsets.empty()) {
        address = AddOffset(address, spec.offsets.front());
        for (std::size_t i = 1; i < spec.offsets.size(); ++i) {
            const auto pointer = UnsignedBytes(ExactRead(read, address, pointerSize, pointerSize));
            Require(pointer != 0, spec.name + ": null pointer at " + Hex(address));
            CheckAddress(pointer, 1, pointerSize);
            address = AddOffset(pointer, spec.offsets[i]);
        }
    }
    CheckAddress(address, spec.ByteSize(pointerSize), pointerSize);
    return address;
}
template<class T> static T Scalar(const std::vector<std::uint8_t>& bytes) {
    T result{};
    std::memcpy(&result, bytes.data(), sizeof(result));
    return result;
}
Json DecodeValue(const ValueSpec& spec, const std::vector<std::uint8_t>& data, unsigned pointerSize) {
    Require(data.size() == spec.ByteSize(pointerSize), spec.name + ": incomplete value");
    if (spec.type == "pointer") return Hex(UnsignedBytes(data), pointerSize * 2);
    if (spec.type == "bool") return data[0] != 0;
    if (spec.type == "i8") return Scalar<std::int8_t>(data);
    if (spec.type == "u8") return Scalar<std::uint8_t>(data);
    if (spec.type == "i16") return Scalar<std::int16_t>(data);
    if (spec.type == "u16") return Scalar<std::uint16_t>(data);
    if (spec.type == "i32") return Scalar<std::int32_t>(data);
    if (spec.type == "u32") return Scalar<std::uint32_t>(data);
    if (spec.type == "i64") return Scalar<std::int64_t>(data);
    if (spec.type == "u64") return Scalar<std::uint64_t>(data);
    if (spec.type == "f32" || spec.type == "f64") {
        const double value = spec.type == "f32" ? Scalar<float>(data) : Scalar<double>(data);
        if (std::isnan(value)) return "nan";
        if (!std::isfinite(value)) return value < 0 ? "-inf" : "inf";
        return value;
    }
    if (spec.type == "bytes") {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for (const auto byte : data) {
            if (!result.empty()) result += ' ';
            result += digits[byte >> 4]; result += digits[byte & 15];
        }
        return result;
    }
    if (spec.type == "utf8") {
        const auto end = std::find(data.begin(), data.end(), 0);
        return ToUtf8(ToWide(std::string(data.begin(), end), false));
    }
    std::wstring result;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        const auto unit = static_cast<wchar_t>(data[i] | (static_cast<unsigned>(data[i + 1]) << 8));
        if (!unit) break;
        result += unit;
    }
    return ToUtf8(result);
}
Json ReadValues(const Profile& profile, unsigned pointerSize, const ModuleBases& modules, const ReadFunction& read) {
    ModuleBases normalized;
    for (const auto& item : modules) normalized.emplace(LowerName(item.first), item.second);
    auto values = Json::array();
    for (const auto& spec : profile.values) {
        Json value{{"name", spec.name}, {"type", spec.type}};
        try {
            auto base = spec.address.value_or(0);
            if (!spec.address) {
                const auto module = spec.module ? spec.module : profile.module;
                Require(module.has_value(), spec.name + ": module is missing");
                const auto found = normalized.find(LowerName(*module));
                Require(found != normalized.end(), "Module not loaded: " + ToUtf8(*module));
                base = found->second;
            }
            const auto address = ResolveAddress(spec, base, pointerSize, read);
            const auto data = ExactRead(read, address, spec.ByteSize(pointerSize), pointerSize);
            value["address"] = Hex(address);
            value["value"] = DecodeValue(spec, data, pointerSize);
        } catch (const std::exception& error) { value["error"] = error.what(); }
        values.push_back(std::move(value));
    }
    return values;
}
Json ReadProfile(const Profile& profile, const ProcessMemory& memory) {
    Require(!profile.pointerSize || profile.pointerSize == memory.PointerSize(), "Profile pointer_size does not match target architecture");
    const bool needsModules = std::any_of(profile.values.begin(), profile.values.end(), [](const auto& spec) { return !spec.address; });
    const auto bases = needsModules ? GetModuleBases(memory.Pid()) : ModuleBases{};
    return ReadValues(profile, memory.PointerSize(), bases, [&](auto address, auto size) { return memory.Read(address, size); });
}
bool HasErrors(const Json& values) {
    return std::any_of(values.begin(), values.end(), [](const auto& value) { return value.contains("error"); });
}
}
