/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <ntstrsafe.h>
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>

#include <debug_interface.h>
#include <suspend_interface.h>

#include "pdo.h"
#include "hid.h"
#include "ring.h"
#include "mrsw.h"
#include "thread.h"
#include "vkbd.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

struct _XENVKBD_HID_CONTEXT {
    PXENVKBD_PDO                Pdo;
    XENVKBD_MRSW_LOCK           Lock;
    LONG                        References;
    PXENVKBD_RING               Ring;
    PXENVKBD_FRONTEND           Frontend;
    BOOLEAN                     Enabled;
    ULONG                       Version;
    XENHID_HID_CALLBACK         Callback;
    PVOID                       Argument;
};

#define XENVKBD_VKBD_TAG  'FIV'

static FORCEINLINE NTSTATUS
HidCopyBuffer(
    IN  PVOID       Buffer,
    IN  ULONG       Length,
    IN  const VOID  *Source,
    IN  ULONG       SourceLength,
    OUT PULONG      Returned
    )
{
    if (Buffer == NULL)
        return STATUS_INVALID_PARAMETER;
    if (Length < SourceLength)
        return STATUS_NO_MEMORY;

    RtlCopyMemory(Buffer,
                  Source,
                  SourceLength);
    if (Returned)
        *Returned = SourceLength;
    return STATUS_SUCCESS;
}

static FORCEINLINE PVOID
__HidAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENVKBD_VKBD_TAG);
}

static FORCEINLINE VOID
__HidFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XENVKBD_VKBD_TAG);
}

static NTSTATUS
HidEnable(
    IN  PINTERFACE          Interface,
    IN  XENHID_HID_CALLBACK Callback,
    IN  PVOID               Argument
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    AcquireMrswLockExclusive(&Context->Lock, &Irql);

    if (Context->Enabled)
        goto done;

    Context->Callback = Callback;
    Context->Argument = Argument;

    Context->Enabled = TRUE;

    KeMemoryBarrier();

    status = FrontendSetState(Context->Frontend, FRONTEND_ENABLED);
    if (!NT_SUCCESS(status)) {
        if (status != STATUS_DEVICE_NOT_READY)
            goto fail1;
    }

done:
    ReleaseMrswLockExclusive(&Context->Lock, Irql, FALSE);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Context->Enabled = FALSE;

    KeMemoryBarrier();

    Context->Argument = NULL;
    Context->Callback = NULL;

    ReleaseMrswLockExclusive(&Context->Lock, Irql, FALSE);

    return status;
}

static VOID
HidDisable(
    IN  PINTERFACE          Interface
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    KIRQL                   Irql;

    Trace("====>\n");

    AcquireMrswLockExclusive(&Context->Lock, &Irql);

    if (!Context->Enabled) {
        ReleaseMrswLockExclusive(&Context->Lock, Irql, FALSE);
        goto done;
    }

    Context->Enabled = FALSE;

    KeMemoryBarrier();

    (VOID) FrontendSetState(Context->Frontend, FRONTEND_CONNECTED);

    ReleaseMrswLockExclusive(&Context->Lock, Irql, TRUE);

    Context->Argument = NULL;
    Context->Callback = NULL;

    ReleaseMrswLockShared(&Context->Lock);

done:
    Trace("<====\n");
}

static NTSTATUS
HidGetDeviceAttributes(
    IN  PINTERFACE          Interface,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = HidCopyBuffer(Buffer,
                           Length,
                           &VkbdDeviceAttributes,
                           sizeof(VkbdDeviceAttributes),
                           Returned);

    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    return status;
}

static NTSTATUS
HidGetDeviceDescriptor(
    IN  PINTERFACE          Interface,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = HidCopyBuffer(Buffer,
                           Length,
                           &VkbdDeviceDescriptor,
                           sizeof(VkbdDeviceDescriptor),
                           Returned);

    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    return status;
}

static NTSTATUS
HidGetReportDescriptor(
    IN  PINTERFACE          Interface,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = HidCopyBuffer(Buffer,
                           Length,
                           VkbdReportDescriptor,
                           sizeof(VkbdReportDescriptor),
                           Returned);

    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    return status;
}

static NTSTATUS
HidGetString(
    IN  PINTERFACE          Interface,
    IN  ULONG               Identifier,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    PXENVKBD_PDO            Pdo;
    PXENVKBD_FDO            Fdo;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    Pdo = FrontendGetPdo(Context->Frontend);
    Fdo = PdoGetFdo(Pdo);

    // Ignore LangID
    switch (Identifier & 0xFF) {
    case HID_STRING_ID_IMANUFACTURER:
        status = HidCopyBuffer(Buffer,
                               Length,
                               FdoGetVendorName(Fdo),
                               (ULONG)strlen(FdoGetVendorName(Fdo)),
                               Returned);
        break;
    case HID_STRING_ID_IPRODUCT:
        status = HidCopyBuffer(Buffer,
                               Length,
                               "PV HID Device",
                               sizeof("PV HID Device"),
                               Returned);
        break;
    //case HID_STRING_ID_ISERIALNUMBER:
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
  
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    return status;
}

static NTSTATUS
HidGetIndexedString(
    IN  PINTERFACE          Interface,
    IN  ULONG               Index,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = STATUS_NOT_SUPPORTED;

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    UNREFERENCED_PARAMETER(Index);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Returned);
    return status;
}

static NTSTATUS
HidGetFeature(
    IN  PINTERFACE          Interface,
    IN  ULONG               ReportId,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = STATUS_NOT_SUPPORTED;

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    UNREFERENCED_PARAMETER(ReportId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Returned);
    return status;
}

static NTSTATUS
HidSetFeature(
    IN  PINTERFACE          Interface,
    IN  ULONG               ReportId,
    IN  PVOID               Buffer,
    IN  ULONG               Length
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = STATUS_NOT_SUPPORTED;

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    UNREFERENCED_PARAMETER(ReportId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    return status;
}

static NTSTATUS
HidGetInputReport(
    IN  PINTERFACE          Interface,
    IN  ULONG               ReportId,
    IN  PVOID               Buffer,
    IN  ULONG               Length,
    OUT PULONG              Returned
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = RingGetInputReport(Context->Ring,
                                ReportId,
                                Buffer,
                                Length,
                                Returned);

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    return status;
}

static NTSTATUS
HidSetOutputReport(
    IN  PINTERFACE          Interface,
    IN  ULONG               ReportId,
    IN  PVOID               Buffer,
    IN  ULONG               Length
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = STATUS_NOT_SUPPORTED;

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    UNREFERENCED_PARAMETER(ReportId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    return status;
}

static VOID
HidReadReport(
    IN  PINTERFACE          Interface
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;

    AcquireMrswLockShared(&Context->Lock);

    if (Context->Enabled)
        RingReadReport(Context->Ring);

    ReleaseMrswLockShared(&Context->Lock);
}

static NTSTATUS
HidWriteReport(
    IN  PINTERFACE          Interface,
    IN  ULONG               ReportId,
    IN  PVOID               Buffer,
    IN  ULONG               Length
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    NTSTATUS                status;

    Trace("=====>\n");
    AcquireMrswLockShared(&Context->Lock);

    status = STATUS_DEVICE_NOT_READY;
    if (!Context->Enabled)
        goto done;

    status = STATUS_NOT_SUPPORTED;

done:
    ReleaseMrswLockShared(&Context->Lock);
    Trace("<=====\n");

    UNREFERENCED_PARAMETER(ReportId);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    return status;
}

static NTSTATUS
HidAcquire(
    PINTERFACE              Interface
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    KIRQL                   Irql;

    AcquireMrswLockExclusive(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    Context->Frontend = PdoGetFrontend(Context->Pdo);
    Context->Ring = FrontendGetRing(Context->Frontend);
    Context->Version = Interface->Version;

    Trace("<====\n");

done:
    ReleaseMrswLockExclusive(&Context->Lock, Irql, FALSE);

    return STATUS_SUCCESS;
}

VOID
HidRelease(
    IN  PINTERFACE          Interface
    )
{
    PXENVKBD_HID_CONTEXT    Context = Interface->Context;
    KIRQL                   Irql;

    AcquireMrswLockExclusive(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    ASSERT(!Context->Enabled);

    Context->Version = 0;
    Context->Frontend = NULL;
    Context->Ring = NULL;

    Trace("<====\n");

done:
    ReleaseMrswLockExclusive(&Context->Lock, Irql, FALSE);
}

static struct _XENHID_HID_INTERFACE_V1 HidInterfaceVersion1 = {
    { sizeof (struct _XENHID_HID_INTERFACE_V1), 1, NULL, NULL, NULL },
    HidAcquire,
    HidRelease,
    HidEnable,
    HidDisable,
    HidGetDeviceAttributes,
    HidGetDeviceDescriptor,
    HidGetReportDescriptor,
    HidGetString,
    HidGetIndexedString,
    HidGetFeature,
    HidSetFeature,
    HidGetInputReport,
    HidSetOutputReport,
    HidReadReport,
    HidWriteReport
};

NTSTATUS
HidInitialize(
    IN  PXENVKBD_PDO            Pdo,
    OUT PXENVKBD_HID_CONTEXT    *Context
    )
{
    NTSTATUS                    status;

    Trace("====>\n");

    *Context = __HidAllocate(sizeof (XENVKBD_HID_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    InitializeMrswLock(&(*Context)->Lock);

    (*Context)->Pdo = Pdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
HidGetInterface(
    IN      PXENVKBD_HID_CONTEXT    Context,
    IN      ULONG                   Version,
    IN OUT  PINTERFACE              Interface,
    IN      ULONG                   Size
    )
{
    NTSTATUS                        status;

    switch (Version) {
    case 1: {
        struct _XENHID_HID_INTERFACE_V1 *HidInterface;

        HidInterface = (struct _XENHID_HID_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENHID_HID_INTERFACE_V1))
            break;

        *HidInterface = HidInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}   

VOID
HidTeardown(
    IN  PXENVKBD_HID_CONTEXT    Context
    )
{
    Trace("====>\n");

    Context->Pdo = NULL;
    Context->Version = 0;

    RtlZeroMemory(&Context->Lock, sizeof (XENVKBD_MRSW_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENVKBD_HID_CONTEXT)));
    __HidFree(Context);

    Trace("<====\n");
}

BOOLEAN
HidSendReadReport(
    IN  PXENVKBD_HID_CONTEXT    Context,
    IN  PVOID                   Buffer,
    IN  ULONG                   Length
    )
{
    if (!Context->Enabled)
        return TRUE; // flag as pending

    // Callback returns TRUE on success, FALSE when Irp could not be completed
    // Invert the result to indicate Pending state
    return !Context->Callback(Context->Argument,
                              Buffer,
                              Length);
}
