#pragma once

/* Fixed-width wire layout shared with the native C++ client. */
#define OR_PROTOCOL_VERSION 1u
#define OR_MAX_READ_SIZE (64u * 1024u)
#define OR_DEVICE_TYPE 0x8000u
#define IOCTL_OR_INFO CTL_CODE(OR_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_OR_READ CTL_CODE(OR_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct _OR_READ_REQUEST {
    ULONG Version;
    ULONG Reserved;
    ULONGLONG ProcessHandle;
    ULONGLONG Address;
    ULONG Size;
    ULONG Reserved2;
} OR_READ_REQUEST;

typedef struct _OR_INFO {
    ULONG Version;
    ULONG MaxReadSize;
    ULONG DriverPointerSize;
    ULONG Reserved;
} OR_INFO;

C_ASSERT(sizeof(OR_READ_REQUEST) == 32);
C_ASSERT(FIELD_OFFSET(OR_READ_REQUEST, ProcessHandle) == 8);
C_ASSERT(FIELD_OFFSET(OR_READ_REQUEST, Address) == 16);
C_ASSERT(sizeof(OR_INFO) == 16);

