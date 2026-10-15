#include "memory_reader.hpp"

namespace offset_reader::kernel {

_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS ReadMemory(
    _In_ PEPROCESS Process,
    _In_ ULONGLONG Address,
    _Out_writes_bytes_(Size) PVOID Destination,
    _In_range_(1, OR_MAX_READ_SIZE) SIZE_T Size,
    _Out_ PSIZE_T BytesRead) noexcept
{
    if (BytesRead == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesRead = 0;
    if (Process == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Destination == nullptr || Size == 0 || Size > OR_MAX_READ_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    const auto highestUserAddress = reinterpret_cast<ULONGLONG>(MmHighestUserAddress);
    if (Address == 0 || Address > highestUserAddress ||
        static_cast<ULONGLONG>(Size - 1) > highestUserAddress - Address) {
        return STATUS_INVALID_ADDRESS;
    }

    RtlZeroMemory(Destination, Size);
    KAPC_STATE apcState;
    MM_COPY_ADDRESS source = {};
    source.VirtualAddress = reinterpret_cast<PVOID>(Address);
    SIZE_T transferred = 0;
    NTSTATUS status;
    KeStackAttachProcess(reinterpret_cast<PRKPROCESS>(Process), &apcState);
    __try {
        // Keep the attached interval limited to the existing bounded copy.
        status = MmCopyMemory(Destination, source, Size, MM_COPY_MEMORY_VIRTUAL, &transferred);
    }
    __finally {
        KeUnstackDetachProcess(&apcState);
    }

    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(Destination, Size);
        return status;
    }
    if (transferred != Size) {
        RtlZeroMemory(Destination, Size);
        return STATUS_PARTIAL_COPY;
    }
    *BytesRead = transferred;
    return STATUS_SUCCESS;
}

} // namespace offset_reader::kernel
