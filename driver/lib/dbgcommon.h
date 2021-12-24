#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <ntddk.h>
#include <usb.h>

struct usbip_header;

__inline const char *bmrequest_dir_str(BM_REQUEST_TYPE r)
{
	return r.Dir == BMREQUEST_HOST_TO_DEVICE ? "OUT" : "IN";
}

const char *bmrequest_type_str(BM_REQUEST_TYPE r);
const char *bmrequest_recipient_str(BM_REQUEST_TYPE r);

const char *brequest_str(UCHAR bRequest);

const char *dbg_usbd_status(USBD_STATUS status);
const char *dbg_ioctl_code(ULONG ioctl_code);

const char *usbd_pipe_type_str(USBD_PIPE_TYPE t);
const char *urb_function_str(int function);

enum { DBG_USBIP_HDR_BUFSZ = 255 };
const char *dbg_usbip_hdr(char *buf, size_t len, const struct usbip_header *hdr);

enum { USB_SETUP_PKT_STR_BUFBZ = 128 };
const char *usb_setup_pkt_str(char *buf, size_t len, const void *packet);

enum { USBD_TRANSFER_FLAGS_BUFBZ = 36 };
const char *usbd_transfer_flags(char *buf, size_t len, ULONG TransferFlags);

#ifdef __cplusplus
}
#endif
