#include "vhci_driver.h"
#include "vhci_read.tmh"

#include "vhci_urbr.h"

extern NTSTATUS
store_urbr_partial(WDFREQUEST req_read, purb_req_t urbr);

static purb_req_t
find_pending_urbr(pctx_vusb_t vusb)
{
	purb_req_t	urbr;

	if (IsListEmpty(&vusb->head_urbr_pending))
		return NULL;

	urbr = CONTAINING_RECORD(vusb->head_urbr_pending.Flink, urb_req_t, list_state);
	urbr->seq_num = ++(vusb->seq_num);
	RemoveEntryListInit(&urbr->list_state);
	return urbr;
}

static VOID
req_read_cancelled(WDFREQUEST req_read)
{
	pctx_vusb_t	vusb;

	TRD(READ, "a pending read req cancelled");

	vusb = *TO_PVUSB(WdfRequestGetFileObject(req_read));
	WdfSpinLockAcquire(vusb->spin_lock);
	if (vusb->pending_req_read == req_read) {
		vusb->pending_req_read = NULL;
	}
	WdfSpinLockRelease(vusb->spin_lock);

	WdfRequestComplete(req_read, STATUS_CANCELLED);
}

static NTSTATUS
read_vusb(pctx_vusb_t vusb, WDFREQUEST req)
{
	purb_req_t	urbr;
	NTSTATUS status;

	TRD(READ, "Enter");

	WdfSpinLockAcquire(vusb->spin_lock);

	if (vusb->pending_req_read) {
		WdfSpinLockRelease(vusb->spin_lock);
		return STATUS_INVALID_DEVICE_REQUEST;
	}
	if (vusb->urbr_sent_partial != NULL) {
		urbr = vusb->urbr_sent_partial;

		WdfSpinLockRelease(vusb->spin_lock);

		status = store_urbr_partial(req, urbr);

		WdfSpinLockAcquire(vusb->spin_lock);
		vusb->len_sent_partial = 0;
	}
	else {
		urbr = find_pending_urbr(vusb);
		if (urbr == NULL) {
			vusb->pending_req_read = req;

			status = WdfRequestMarkCancelableEx(req, req_read_cancelled);
			if (!NT_SUCCESS(status)) {
				if (vusb->pending_req_read == req) {
					vusb->pending_req_read = NULL;
				}
			}
			WdfSpinLockRelease(vusb->spin_lock);
			if (!NT_SUCCESS(status)) {
				WdfRequestComplete(req, status);
				TRE(READ, "a pending read req cancelled: %!STATUS!", status);
			}

			return STATUS_PENDING;
		}
		vusb->urbr_sent_partial = urbr;
		WdfSpinLockRelease(vusb->spin_lock);

		status = store_urbr(req, urbr);

		WdfSpinLockAcquire(vusb->spin_lock);
	}

	if (status != STATUS_SUCCESS) {
		RemoveEntryListInit(&urbr->list_all);
		WdfSpinLockRelease(vusb->spin_lock);

		complete_urbr(urbr, status);
	}
	else {
		if (vusb->len_sent_partial == 0) {
			InsertTailList(&vusb->head_urbr_sent, &urbr->list_state);
			vusb->urbr_sent_partial = NULL;
		}
		WdfSpinLockRelease(vusb->spin_lock);
	}
	return status;
}

VOID
io_read(_In_ WDFQUEUE queue, _In_ WDFREQUEST req, _In_ size_t len)
{
	pctx_vusb_t	vusb;
	NTSTATUS	status;

	UNREFERENCED_PARAMETER(queue);

	TRD(READ, "Enter: len: %u", (ULONG)len);

	vusb = *TO_PVUSB(WdfRequestGetFileObject(req));
	if (vusb->invalid) {
		TRD(READ, "vusb disconnected: port: %u", vusb->port);
		status = STATUS_DEVICE_NOT_CONNECTED;
	}
	else
		status = read_vusb(vusb, req);

	if (status != STATUS_PENDING) {
		WdfRequestComplete(req, status);
	}

	TRD(READ, "Leave: %!STATUS!", status);
}
