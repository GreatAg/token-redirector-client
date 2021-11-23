#include "dbgcommon.h"
#include "usbip_proto.h"
#include "usbip_vhci_api.h"

#include <usbdi.h>
#include <usbuser.h>
#include <ntstrsafe.h>

const char *bmrequest_type_str(BM_REQUEST_TYPE r)
{
	static const char* v[] = { "STANDARD", "CLASS", "VENDOR", "BMREQUEST_3" };
	ASSERT(r.Type < ARRAYSIZE(v));
	return v[r.Type];
}

const char *bmrequest_recipient_str(BM_REQUEST_TYPE r)
{
	static const char* v[] = { "DEVICE", "INTERFACE", "ENDPOINT", "OTHER" };
	ASSERT(r.Recipient < ARRAYSIZE(v));
	return v[r.Recipient];
}

const char *brequest_str(UCHAR bRequest)
{
	switch (bRequest) {
	case USB_REQUEST_GET_STATUS: return "GET_STATUS";
	case USB_REQUEST_CLEAR_FEATURE: return "CLEAR_FEATURE";
	case USB_REQUEST_SET_FEATURE: return "SET_FEATURE";
	case USB_REQUEST_SET_ADDRESS: return "SET_ADDRESS";
	case USB_REQUEST_GET_DESCRIPTOR: return "GET_DESCRIPTOR";
	case USB_REQUEST_SET_DESCRIPTOR: return "SET_DESCRIPTOR";
	case USB_REQUEST_GET_CONFIGURATION: return "GET_CONFIGURATION";
	case USB_REQUEST_SET_CONFIGURATION: return "SET_CONFIGURATION";
	case USB_REQUEST_GET_INTERFACE: return "GET_INTERFACE";
	case USB_REQUEST_SET_INTERFACE: return "SET_INTERFACE";
	case USB_REQUEST_SYNC_FRAME: return "SYNC_FRAME";
	case USB_REQUEST_GET_FIRMWARE_STATUS: return "GET_FIRMWARE_STATUS";
	case USB_REQUEST_SET_FIRMWARE_STATUS: return "SET_FIRMWARE_STATUS";
	case USB_REQUEST_SET_SEL: return "SET_SEL";
	case USB_REQUEST_ISOCH_DELAY: return "ISOCH_DELAY";
	}

	return "USB_REQUEST_?";
}

const char *dbg_usbd_status(USBD_STATUS status)
{
        switch (status) {
        case USBD_STATUS_SUCCESS: 
                return "USBD_STATUS_SUCCESS";
        case USBD_STATUS_PORT_OPERATION_PENDING:
                return "USBD_STATUS_PORT_OPERATION_PENDING";
        case USBD_STATUS_PENDING:
                return "USBD_STATUS_PENDING";
        case USBD_STATUS_CRC:
                return "USBD_STATUS_CRC";
        case USBD_STATUS_BTSTUFF:
                return "USBD_STATUS_BTSTUFF";
        case USBD_STATUS_DATA_TOGGLE_MISMATCH:
                return "USBD_STATUS_DATA_TOGGLE_MISMATCH";
        case USBD_STATUS_STALL_PID:
                return "USBD_STATUS_STALL_PID";
        case USBD_STATUS_DEV_NOT_RESPONDING:
                return "USBD_STATUS_DEV_NOT_RESPONDING";
        case USBD_STATUS_PID_CHECK_FAILURE:
                return "USBD_STATUS_PID_CHECK_FAILURE";
        case USBD_STATUS_UNEXPECTED_PID:
                return "USBD_STATUS_UNEXPECTED_PID";
        case USBD_STATUS_DATA_OVERRUN:
                return "USBD_STATUS_DATA_OVERRUN";
        case USBD_STATUS_DATA_UNDERRUN:
                return "USBD_STATUS_DATA_UNDERRUN";
        case USBD_STATUS_BUFFER_OVERRUN:
                return "USBD_STATUS_BUFFER_OVERRUN";
        case USBD_STATUS_BUFFER_UNDERRUN:
                return "USBD_STATUS_BUFFER_UNDERRUN";
        case USBD_STATUS_NOT_ACCESSED:
                return "USBD_STATUS_NOT_ACCESSED";
        case USBD_STATUS_FIFO:
                return "USBD_STATUS_FIFO";
        case USBD_STATUS_XACT_ERROR:
                return "USBD_STATUS_XACT_ERROR";
        case USBD_STATUS_BABBLE_DETECTED:
                return "USBD_STATUS_BABBLE_DETECTED";
        case USBD_STATUS_DATA_BUFFER_ERROR:
                return "USBD_STATUS_DATA_BUFFER_ERROR";
        case USBD_STATUS_NO_PING_RESPONSE:
                return "USBD_STATUS_NO_PING_RESPONSE";
        case USBD_STATUS_INVALID_STREAM_TYPE:
                return "USBD_STATUS_INVALID_STREAM_TYPE";
        case USBD_STATUS_INVALID_STREAM_ID:
                return "USBD_STATUS_INVALID_STREAM_ID";
        case USBD_STATUS_ENDPOINT_HALTED:
                return "USBD_STATUS_ENDPOINT_HALTED";
        case USBD_STATUS_INVALID_URB_FUNCTION:
                return "USBD_STATUS_INVALID_URB_FUNCTION";
        case USBD_STATUS_INVALID_PARAMETER:
                return "USBD_STATUS_INVALID_PARAMETER";
        case USBD_STATUS_ERROR_BUSY:
                return "USBD_STATUS_ERROR_BUSY";
        case USBD_STATUS_INVALID_PIPE_HANDLE:
                return "USBD_STATUS_INVALID_PIPE_HANDLE";
        case USBD_STATUS_NO_BANDWIDTH:
                return "USBD_STATUS_NO_BANDWIDTH";
        case USBD_STATUS_INTERNAL_HC_ERROR:
                return "USBD_STATUS_INTERNAL_HC_ERROR";
        case USBD_STATUS_ERROR_SHORT_TRANSFER:
                return "USBD_STATUS_ERROR_SHORT_TRANSFER";
        case USBD_STATUS_BAD_START_FRAME:
                return "USBD_STATUS_BAD_START_FRAME";
        case USBD_STATUS_ISOCH_REQUEST_FAILED:
                return "USBD_STATUS_ISOCH_REQUEST_FAILED";
        case USBD_STATUS_FRAME_CONTROL_OWNED:
                return "USBD_STATUS_FRAME_CONTROL_OWNED";
        case USBD_STATUS_FRAME_CONTROL_NOT_OWNED:
                return "USBD_STATUS_FRAME_CONTROL_NOT_OWNED";
        case USBD_STATUS_NOT_SUPPORTED:
                return "USBD_STATUS_NOT_SUPPORTED";
        case USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR:
                return "USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR";
        case USBD_STATUS_INSUFFICIENT_RESOURCES:
                return "USBD_STATUS_INSUFFICIENT_RESOURCES";
        case USBD_STATUS_SET_CONFIG_FAILED:
                return "USBD_STATUS_SET_CONFIG_FAILED";
        case USBD_STATUS_BUFFER_TOO_SMALL:
                return "USBD_STATUS_BUFFER_TOO_SMALL";
        case USBD_STATUS_INTERFACE_NOT_FOUND:
                return "USBD_STATUS_INTERFACE_NOT_FOUND";
        case USBD_STATUS_INAVLID_PIPE_FLAGS:
                return "USBD_STATUS_INAVLID_PIPE_FLAGS";
        case USBD_STATUS_TIMEOUT:
                return "USBD_STATUS_TIMEOUT";
        case USBD_STATUS_DEVICE_GONE:
                return "USBD_STATUS_DEVICE_GONE";
        case USBD_STATUS_STATUS_NOT_MAPPED:
                return "USBD_STATUS_STATUS_NOT_MAPPED";
        case USBD_STATUS_HUB_INTERNAL_ERROR:
                return "USBD_STATUS_HUB_INTERNAL_ERROR";
        case USBD_STATUS_CANCELED:
                return "USBD_STATUS_CANCELED";
        case USBD_STATUS_ISO_NOT_ACCESSED_BY_HW:
                return "USBD_STATUS_ISO_NOT_ACCESSED_BY_HW";
        case USBD_STATUS_ISO_TD_ERROR:
                return "USBD_STATUS_ISO_TD_ERROR";
        case USBD_STATUS_ISO_NA_LATE_USBPORT:
                return "USBD_STATUS_ISO_NA_LATE_USBPORT";
        case USBD_STATUS_ISO_NOT_ACCESSED_LATE:
                return "USBD_STATUS_ISO_NOT_ACCESSED_LATE";
        case USBD_STATUS_BAD_DESCRIPTOR:
                return "USBD_STATUS_BAD_DESCRIPTOR";
        case USBD_STATUS_BAD_DESCRIPTOR_BLEN:
                return "USBD_STATUS_BAD_DESCRIPTOR_BLEN";
        case USBD_STATUS_BAD_DESCRIPTOR_TYPE:
                return "USBD_STATUS_BAD_DESCRIPTOR_TYPE";
        case USBD_STATUS_BAD_INTERFACE_DESCRIPTOR:
                return "USBD_STATUS_BAD_INTERFACE_DESCRIPTOR";
        case USBD_STATUS_BAD_ENDPOINT_DESCRIPTOR:
                return "USBD_STATUS_BAD_ENDPOINT_DESCRIPTOR";
        case USBD_STATUS_BAD_INTERFACE_ASSOC_DESCRIPTOR:
                return "USBD_STATUS_BAD_INTERFACE_ASSOC_DESCRIPTOR";
        case USBD_STATUS_BAD_CONFIG_DESC_LENGTH:
                return "USBD_STATUS_BAD_CONFIG_DESC_LENGTH";
        case USBD_STATUS_BAD_NUMBER_OF_INTERFACES:
                return "USBD_STATUS_BAD_NUMBER_OF_INTERFACES";
        case USBD_STATUS_BAD_NUMBER_OF_ENDPOINTS:
                return "USBD_STATUS_BAD_NUMBER_OF_ENDPOINTS";
        case USBD_STATUS_BAD_ENDPOINT_ADDRESS:
                return "USBD_STATUS_BAD_ENDPOINT_ADDRESS";
        }

        return "USBD_STATUS_?";
}

const char *dbg_ioctl_code(int ioctl_code)
{
	static_assert(sizeof(ioctl_code) == sizeof(IOCTL_USBIP_VHCI_PLUGIN_HARDWARE), "assert");

	switch (ioctl_code) {
	case IOCTL_USBIP_VHCI_PLUGIN_HARDWARE: return "IOCTL_USBIP_VHCI_PLUGIN_HARDWARE";
	case IOCTL_USBIP_VHCI_UNPLUG_HARDWARE: return "IOCTL_USBIP_VHCI_UNPLUG_HARDWARE";
	case IOCTL_USBIP_VHCI_GET_PORTS_STATUS: return "IOCTL_USBIP_VHCI_GET_PORTS_STATUS";
	case IOCTL_INTERNAL_USB_CYCLE_PORT: return "IOCTL_INTERNAL_USB_CYCLE_PORT";
	case IOCTL_INTERNAL_USB_ENABLE_PORT: return "IOCTL_INTERNAL_USB_ENABLE_PORT";
	case IOCTL_INTERNAL_USB_GET_BUS_INFO: return "IOCTL_INTERNAL_USB_GET_BUS_INFO";
	case IOCTL_INTERNAL_USB_GET_BUSGUID_INFO: return "IOCTL_INTERNAL_USB_GET_BUSGUID_INFO";
	case IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME: return "IOCTL_INTERNAL_USB_GET_CONTROLLER_NAME";
	case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE: return "IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE";
	case IOCTL_INTERNAL_USB_GET_HUB_COUNT: return "IOCTL_INTERNAL_USB_GET_HUB_COUNT";
	case IOCTL_INTERNAL_USB_GET_HUB_NAME: return "IOCTL_INTERNAL_USB_GET_HUB_NAME";
	case IOCTL_INTERNAL_USB_GET_PARENT_HUB_INFO: return "IOCTL_INTERNAL_USB_GET_PARENT_HUB_INFO";
	case IOCTL_INTERNAL_USB_GET_PORT_STATUS: return "IOCTL_INTERNAL_USB_GET_PORT_STATUS";
	case IOCTL_INTERNAL_USB_RESET_PORT: return "IOCTL_INTERNAL_USB_RESET_PORT";
	case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO: return "IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO";
	case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION: return "IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION";
	case IOCTL_INTERNAL_USB_SUBMIT_URB: return "IOCTL_INTERNAL_USB_SUBMIT_URB";
	case IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS: return "IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS";
	case IOCTL_USB_DIAG_IGNORE_HUBS_ON: return "IOCTL_USB_DIAG_IGNORE_HUBS_ON";
	case IOCTL_USB_DIAG_IGNORE_HUBS_OFF: return "IOCTL_USB_DIAG_IGNORE_HUBS_OFF";
	case IOCTL_USB_DIAGNOSTIC_MODE_OFF: return "IOCTL_USB_DIAGNOSTIC_MODE_OFF";
	case IOCTL_USB_DIAGNOSTIC_MODE_ON: return "IOCTL_USB_DIAGNOSTIC_MODE_ON";
	case IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION: return "IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION";
	case IOCTL_USB_GET_HUB_CAPABILITIES: return "IOCTL_USB_GET_HUB_CAPABILITIES";
	case IOCTL_USB_GET_ROOT_HUB_NAME: return "IOCTL_USB_GET_ROOT_HUB_NAME";
//	case IOCTL_GET_HCD_DRIVERKEY_NAME: return "IOCTL_GET_HCD_DRIVERKEY_NAME";
//	case IOCTL_USB_GET_NODE_INFORMATION: return "IOCTL_USB_GET_NODE_INFORMATION";
	case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION: return "IOCTL_USB_GET_NODE_CONNECTION_INFORMATION";
	case IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES: return "IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES";
	case IOCTL_USB_GET_NODE_CONNECTION_NAME: return "IOCTL_USB_GET_NODE_CONNECTION_NAME";
//	case IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME: return "IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME";
	case IOCTL_USB_HCD_DISABLE_PORT: return "IOCTL_USB_HCD_DISABLE_PORT";
	case IOCTL_USB_HCD_ENABLE_PORT: return "IOCTL_USB_HCD_ENABLE_PORT";
	case IOCTL_USB_HCD_GET_STATS_1: return "IOCTL_USB_HCD_GET_STATS_1";
//	case IOCTL_USB_HCD_GET_STATS_2: return "IOCTL_USB_HCD_GET_STATS_2";
	case IOCTL_USB_USER_REQUEST: return "IOCTL_USB_USER_REQUEST";
//	case IOCTL_USB_GET_HUB_CAPABILITIES: return "IOCTL_USB_GET_HUB_CAPABILITIES";
//	case IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES: return "IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES";
	case IOCTL_USB_HUB_CYCLE_PORT: return "IOCTL_USB_HUB_CYCLE_PORT";
	case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX: return "IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX";
	case IOCTL_USB_RESET_HUB: return "IOCTL_USB_RESET_HUB";
	case IOCTL_USB_GET_HUB_CAPABILITIES_EX: return "IOCTL_USB_GET_HUB_CAPABILITIES_EX";
//	case IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES: return "IOCTL_USB_GET_NODE_CONNECTION_ATTRIBUTES";
	case IOCTL_USB_GET_HUB_INFORMATION_EX: return "IOCTL_USB_GET_HUB_INFORMATION_EX";
	case IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES: return "IOCTL_USB_GET_PORT_CONNECTOR_PROPERTIES";
	case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2: return "IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2";
	}

	return "IOCTL_?";
}

const char *dbg_usbip_hdr(char *buf, unsigned int len, const struct usbip_header *hdr)
{
	NTSTRSAFE_PSTR end = NULL;
	size_t remaining = 0;

	NTSTATUS st = RtlStringCbPrintfExA(buf, len, &end, &remaining, STRSAFE_NULL_ON_FAILURE, 
		"seq:%u,%s,ep:%#04x,cmd:%d",
		hdr->base.seqnum, 
		hdr->base.direction == USBIP_DIR_OUT ? "out" : "in",
		hdr->base.ep, 
		hdr->base.command);

	if (st != STATUS_SUCCESS) {
		return "dbg_usbip_hdr error";
	}

	switch (hdr->base.command) {
	case USBIP_CMD_SUBMIT:
		RtlStringCbPrintfA(end, remaining, "(submit),tlen:%d,intv:%d", 
			hdr->u.cmd_submit.transfer_buffer_length, hdr->u.cmd_submit.interval);
		break;
	case USBIP_RET_SUBMIT:
		RtlStringCbPrintfA(end, remaining, "(ret_submit),alen:%u", hdr->u.ret_submit.actual_length);
		break;
	case USBIP_CMD_UNLINK:
		RtlStringCbPrintfA(end, remaining, "(unlink),unlinkseq:%u", hdr->u.cmd_unlink.seqnum);
		break;
	case USBIP_RET_UNLINK:
		RtlStringCbPrintfA(end, remaining, "(ret_unlink),st:%u", hdr->u.ret_unlink.status);
		break;
	case USBIP_RESET_DEV:
		RtlStringCbCopyA(end, remaining, "(reset_dev)");
		break;
	}

	return buf;
}

const char *dbg_usb_setup_packet(char *buf, unsigned int len, const void *packet)
{
	const USB_DEFAULT_PIPE_SETUP_PACKET *r = packet;

	NTSTATUS st = RtlStringCbPrintfA(buf, len, 
		"[%#02hhx(%s|%s|%s), %s(%#02hhx), wValue %#04hx, wIndex %#04hx, wLength %#04hx(%d)]",
		r->bmRequestType,
		bmrequest_dir_str(r->bmRequestType),
		bmrequest_type_str(r->bmRequestType),
		bmrequest_recipient_str(r->bmRequestType),
		brequest_str(r->bRequest),
		r->bRequest,
		r->wValue,
		r->wIndex, 
		r->wLength,
		r->wLength);

	return st != STATUS_INVALID_PARAMETER ? buf : "dbg_usb_setup_packet invalid parameter";
}
