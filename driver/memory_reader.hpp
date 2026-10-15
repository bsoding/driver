#pragma once

#include <ntifs.h>
#include "protocol.h"

namespace offset_reader::kernel {

// Internal to the driver image. The caller must hold a reference to Process for
// the entire call and must already have authorized read access for its requester.
// Destination must point to writable nonpaged system memory of at least Size bytes.
// The function borrows the process reference; it does not release it.
// BytesRead is zero on failure. Only STATUS_SUCCESS returns a complete value.
_IRQL_requires_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS ReadMemory(
    _In_ PEPROCESS Process,
    _In_ ULONGLONG Address,
    _Out_writes_bytes_(Size) PVOID Destination,
    _In_range_(1, OR_MAX_READ_SIZE) SIZE_T Size,
    _Out_ PSIZE_T BytesRead) noexcept;

} // namespace offset_reader::kernel
