#include "offset_reader/memory.hpp"

// Call this function from an ordinary x64 C++ application.
// The default backend requires the signed driver to be installed and running.
std::uint32_t ReadCounter(DWORD pid, std::uint64_t address) {
    const offset_reader::ProcessMemory memory(pid);
    return memory.ReadValue<std::uint32_t>(address);
}
