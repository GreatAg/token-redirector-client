#pragma once

#include "basetype.h"
#include "vhci_dev.h"

PAGEABLE NTSTATUS vhci_ioctl_user_request(pvhci_dev_t vhci, PVOID buffer, ULONG inlen, PULONG poutlen);