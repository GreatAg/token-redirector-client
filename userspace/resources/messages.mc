;// Copyright (C) 2023 - 2024 Vadym Hrynchyshyn <vadimgrn@gmail.com>
;//
;#pragma once
;
;#include <minwindef.h>
;
;using USBIP_STATUS = DWORD;
;#define USBIP_ERROR_SUCCESS              ((USBIP_STATUS)0x00000000L)
;
;#ifdef _NTSTATUS_
;
;namespace usbip
;{
;
;static_assert(sizeof(USBIP_STATUS) == sizeof(NTSTATUS));
;static_assert(USBIP_ERROR_SUCCESS == STATUS_SUCCESS);
;
;constexpr auto as_ntstatus(_In_ USBIP_STATUS status)
;{
;       return static_cast<NTSTATUS>(status);
;}
;
;constexpr auto as_usbip_status(_In_ NTSTATUS status)
;{
;       return static_cast<USBIP_STATUS>(status);
;}
;
;} // namespace usbip
;
;#endif // #ifdef _NTSTATUS_
;

MessageIdTypedef=USBIP_STATUS

FacilityNames=(
Driver=0x100:FACILITY_DRIVER
Library=0x101:FACILITY_LIBRARY
Device=0x102:FACILITY_DEVICE)

LanguageNames=(English=0x409:MSG00409)

MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_GENERAL
Language=English
Driver command completed unsuccessfully.
.

;// STATUS_NOT_FOUND -> WSAHOST_NOT_FOUND
MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_ADDRINFO_NOT_FOUND
Language=English
The host name cannot be resolved, no such host is known
.

;// STATUS_NO_MATCH
MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_ADDRINFO_NO_MATCH
Language=English
The host name cannot be resolved, there was no match
.

;// STATUS_IO_TIMEOUT -> WSAETIMEDOUT
MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_CONNECT_IO_TIMEOUT
Language=English
A connection attempt failed because the connected party did not properly respond after a period of time, or established connection failed because connected host has failed to respond 
.

MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_VERSION
Language=English
Incompatible USB/IP protocol version.
.

MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_PROTOCOL
Language=English
USB/IP protocol violation.
.

MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_PORTFULL
Language=English
No free port on USB/IP hub.
.

MessageId=
Severity=Error
Facility=Driver
SymbolicName=USBIP_ERROR_ABI
Language=English
ABI mismatch, unexpected size of the input structure
.

MessageId=
Severity=Error
Facility=Device
SymbolicName=USBIP_ERROR_ST_NA
Language=English
Device not available.
.

MessageId=
Severity=Error
Facility=Device
SymbolicName=USBIP_ERROR_ST_DEV_BUSY
Language=English
Device busy (already exported).
.

MessageId=
Severity=Error
Facility=Device
SymbolicName=USBIP_ERROR_ST_DEV_ERR
Language=English
Device in error state.
.

MessageId=
Severity=Error
Facility=Device
SymbolicName=USBIP_ERROR_ST_NODEV
Language=English
Device not found by bus id.
.

MessageId=
Severity=Error
Facility=Device
SymbolicName=USBIP_ERROR_ST_ERROR
Language=English
Device unexpected response.
.

MessageId=
Severity=Error
Facility=Library
SymbolicName=USBIP_ERROR_VHCI_NOT_FOUND
Language=English
VHCI device not found, driver not loaded?
.

MessageId=
Severity=Error
Facility=Library
SymbolicName=USBIP_ERROR_DEVICE_INTERFACE_LIST
Language=English
Multiple instances of VHCI device interface found.
.

MessageId=
Severity=Error
Facility=Library
SymbolicName=USBIP_ERROR_DRIVER_RESPONSE
Language=English
Unexpected response from the driver (length, content, etc.).
.
