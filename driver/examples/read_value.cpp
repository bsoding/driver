#include "../memory_reader.hpp"

// Compile-checked example for another translation unit in this driver.
// AuthorizedProcess is borrowed from an already validated request; its owner
// retains the reference until this call returns. Value must be nonpaged.
_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS ReadCounterForRequest(
    _In_ PEPROCESS AuthorizedProcess,
    _In_ ULONGLONG CounterAddress,
    _Out_ ULONG* Value) noexcept
{
    SIZE_T bytesRead = 0;
    return offset_reader::kernel::ReadMemory(
        AuthorizedProcess, CounterAddress, Value, sizeof(*Value), &bytesRead);
}
