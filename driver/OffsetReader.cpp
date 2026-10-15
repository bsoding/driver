#include <ntifs.h>
#include <wdf.h>
#include <wdmsec.h>
#include "protocol.h"
#include "memory_reader.hpp"

/* Process-specific read access, as defined by the Windows process API. */
#define OR_PROCESS_VM_READ ((ACCESS_MASK)0x0010)

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD OffsetReaderUnload;
EVT_WDF_IO_IN_CALLER_CONTEXT OffsetReaderIoInCallerContext;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OffsetReaderIoDeviceControl;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OffsetReaderRequestCleanup;

typedef struct _REQUEST_CONTEXT {
    PEPROCESS Process;
    ULONGLONG Address;
    ULONG Size;
} REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_CONTEXT, RequestGetContext)

static VOID
ReleaseRequestProcess(_In_ WDFREQUEST Request)
{
    REQUEST_CONTEXT* context = RequestGetContext(Request);
    if (context->Process != NULL) {
        PEPROCESS process = context->Process;
        context->Process = NULL;
        ObDereferenceObject(process);
    }
}

VOID
OffsetReaderRequestCleanup(_In_ WDFOBJECT Object)
{
    /* Also covers a request canceled before reaching the queue callback. */
    ReleaseRequestProcess((WDFREQUEST)Object);
}

static NTSTATUS
PrepareReadForCaller(_In_ WDFREQUEST Request)
{
    NTSTATUS status;
    OR_READ_REQUEST input;
    OR_READ_REQUEST* inputBuffer;
    PVOID outputBuffer;
    PEPROCESS process = NULL;
    REQUEST_CONTEXT* context = RequestGetContext(Request);
    ULONGLONG highestUserAddress = (ULONGLONG)(ULONG_PTR)MmHighestUserAddress;
    PIRP irp = WdfRequestWdmGetIrp(Request);

    /* A user handle must be resolved in the originating process, at PASSIVE. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        WdfRequestGetRequestorMode(Request) != UserMode ||
        IoGetRequestorProcess(irp) != PsGetCurrentProcess()) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), (PVOID*)&inputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* METHOD_BUFFERED shares the input and output buffer. Save input first. */
    input = *inputBuffer;
    if (input.Version != OR_PROTOCOL_VERSION || input.Reserved != 0 || input.Reserved2 != 0 ||
        input.Size == 0 || input.Size > OR_MAX_READ_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (input.ProcessHandle == 0 || input.ProcessHandle > (ULONGLONG)MAXLONG_PTR) {
        return STATUS_INVALID_HANDLE;
    }

    /* Reject kernel addresses, wraparound, and ranges crossing the user limit. */
    if (input.Address == 0 || input.Address > highestUserAddress ||
        ((ULONGLONG)input.Size - 1) > highestUserAddress - input.Address) {
        return STATUS_INVALID_ADDRESS;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, input.Size, &outputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Never turn an arbitrary PID into a privileged process handle. */
    status = ObReferenceObjectByHandle(
        (HANDLE)(ULONG_PTR)input.ProcessHandle,
        OR_PROCESS_VM_READ,
        *PsProcessType,
        UserMode,
        (PVOID*)&process,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context->Address = input.Address;
    context->Size = input.Size;
    context->Process = process;
    return STATUS_SUCCESS;
}

static NTSTATUS
ReadPreparedRequest(_In_ WDFREQUEST Request, _Out_ size_t* BytesRead)
{
    REQUEST_CONTEXT* context = RequestGetContext(Request);
    PVOID outputBuffer;
    NTSTATUS status;
    *BytesRead = 0;
    if (context->Process == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, context->Size, &outputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* The caller-context callback already checked PROCESS_VM_READ and holds a
       reference until request completion. Buffered I/O supplies nonpaged output. */
    return offset_reader::kernel::ReadMemory(
        context->Process, context->Address, outputBuffer, context->Size, BytesRead);
}

VOID
OffsetReaderIoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS parameters;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(Request, &parameters);
    if (parameters.Type == WdfRequestTypeDeviceControl) {
        switch (parameters.Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_OR_INFO:
            status = STATUS_SUCCESS;
            break;
        case IOCTL_OR_READ:
            status = PrepareReadForCaller(Request);
            break;
        default:
            break;
        }
    }
    if (NT_SUCCESS(status)) {
        /* The queue owns lifetime/cancellation after a process reference is captured. */
        status = WdfDeviceEnqueueRequest(Device, Request);
    }
    if (!NT_SUCCESS(status)) {
        ReleaseRequestProcess(Request);
        WdfRequestComplete(Request, status);
    }
}

VOID
OffsetReaderIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t information = 0;
    OR_INFO* info;
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_OR_INFO:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*info), (PVOID*)&info, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(info, sizeof(*info));
            info->Version = OR_PROTOCOL_VERSION;
            info->MaxReadSize = OR_MAX_READ_SIZE;
            info->DriverPointerSize = sizeof(PVOID);
            information = sizeof(*info);
        }
        break;
    case IOCTL_OR_READ:
        status = ReadPreparedRequest(Request, &information);
        break;
    default:
        break;
    }

    ReleaseRequestProcess(Request);
    WdfRequestCompleteWithInformation(Request, status, information);
}

VOID
OffsetReaderUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    /* KMDF tears down the control device, symbolic link, and queue. */
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDRIVER driver;
    WDFDEVICE device;
    PWDFDEVICE_INIT deviceInit;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(deviceName, L"\\Device\\OffsetReader");
    DECLARE_CONST_UNICODE_STRING(symbolicLink, L"\\DosDevices\\OffsetReader");

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = OffsetReaderUnload;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, &driver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceInit = WdfControlDeviceInitAllocate(driver, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfDeviceInitSetDeviceType(deviceInit, OR_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetIoInCallerContextCallback(deviceInit, OffsetReaderIoInCallerContext);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REQUEST_CONTEXT);
    attributes.EvtCleanupCallback = OffsetReaderRequestCleanup;
    WdfDeviceInitSetRequestAttributes(deviceInit, &attributes);
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        if (deviceInit != NULL) {
            WdfDeviceInitFree(deviceInit);
        }
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = OffsetReaderIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (NT_SUCCESS(status)) {
        status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    }
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }
    WdfControlFinishInitializing(device);
    return STATUS_SUCCESS;
}

