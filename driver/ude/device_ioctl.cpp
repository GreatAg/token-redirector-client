/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "device_ioctl.h"
#include "trace.h"
#include "device_ioctl.tmh"

#include "context.h"
#include "wsk_context.h"
#include "device.h"
#include "device_queue.h"
#include "proto.h"
#include "network.h"
#include "ioctl.h"
#include "wsk_receive.h"

#include <libdrv\pdu.h>
#include <libdrv\ch9.h>
#include <libdrv\usb_util.h>
#include <libdrv\wsk_cpp.h>
#include <libdrv\dbgcommon.h>
#include <libdrv\usbd_helper.h>

namespace
{

using namespace usbip;

/*
 * wsk_irp->Tail.Overlay.DriverContext[] are zeroed.
 *
 * In general, you must not touch IRP that was put in Cancel-Safe Queue because it can be canceled at any moment.
 * You should remove IRP from the CSQ and then use it. BUT you can access IRP if you shure it is alive.
 *
 * To avoid copying of URB's transfer buffer, it must not be completed until this handler will be called.
 * This means that:
 * 1.EvtIoCanceledOnQueue must not complete IRP if it's called before send_complete because WskSend can still access
 *   IRP transfer buffer.
 * 2.WskReceive must not complete IRP if it's called before send_complete because send_complete modifies request_context.status.
 * 3.EvtIoCanceledOnQueue and WskReceive are mutually exclusive because IRP is dequeued from the CSQ.
 * 4.Thus, send_complete can run concurrently with EvtIoCanceledOnQueue or WskReceive.
 * 
 * @see wsk_receive.cpp, complete 
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS send_complete(
        _In_ DEVICE_OBJECT*, _In_ IRP *wsk_irp, _In_reads_opt_(_Inexpressible_("varies")) void *Context)
{
        wsk_context_ptr ctx(static_cast<wsk_context*>(Context), true);
        auto request = ctx->request;

        request_ctx *req_ctx;
        request_status old_status;

        if (request) { // NULL for send_cmd_unlink
                req_ctx = get_request_ctx(request);
                old_status = atomic_set_status(*req_ctx, REQ_SEND_COMPLETE);
        } else {
                req_ctx = nullptr;
                old_status = REQ_NO_HANDLE;
        }

        auto &st = wsk_irp->IoStatus;

        TraceWSK("wsk irp %04x, %!STATUS!, Information %Iu, %!request_status!",
                  ptr04x(wsk_irp), st.Status, st.Information, old_status);

        if (!request) {
                // nothing to do
        } else if (NT_SUCCESS(st.Status)) { // request has sent
                switch (old_status) {
                case REQ_RECV_COMPLETE:
                        complete(request);
                        break;
                case REQ_CANCELED:
                        UdecxUrbCompleteWithNtStatus(request, STATUS_CANCELLED);
                        break;
                }
        } else if (auto victim = device::dequeue_request(ctx->dev_ctx->queue, req_ctx->seqnum)) { // ctx->hdr.base.seqnum is in network byte order
                NT_ASSERT(victim == request);
                UdecxUrbCompleteWithNtStatus(victim, STATUS_UNSUCCESSFUL);
        } else if (old_status == REQ_CANCELED) {
                UdecxUrbCompleteWithNtStatus(request, STATUS_CANCELLED);
        }

        if (st.Status == STATUS_FILE_FORCED_CLOSED) {
                auto dev = get_device(ctx->dev_ctx);
                device::destroy(dev);
        }

        return StopCompletion;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto prepare_wsk_buf(_Out_ WSK_BUF &buf, _Inout_ wsk_context &ctx, _Inout_opt_ const URB *transfer_buffer)
{
        NT_ASSERT(!ctx.mdl_buf);

        if (transfer_buffer && is_transfer_direction_out(ctx.hdr)) { // TransferFlags can have wrong direction
                if (auto err = make_transfer_buffer_mdl(ctx.mdl_buf, URB_BUF_LEN, ctx.is_isoc, IoReadAccess, *transfer_buffer)) {
                        Trace(TRACE_LEVEL_ERROR, "make_transfer_buffer_mdl %!STATUS!", err);
                        return err;
                }
        }

        ctx.mdl_hdr.next(ctx.mdl_buf); // always replace tie from previous call

        if (ctx.is_isoc) {
                NT_ASSERT(ctx.mdl_isoc);
                byteswap(ctx.isoc, number_of_packets(ctx));
                auto t = tail(ctx.mdl_hdr); // ctx.mdl_buf can be a chain
                t->Next = ctx.mdl_isoc.get();
        }

        buf.Mdl = ctx.mdl_hdr.get();
        buf.Offset = 0;
        buf.Length = get_total_size(ctx.hdr);

        NT_ASSERT(verify(buf, ctx.is_isoc));
        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto send(_In_ wsk_context_ptr &ctx, _In_ device_ctx &dev,
        _Inout_opt_ const URB *transfer_buffer = nullptr,
        _In_ bool log_setup = false)
{
        WSK_BUF buf;

        if (auto err = prepare_wsk_buf(buf, *ctx, transfer_buffer)) {
                return err;
        } else {
                char str[DBG_USBIP_HDR_BUFSZ];
                TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "req %04x -> %Iu%s",
                        ptr04x(ctx->request), buf.Length, dbg_usbip_hdr(str, sizeof(str), &ctx->hdr, log_setup));
        }

        if (auto req = ctx->request) {
                auto &req_ctx = *get_request_ctx(req);
                req_ctx.seqnum = ctx->hdr.base.seqnum;

                NT_ASSERT(is_valid_seqnum(req_ctx.seqnum));
                NT_ASSERT(req_ctx.status == REQ_INIT);

                if (auto err = WdfRequestForwardToIoQueue(req, dev.queue)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfRequestForwardToIoQueue %!STATUS!", err);
                        return err;
                }
        }

        byteswap_header(ctx->hdr, swap_dir::host2net);

        auto wsk_irp = ctx->wsk_irp; // do not access ctx or wsk_irp after send
        IoSetCompletionRoutine(wsk_irp, send_complete, ctx.release(), true, true, true);

        auto err = send(dev.sock(), &buf, WSK_FLAG_NODELAY, wsk_irp);
        NT_ASSERT(err != STATUS_NOT_SUPPORTED);

        TraceWSK("wsk irp %04x, %Iu bytes, %!STATUS!", ptr04x(wsk_irp), buf.Length, err);
        return STATUS_PENDING;
}

using urb_function_t = NTSTATUS (WDFREQUEST, URB&, const endpoint_ctx&);

/*
 * Any URBs queued for such an endpoint should normally be unlinked by the driver before clearing the halt condition,
 * as described in sections 5.7.5 and 5.8.5 of the USB 2.0 spec.
 *
 * Thus, a driver must call URB_FUNCTION_ABORT_PIPE before URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL.
 * For that reason abort_pipe(urbr->vpdo, r.PipeHandle) is not called here.
 *
 * Linux server catches control transfer USB_REQ_CLEAR_FEATURE/USB_ENDPOINT_HALT and calls usb_clear_halt which
 * a) Issues USB_REQ_CLEAR_FEATURE/USB_ENDPOINT_HALT # URB_FUNCTION_SYNC_CLEAR_STALL
 * b) Calls usb_reset_endpoint # URB_FUNCTION_SYNC_RESET_PIPE
 *
 * See: <linux>/drivers/usb/usbip/stub_rx.c, is_clear_halt_cmd
 *      <linux>/drivers/usb/core/message.c, usb_clear_halt
 *
 * ###
 * 
 * URB_FUNCTION_SYNC_CLEAR_STALL must issue USB_REQ_CLEAR_FEATURE, USB_ENDPOINT_HALT.
 * URB_FUNCTION_SYNC_RESET_PIPE must call usb_reset_endpoint.
 *
 * Linux server catches control transfer USB_REQ_CLEAR_FEATURE/USB_ENDPOINT_HALT and calls usb_clear_halt.
 * There is no way to distinguish these two operations without modifications on server's side.
 * It can be implemented by passing extra parameter
 * a) wValue=1 to clear halt
 * b) wValue=2 to call usb_reset_endpoint
 *
 * @see <linux>/drivers/usb/usbip/stub_rx.c, is_clear_halt_cmd
 * @see <linux>/drivers/usb/core/message.c, usb_clear_halt, usb_reset_endpoint
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS pipe_request(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbPipeRequest;
        NT_ASSERT(r.PipeHandle);

        switch (urb.UrbHeader.Function) {
        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_SYNC_RESET_PIPE:
        case URB_FUNCTION_SYNC_CLEAR_STALL:
        case URB_FUNCTION_CLOSE_STATIC_STREAMS:
                break;
        }
/*
        TraceUrb("req %04x -> %s: PipeHandle %04x(EndpointAddress %#02x, %!USBD_PIPE_TYPE!, Interval %d)",
                ptr04x(irp),
                urb_function_str(r.Hdr.Function),
                ph4log(r.PipeHandle),
                get_endpoint_address(r.PipeHandle),
                get_endpoint_type(r.PipeHandle),
                get_endpoint_interval(r.PipeHandle));
*/
        TraceUrb("req %04x -> %s: PipeHandle %04x",
                ptr04x(request), urb_function_str(r.Hdr.Function), ptr04x(r.PipeHandle));

        UdecxUrbComplete(request, USBD_STATUS_NOT_SUPPORTED);
        return STATUS_PENDING;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS control_get_status_request(
        _In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&, _In_ UCHAR recipient)
{
        auto &r = urb.UrbControlGetStatusRequest;

        TraceUrb("req %04x -> %s: TransferBufferLength %lu (must be 2), %s, Index %hd",
                ptr04x(request), urb_function_str(r.Hdr.Function), r.TransferBufferLength,
                request_recipient(recipient), r.Index);

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_status_from_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_get_status_request(request, urb, endp, USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_status_from_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_get_status_request(request, urb, endp, USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_status_from_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_get_status_request(request, urb, endp, USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_status_from_other(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_get_status_request(request, urb, endp, USB_RECIP_OTHER);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS control_vendor_class_request(
        _In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&, _In_ UCHAR type, _In_ UCHAR recipient)
{
        auto &r = urb.UrbControlVendorClassRequest;

        {
                char buf[USBD_TRANSFER_FLAGS_BUFBZ];
                TraceUrb("req %04x -> %s: %s, TransferBufferLength %lu, %s, %s, %s(%!#XBYTE!), Value %#hx, Index %#hx",
                        ptr04x(request), urb_function_str(r.Hdr.Function), usbd_transfer_flags(buf, sizeof(buf), r.TransferFlags),
                        r.TransferBufferLength, request_type(type), request_recipient(recipient), 
                        brequest_str(r.Request), r.Request, r.Value, r.Index);
        }

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS vendor_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_VENDOR, USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS vendor_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_VENDOR, USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS vendor_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_VENDOR, USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS vendor_other(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_VENDOR, USB_RECIP_OTHER);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS class_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_CLASS, USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS class_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_CLASS, USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS class_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_CLASS, USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS class_other(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_vendor_class_request(request, urb, endp, USB_TYPE_CLASS, USB_RECIP_OTHER);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS control_descriptor_request(
        _In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&,
        _In_ bool dir_in, _In_ UCHAR recipient)
{
        auto &r = urb.UrbControlDescriptorRequest;

        TraceUrb("req %04x -> %s: TransferBufferLength %lu(%#lx), %s %s, Index %#x, %!usb_descriptor_type!, LanguageId %#04hx",
                ptr04x(request), urb_function_str(r.Hdr.Function), r.TransferBufferLength, r.TransferBufferLength,
                dir_in ? "IN" : "OUT", request_recipient(recipient),
                r.Index, r.DescriptorType, r.LanguageId);

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_descriptor_from_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_IN), USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_descriptor_to_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_OUT), USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_descriptor_from_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_IN), USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_descriptor_to_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_OUT), USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_descriptor_from_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_IN), USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_descriptor_to_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_descriptor_request(request, urb, endp, bool(USB_DIR_OUT), USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS control_feature_request(
        _In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&,
        _In_ UCHAR bRequest, _In_ UCHAR recipient)
{
        auto &r = urb.UrbControlFeatureRequest;

        TraceUrb("req %04x -> %s: %s, %s, FeatureSelector %#hx, Index %#hx",
                ptr04x(request), urb_function_str(r.Hdr.Function), request_recipient(recipient), brequest_str(bRequest),
                r.FeatureSelector, r.Index);

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_feature_to_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_SET_FEATURE, USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_feature_to_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_SET_FEATURE, USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_feature_to_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_SET_FEATURE, USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS set_feature_to_other(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_SET_FEATURE, USB_RECIP_OTHER);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS clear_feature_to_device(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_DEVICE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS clear_feature_to_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_INTERFACE);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS clear_feature_to_endpoint(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_ENDPOINT);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS clear_feature_to_other(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        return control_feature_request(request, urb, endp, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_OTHER);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS select_configuration(_In_ WDFREQUEST, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbSelectConfiguration;
        auto cd = r.ConfigurationDescriptor; // nullptr if unconfigured

        UCHAR value = cd ? cd->bConfigurationValue : 0;
        TraceUrb("bConfigurationValue %d", value);

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS select_interface(_In_ WDFREQUEST, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbSelectInterface;
        auto &iface = r.Interface;

        TraceUrb("InterfaceNumber %d, AlternateSetting %d", iface.InterfaceNumber, iface.AlternateSetting);
        return STATUS_NOT_IMPLEMENTED;
}

/*
 * Can't be implemented without server's support.
 * In any case the result will be irrelevant due to network latency.
 *
 * See: <linux>//drivers/usb/core/usb.c, usb_get_current_frame_number.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_current_frame_number(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &num = urb.UrbGetCurrentFrameNumber.FrameNumber;
//      num = 0; // vpdo.current_frame_number;

        TraceUrb("req %04x, FrameNumber %lu", ptr04x(request), num);

        UdecxUrbComplete(request, USBD_STATUS_NOT_SUPPORTED);
        return STATUS_PENDING;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS control_transfer(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        static_assert(offsetof(_URB_CONTROL_TRANSFER, SetupPacket) == offsetof(_URB_CONTROL_TRANSFER_EX, SetupPacket));
        auto &r = urb.UrbControlTransferEx;

        {
                char buf_flags[USBD_TRANSFER_FLAGS_BUFBZ];
                char buf_setup[USB_SETUP_PKT_STR_BUFBZ];

                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu, Timeout %lu, %s",
                        ptr04x(request), ptr04x(r.PipeHandle),
                        usbd_transfer_flags(buf_flags, sizeof(buf_flags), r.TransferFlags),
                        r.TransferBufferLength,
                        urb.UrbHeader.Function == URB_FUNCTION_CONTROL_TRANSFER_EX ? r.Timeout : 0,
                        usb_setup_pkt_str(buf_setup, sizeof(buf_setup), r.SetupPacket));
        }

        auto &dev = *get_device_ctx(endp.device);

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        auto dir_out = is_transfer_dir_out(urb.UrbControlTransfer); // default control pipe is bidirectional

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp, r.TransferFlags, r.TransferBufferLength, &dir_out)) {
                return err;
        }

        static_assert(sizeof(ctx->hdr.u.cmd_submit.setup) == sizeof(r.SetupPacket));
        RtlCopyMemory(ctx->hdr.u.cmd_submit.setup, r.SetupPacket, sizeof(r.SetupPacket));

        return send(ctx, dev, &urb, true);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS bulk_or_interrupt_transfer(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        auto &r = urb.UrbBulkOrInterruptTransfer;

        {
                auto func = urb.UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL ? ", chained mdl" : " ";
                char buf[USBD_TRANSFER_FLAGS_BUFBZ];

                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu%s",
                        ptr04x(request), ptr04x(r.PipeHandle), usbd_transfer_flags(buf, sizeof(buf), r.TransferFlags),
                        r.TransferBufferLength, func);
        }

        auto type = usb_endpoint_type(endp.descriptor);

        if (!(type == UsbdPipeTypeBulk || type == UsbdPipeTypeInterrupt)) {
                Trace(TRACE_LEVEL_ERROR, "%!USBD_PIPE_TYPE!", type);
                return STATUS_INVALID_PARAMETER;
        }

        auto &dev = *get_device_ctx(endp.device);

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp, r.TransferFlags, r.TransferBufferLength)) {
                return err;
        }

        return send(ctx, dev, &urb);
}

/*
 * USBD_ISO_PACKET_DESCRIPTOR.Length is not used (zero) for USB_DIR_OUT transfer.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
auto repack(_In_ usbip_iso_packet_descriptor *d, _In_ const _URB_ISOCH_TRANSFER &r)
{
        ULONG length = 0;

        for (ULONG i = 0; i < r.NumberOfPackets; ++d) {

                auto offset = r.IsoPacket[i].Offset;
                auto next_offset = ++i < r.NumberOfPackets ? r.IsoPacket[i].Offset : r.TransferBufferLength;

                if (next_offset >= offset && next_offset <= r.TransferBufferLength) {
                        d->offset = offset;
                        d->length = next_offset - offset;
                        d->actual_length = 0;
                        d->status = 0;
                        length += d->length;
                } else {
                        Trace(TRACE_LEVEL_ERROR, "[%lu] next_offset(%lu) >= offset(%lu) && next_offset <= r.TransferBufferLength(%lu)",
                                i, next_offset, offset, r.TransferBufferLength);
                        return STATUS_INVALID_PARAMETER;
                }
        }

        NT_ASSERT(length == r.TransferBufferLength);
        return STATUS_SUCCESS;
}

/*
 * USBD_START_ISO_TRANSFER_ASAP is appended because URB_GET_CURRENT_FRAME_NUMBER is not implemented.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS isoch_transfer(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx &endp)
{
        auto &r = urb.UrbIsochronousTransfer;

        {
                const char *func = urb.UrbHeader.Function == URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL ? ", chained mdl" : ".";
                char buf[USBD_TRANSFER_FLAGS_BUFBZ];
                TraceUrb("req %04x -> PipeHandle %04x, %s, TransferBufferLength %lu, StartFrame %lu, NumberOfPackets %lu, ErrorCount %lu%s",
                        ptr04x(request), ptr04x(r.PipeHandle),
                        usbd_transfer_flags(buf, sizeof(buf), r.TransferFlags),
                        r.TransferBufferLength,
                        r.StartFrame,
                        r.NumberOfPackets,
                        r.ErrorCount,
                        func);
        }

        auto type = usb_endpoint_type(endp.descriptor);

        if (type != UsbdPipeTypeIsochronous) {
                Trace(TRACE_LEVEL_ERROR, "%!USBD_PIPE_TYPE!", type);
                return STATUS_INVALID_PARAMETER;
        }

        auto &dev = *get_device_ctx(endp.device);

        wsk_context_ptr ctx(&dev, request, r.NumberOfPackets);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp, 
                               r.TransferFlags | USBD_START_ISO_TRANSFER_ASAP, r.TransferBufferLength)) {
                return err;
        }

        if (auto err = repack(ctx->isoc, r)) {
                return err;
        }

        if (auto cmd = &ctx->hdr.u.cmd_submit) {
                cmd->start_frame = r.StartFrame;
                cmd->number_of_packets = r.NumberOfPackets;
        }

        return send(ctx, dev, &urb);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS function_deprecated(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        TraceUrb("req %04x, %s not supported", ptr04x(request), urb_function_str(urb.UrbHeader.Function));

        UdecxUrbComplete(request, USBD_STATUS_NOT_SUPPORTED);
        return STATUS_PENDING;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_configuration(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbControlGetConfigurationRequest;
        TraceUrb("req %04x -> TransferBufferLength %lu (must be 1)", ptr04x(request), r.TransferBufferLength);

        return STATUS_NOT_IMPLEMENTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_interface(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbControlGetInterfaceRequest;

        TraceUrb("req %04x -> TransferBufferLength %lu (must be 1), Interface %hu",
                  ptr04x(request), r.TransferBufferLength, r.Interface);

        return STATUS_NOT_IMPLEMENTED;
}

/*
 * @see https://github.com/libusb/libusb/blob/master/examples/xusb.c, read_ms_winsub_feature_descriptors
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_ms_feature_descriptor(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbOSFeatureDescriptorRequest;

        TraceUrb("req %04x -> TransferBufferLength %lu, %s, InterfaceNumber %d, MS_PageIndex %d, "
                "MS_FeatureDescriptorIndex %d",
                ptr04x(request), r.TransferBufferLength, request_recipient(r.Recipient), r.InterfaceNumber, 
                r.MS_PageIndex, r.MS_FeatureDescriptorIndex);

        return STATUS_NOT_IMPLEMENTED;
}

/*
 * See: <kernel>/drivers/usb/core/message.c, usb_set_isoch_delay.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS get_isoch_pipe_transfer_path_delays(
        _In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbGetIsochPipeTransferPathDelays;

        TraceUrb("req %04x -> PipeHandle %04x, MaximumSendPathDelayInMilliSeconds %lu, MaximumCompletionPathDelayInMilliSeconds %lu",
                ptr04x(request), ptr04x(r.PipeHandle),
                r.MaximumSendPathDelayInMilliSeconds,
                r.MaximumCompletionPathDelayInMilliSeconds);

        UdecxUrbComplete(request, USBD_STATUS_NOT_SUPPORTED);
        return STATUS_PENDING;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(urb_function_t)
NTSTATUS open_static_streams(_In_ WDFREQUEST request, _In_ URB &urb, _In_ const endpoint_ctx&)
{
        auto &r = urb.UrbOpenStaticStreams;

        TraceUrb("req %04x -> PipeHandle %04x, NumberOfStreams %lu, StreamInfoVersion %hu, StreamInfoSize %hu",
                ptr04x(request), ptr04x(r.PipeHandle), r.NumberOfStreams, r.StreamInfoVersion, r.StreamInfoSize);

        UdecxUrbComplete(request, USBD_STATUS_NOT_SUPPORTED);
        return STATUS_PENDING;
}

urb_function_t* const urb_functions[] =
{
        select_configuration,
        select_interface,
        pipe_request, // URB_FUNCTION_ABORT_PIPE

        function_deprecated, // URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL
        function_deprecated, // URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL

        function_deprecated, // URB_FUNCTION_GET_FRAME_LENGTH
        function_deprecated, // URB_FUNCTION_SET_FRAME_LENGTH
        get_current_frame_number,

        control_transfer,
        bulk_or_interrupt_transfer,
        isoch_transfer,

        get_descriptor_from_device,
        set_descriptor_to_device,

        set_feature_to_device,
        set_feature_to_interface,
        set_feature_to_endpoint,

        clear_feature_to_device,
        clear_feature_to_interface,
        clear_feature_to_endpoint,

        get_status_from_device,
        get_status_from_interface,
        get_status_from_endpoint,

        nullptr, // URB_FUNCTION_RESERVED_0X0016

        vendor_device,
        vendor_interface,
        vendor_endpoint,

        class_device,
        class_interface,
        class_endpoint,

        nullptr, // URB_FUNCTION_RESERVE_0X001D

        pipe_request, // URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL

        class_other,
        vendor_other,

        get_status_from_other,

        set_feature_to_other,
        clear_feature_to_other,

        get_descriptor_from_endpoint,
        set_descriptor_to_endpoint,

        get_configuration,
        get_interface,

        get_descriptor_from_interface,
        set_descriptor_to_interface,

        get_ms_feature_descriptor,

        nullptr, // URB_FUNCTION_RESERVE_0X002B
        nullptr, // URB_FUNCTION_RESERVE_0X002C
        nullptr, // URB_FUNCTION_RESERVE_0X002D
        nullptr, // URB_FUNCTION_RESERVE_0X002E
        nullptr, // URB_FUNCTION_RESERVE_0X002F

        pipe_request, // URB_FUNCTION_SYNC_RESET_PIPE
        pipe_request, // URB_FUNCTION_SYNC_CLEAR_STALL

        control_transfer, // URB_FUNCTION_CONTROL_TRANSFER_EX

        nullptr, // URB_FUNCTION_RESERVE_0X0033
        nullptr, // URB_FUNCTION_RESERVE_0X0034

        open_static_streams,
        pipe_request, // URB_FUNCTION_CLOSE_STATIC_STREAMS

        bulk_or_interrupt_transfer, // URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL
        isoch_transfer, // URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL

        nullptr, // 0x0039
        nullptr, // 0x003A
        nullptr, // 0x003B
        nullptr, // 0x003C

        get_isoch_pipe_transfer_path_delays // URB_FUNCTION_GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS
};

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto usb_submit_urb(_In_ WDFREQUEST request, _In_ UDECXUSBENDPOINT endp)
{
        auto irp = WdfRequestWdmGetIrp(request);
        auto &urb = *urb_from_irp(irp);
        
        auto func = urb.UrbHeader.Function;
        auto &ctx = *get_endpoint_ctx(endp);

        TraceDbg("%s(%#04x), dev %04x, endp %04x", urb_function_str(func), func, ptr04x(ctx.device), ptr04x(endp));

        if (auto handler = func < ARRAYSIZE(urb_functions) ? urb_functions[func] : nullptr) {
                return handler(request, urb, ctx);
        }

        Trace(TRACE_LEVEL_ERROR, "%s(%#04x) has no handler (reserved?)", urb_function_str(func), func);
        return STATUS_NOT_IMPLEMENTED;
}

/*
 * FIXME: not sure that this request really has URB. 
 */
auto verify_urb(_In_ WDFREQUEST request, _In_ USHORT Function)
{
        auto irp = WdfRequestWdmGetIrp(request);
        auto stack = IoGetCurrentIrpStackLocation(irp);
        auto ioctl = stack->Parameters.DeviceIoControl.IoControlCode; // DeviceIoControlCode(irp)

        auto ok = stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL && 
                  ioctl == IOCTL_INTERNAL_USBEX_SUBMIT_URB;

        if (!ok) {
                Trace(TRACE_LEVEL_ERROR, "Unexpected IoControlCode %s(%#x)", 
                        internal_device_control_name(ioctl), ioctl);
                return STATUS_INVALID_DEVICE_REQUEST;
        }

        auto urb = urb_from_irp(irp);
        auto func = urb->UrbHeader.Function;

        if (func != Function) {
                Trace(TRACE_LEVEL_ERROR, "%s expected, got %s(%#x)", 
                        urb_function_str(Function), urb_function_str(func), func);
                return STATUS_INVALID_PARAMETER;
        }

        return STATUS_SUCCESS;
}

/*
 * WdfRequestGetIoQueue(request) returns queue that does not belong to the device (not its EP0 or others).
 * get_endpoint(WdfRequestGetIoQueue(request)) causes BSOD.
 */
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
auto do_select(_In_ UDECXUSBDEVICE device, _In_ WDFREQUEST request, _In_ ULONG params)
{
        if (auto ctx = get_request_ctx(request)) {
                ctx->urb_function_select = true;
        }

        auto &dev = *get_device_ctx(device);
        auto &endp = *get_endpoint_ctx(dev.ep0);

        wsk_context_ptr ctx(&dev, request);
        if (!ctx) {
                return STATUS_INSUFFICIENT_RESOURCES;
        }

        const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;
        bool dir_out = true;

        if (auto err = set_cmd_submit_usbip_header(ctx->hdr, dev, endp, TransferFlags, 0, &dir_out)) {
                return err;
        }

        bool iface = params >> 16; 

        auto &pkt = get_submit_setup(ctx->hdr);
        pkt.bmRequestType.B = USB_DIR_OUT | USB_TYPE_STANDARD | UCHAR(iface ? USB_RECIP_INTERFACE : USB_RECIP_DEVICE);
        pkt.bRequest = iface ? USB_REQUEST_SET_INTERFACE : USB_REQUEST_SET_CONFIGURATION;
        NT_ASSERT(!pkt.wLength);

        if (iface) {
                pkt.wValue.W = UCHAR(params >> 8); // Alternative Setting
                pkt.wIndex.W = UCHAR(params); // Interface
        } else {
                pkt.wValue.W = UCHAR(params); // Configuration Value
                NT_ASSERT(!pkt.wIndex.W);
        }

        return ::send(ctx, dev, nullptr, true);
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::select_configuration(
        _In_ UDECXUSBDEVICE dev, _In_ WDFREQUEST request, _In_ UCHAR ConfigurationValue)
{
        TraceDbg("dev %04x, ConfigurationValue %d", ptr04x(dev), ConfigurationValue);

        if (auto err = verify_urb(request, URB_FUNCTION_SELECT_CONFIGURATION)) {
                return err;
        }

        return do_select(dev, request, ConfigurationValue);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::device::select_interface(
        _In_ UDECXUSBDEVICE dev, _In_ WDFREQUEST request, _In_ UCHAR InterfaceNumber, _In_ UCHAR InterfaceSetting)
{
        TraceDbg("dev %04x, bInterfaceNumber %d, bAlternateSetting %d", ptr04x(dev), InterfaceNumber, InterfaceSetting);

        if (auto err = verify_urb(request, URB_FUNCTION_SELECT_INTERFACE)) {
                return err;
        }

        auto params = (1UL << 16) | (InterfaceSetting << 8) | InterfaceNumber;
        return do_select(dev, request, params);
}

/*
 * IRP_MJ_INTERNAL_DEVICE_CONTROL 
 */
_Function_class_(EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void NTAPI usbip::device::internal_device_control(
        _In_ WDFQUEUE Queue, 
        _In_ WDFREQUEST Request,
        _In_ size_t /*OutputBufferLength*/,
        _In_ size_t /*InputBufferLength*/,
        _In_ ULONG IoControlCode)
{
        if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB) {
                auto st = STATUS_INVALID_DEVICE_REQUEST;
                Trace(TRACE_LEVEL_ERROR, "%s(%#08lX) %!STATUS!", internal_device_control_name(IoControlCode), 
                        IoControlCode, st);

                WdfRequestComplete(Request, st);
                return;
        }

        auto endp = get_endpoint(Queue);
        auto st = usb_submit_urb(Request, endp);

        if (st != STATUS_PENDING) {
                TraceDbg("%!STATUS!", st);
                UdecxUrbCompleteWithNtStatus(Request, st);
        }
}
