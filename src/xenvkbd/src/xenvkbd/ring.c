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
 *     following disclaimer in the documetation and/or other
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
#include <procgrp.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include <debug_interface.h>
#include <store_interface.h>
#include <cache_interface.h>
#include <gnttab_interface.h>
#include <range_set_interface.h>
#include <evtchn_interface.h>

#include "pdo.h"
#include "frontend.h"
#include "ring.h"
#include "hid.h"
#include "vkbd.h"
#include "thread.h"
#include "registry.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define MAXNAMELEN  128

struct _XENVKBD_RING {
    PXENVKBD_FRONTEND       Frontend;
    PXENVKBD_HID_CONTEXT    Hid;

    XENBUS_DEBUG_INTERFACE  DebugInterface;
    XENBUS_STORE_INTERFACE  StoreInterface;
    XENBUS_GNTTAB_INTERFACE GnttabInterface;
    XENBUS_EVTCHN_INTERFACE EvtchnInterface;

    PXENBUS_DEBUG_CALLBACK  DebugCallback;

    PXENBUS_GNTTAB_CACHE    GnttabCache;
    PMDL                    Mdl;
    struct xenkbd_page      *Shared;
    PXENBUS_GNTTAB_ENTRY    Entry;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    KSPIN_LOCK              Lock;
    KDPC                    Dpc;
    ULONG                   Dpcs;
    ULONG                   Events;
    BOOLEAN                 Connected;
    BOOLEAN                 Enabled;
    BOOLEAN                 AbsPointer;
    BOOLEAN                 RawPointer;

    XENVKBD_HID_KEYBOARD    KeyboardReport;
    XENVKBD_HID_ABSMOUSE    AbsMouseReport;
    BOOLEAN                 KeyboardPending;
    BOOLEAN                 AbsMousePending;
};

#define XENVKBD_RING_TAG    'gniR'

static FORCEINLINE PVOID
__RingAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENVKBD_RING_TAG);
}

static FORCEINLINE VOID
__RingFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XENVKBD_RING_TAG);
}

static FORCEINLINE NTSTATUS
__RingCopyBuffer(
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

static FORCEINLINE LONG
Constrain(
    IN  LONG    Value,
    IN  LONG    Min,
    IN  LONG    Max
    )
{
    if (Value < Min)
        return Min;
    if (Value > Max)
        return Max;
    return Value;
}

static FORCEINLINE UCHAR
SetBit(
    IN  UCHAR   Value,
    IN  UCHAR   BitIdx,
    IN  BOOLEAN Pressed
    )
{
    if (Pressed) {
        return Value | (1 << BitIdx);
    } else {
        return Value & ~(1 << BitIdx);
    }
}

static FORCEINLINE VOID
SetArray(
    IN  PUCHAR  Array,
    IN  ULONG   Size,
    IN  UCHAR   Value,
    IN  BOOLEAN Pressed
    )
{
    ULONG       Idx;
    if (Pressed) {
        for (Idx = 0; Idx < Size; ++Idx) {
            if (Array[Idx] == Value)
                break;
            if (Array[Idx] != 0)
                continue;
            Array[Idx] = Value;
            break;
        }
    } else {
        for (Idx = 0; Idx < Size; ++Idx) {
            if (Array[Idx] == 0)
                break;
            if (Array[Idx] != Value)
                continue;
            for (; Idx < Size - 1; ++Idx)
                Array[Idx] = Array[Idx + 1];
            Array[Size - 1] = 0;
            break;
        }
    }
}

static FORCEINLINE USHORT
KeyCodeToUsage(
    IN  ULONG   KeyCode
    )
{
    if (KeyCode < sizeof(VkbdKeyCodeToUsage)/sizeof(VkbdKeyCodeToUsage[0]))
        return VkbdKeyCodeToUsage[KeyCode];
    return 0;
}

static FORCEINLINE VOID
__RingEventMotion(
    IN  PXENVKBD_RING   Ring,
    IN  LONG            dX,
    IN  LONG            dY,
    IN  LONG            dZ
    )
{
    Ring->AbsMouseReport.X = (USHORT)Constrain(Ring->AbsMouseReport.X + dX, 0, 32767);
    Ring->AbsMouseReport.Y = (USHORT)Constrain(Ring->AbsMouseReport.Y + dY, 0, 32767);
    Ring->AbsMouseReport.dZ = -(CHAR)Constrain(dZ, -127, 127);

    Ring->AbsMousePending = HidSendReadReport(Ring->Hid,
                                              &Ring->AbsMouseReport,
                                              sizeof(XENVKBD_HID_ABSMOUSE));
}

static FORCEINLINE VOID
__RingEventKeypress(
    IN  PXENVKBD_RING   Ring,
    IN  ULONG           KeyCode,
    IN  BOOLEAN         Pressed
    )
{
    if (KeyCode >= 0x110 && KeyCode <= 0x114) {
        // Mouse Buttons
        Ring->AbsMouseReport.Buttons = SetBit(Ring->AbsMouseReport.Buttons,
                                              (UCHAR)(KeyCode - 0x110),
                                              Pressed);

        Ring->AbsMousePending = HidSendReadReport(Ring->Hid,
                                                  &Ring->AbsMouseReport,
                                                  sizeof(XENVKBD_HID_ABSMOUSE));

    } else {
        // map KeyCode to Usage
        USHORT  Usage = KeyCodeToUsage(KeyCode);
        if (Usage == 0)
            return; // non-standard key

        if (Usage >= 0xE0 && Usage <= 0xE7) {
            // Modifier
            Ring->KeyboardReport.Modifiers = SetBit(Ring->KeyboardReport.Modifiers,
                                                    (UCHAR)(Usage - 0xE0),
                                                    Pressed);
        } else {
            // Standard Key
            SetArray(Ring->KeyboardReport.Keys,
                     6,
                     (UCHAR)Usage,
                     Pressed);
        }
        Ring->KeyboardPending = HidSendReadReport(Ring->Hid,
                                                  &Ring->KeyboardReport,
                                                  sizeof(XENVKBD_HID_KEYBOARD));

    }
}

static FORCEINLINE VOID
__RingEventPosition(
    IN  PXENVKBD_RING   Ring,
    IN  ULONG           X,
    IN  ULONG           Y,
    IN  LONG            dZ
    )
{
    Ring->AbsMouseReport.X = (USHORT)Constrain(X, 0, 32767);
    Ring->AbsMouseReport.Y = (USHORT)Constrain(Y, 0, 32767);
    Ring->AbsMouseReport.dZ = -(CHAR)Constrain(dZ, -127, 127);

    Ring->AbsMousePending = HidSendReadReport(Ring->Hid,
                                              &Ring->AbsMouseReport,
                                              sizeof(XENVKBD_HID_ABSMOUSE));
}

__drv_functionClass(KDEFERRED_ROUTINE)
__drv_maxIRQL(DISPATCH_LEVEL)
__drv_minIRQL(DISPATCH_LEVEL)
__drv_requiresIRQL(DISPATCH_LEVEL)
__drv_sameIRQL
static VOID
RingDpc(
    IN  PKDPC       Dpc,
    IN  PVOID       Context,
    IN  PVOID       Argument1,
    IN  PVOID       Argument2
    )
{
    PXENVKBD_RING   Ring = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Ring != NULL);

    for (;;) {
        ULONG   in_cons;
        ULONG   in_prod;

        KeMemoryBarrier();

        in_cons = Ring->Shared->in_cons;
        in_prod = Ring->Shared->in_prod;

        KeMemoryBarrier();

        if (in_cons == in_prod)
            break;

        while (in_cons != in_prod) {
            union xenkbd_in_event *in_evt;

            in_evt = &XENKBD_IN_RING_REF(Ring->Shared, in_cons);
            ++in_cons;

            switch (in_evt->type) {
            case XENKBD_TYPE_MOTION:
                __RingEventMotion(Ring,
                                  in_evt->motion.rel_x,
                                  in_evt->motion.rel_y,
                                  in_evt->motion.rel_z);
                break;
            case XENKBD_TYPE_KEY:
                __RingEventKeypress(Ring,
                                    in_evt->key.keycode,
                                    in_evt->key.pressed);
                break;
            case XENKBD_TYPE_POS:
                __RingEventPosition(Ring,
                                    in_evt->pos.abs_x,
                                    in_evt->pos.abs_y,
                                    in_evt->pos.rel_z);
                break;
            case XENKBD_TYPE_MTOUCH:
                Trace("MTOUCH: %u %u %u %u\n",
                     in_evt->mtouch.event_type,
                     in_evt->mtouch.contact_id,
                     in_evt->mtouch.u.pos.abs_x,
                     in_evt->mtouch.u.pos.abs_y);
                // call Frontend
                break;
            default:
                Trace("UNKNOWN: %u\n",
                      in_evt->type);
                break;
            }
        }

        KeMemoryBarrier();

        Ring->Shared->in_cons = in_cons;
    }

    XENBUS_EVTCHN(Unmask,
                  &Ring->EvtchnInterface,
                  Ring->Channel,
                  FALSE);
}

static VOID
RingAcquireLock(
    IN  PVOID       Context
    )
{
    PXENVKBD_RING   Ring = Context;
    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
}

static VOID
RingReleaseLock(
    IN  PVOID       Context
    )
{
    PXENVKBD_RING   Ring = Context;
#pragma warning(suppress:26110)
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);
}

KSERVICE_ROUTINE    RingEvtchnCallback;

BOOLEAN
RingEvtchnCallback(
    IN  PKINTERRUPT     InterruptObject,
    IN  PVOID           Argument
    )
{
    PXENVKBD_RING       Ring = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Ring != NULL);
    Ring->Events++;

    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;

    return TRUE;
}

static VOID
RingDebugCallback(
    IN  PVOID           Argument,
    IN  BOOLEAN         Crashing
    )
{
    PXENVKBD_RING       Ring = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "0x%p [%s]\n",
                 Ring,
                 (Ring->Enabled) ? "ENABLED" : "DISABLED");

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "KBD: %02x %02x %02x %02x %02x %02x %02x %02x%s\n",
                 Ring->KeyboardReport.ReportId,
                 Ring->KeyboardReport.Modifiers,
                 Ring->KeyboardReport.Keys[0],
                 Ring->KeyboardReport.Keys[1],
                 Ring->KeyboardReport.Keys[2],
                 Ring->KeyboardReport.Keys[3],
                 Ring->KeyboardReport.Keys[4],
                 Ring->KeyboardReport.Keys[5],
                 Ring->KeyboardPending ? " PENDING" : "");

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "MOU: %02x %02x %04x %04x %02x%s\n",
                 Ring->AbsMouseReport.ReportId,
                 Ring->AbsMouseReport.Buttons,
                 Ring->AbsMouseReport.X,
                 Ring->AbsMouseReport.Y,
                 Ring->AbsMouseReport.dZ,
                 Ring->AbsMousePending ? " PENDING" : "");
}

NTSTATUS
RingInitialize(
    IN  PXENVKBD_FRONTEND   Frontend,
    OUT PXENVKBD_RING       *Ring
    )
{
    NTSTATUS                status;

    Trace("=====>\n");
    status = STATUS_NO_MEMORY;
    *Ring = __RingAllocate(sizeof(XENVKBD_RING));
    if (*Ring == NULL)
        goto fail1;

    (*Ring)->Frontend = Frontend;
    (*Ring)->Hid = PdoGetHidContext(FrontendGetPdo(Frontend));
    KeInitializeDpc(&(*Ring)->Dpc, RingDpc, *Ring);
    KeInitializeSpinLock(&(*Ring)->Lock);

    FdoGetDebugInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->DebugInterface);

    FdoGetStoreInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->StoreInterface);

    FdoGetGnttabInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->GnttabInterface);

    FdoGetEvtchnInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->EvtchnInterface);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static FORCEINLINE VOID
RingReadFeatures(
    IN  PXENVKBD_RING   Ring
    )
{
    PCHAR               Buffer;
    NTSTATUS            status;

    status = XENBUS_STORE(Read,
                          &Ring->StoreInterface,
                          NULL,
                          FrontendGetBackendPath(Ring->Frontend),
                          "feature-abs-pointer",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Ring->AbsPointer = (BOOLEAN)strtoul(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Ring->StoreInterface,
                     Buffer);
    } else {
        Ring->AbsPointer = FALSE;
    }

    status = XENBUS_STORE(Read,
                          &Ring->StoreInterface,
                          NULL,
                          FrontendGetBackendPath(Ring->Frontend),
                          "feature-raw-pointer",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Ring->RawPointer = (BOOLEAN)strtoul(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Ring->StoreInterface,
                     Buffer);
    } else {
        Ring->RawPointer = FALSE;
    }
}

NTSTATUS
RingConnect(
    IN  PXENVKBD_RING   Ring
    )
{
    PFN_NUMBER          Pfn;
    PXENVKBD_FRONTEND   Frontend;
    NTSTATUS            status;

    Trace("=====>\n");
    Frontend = Ring->Frontend;

    status = XENBUS_DEBUG(Acquire, &Ring->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_STORE(Acquire, &Ring->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_EVTCHN(Acquire, &Ring->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_GNTTAB(Acquire, &Ring->GnttabInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_GNTTAB(CreateCache,
                           &Ring->GnttabInterface,
                           "VKBD_Ring_Gnttab",
                           0,
                           RingAcquireLock,
                           RingReleaseLock,
                           Ring,
                           &Ring->GnttabCache);
    if (!NT_SUCCESS(status))
        goto fail5;

    Ring->KeyboardReport.ReportId = 1;
    Ring->AbsMouseReport.ReportId = 2;
    RingReadFeatures(Ring);

    status = STATUS_DEVICE_NOT_READY;
    if (!Ring->RawPointer)
        goto fail6;

    Ring->Mdl = __AllocatePage();
    
    status = STATUS_NO_MEMORY;
    if (Ring->Mdl == NULL)
        goto fail7;

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    Ring->Shared = Ring->Mdl->MappedSystemVa;
    ASSERT(Ring->Shared != NULL);

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    RtlZeroMemory(Ring->Mdl->MappedSystemVa, PAGE_SIZE);

    Pfn = MmGetMdlPfnArray(Ring->Mdl)[0];

    status = XENBUS_GNTTAB(PermitForeignAccess,
                           &Ring->GnttabInterface,
                           Ring->GnttabCache,
                           TRUE,
                           FrontendGetBackendDomain(Frontend),
                           Pfn,
                           FALSE,
                           &Ring->Entry);
    if (!NT_SUCCESS(status))
        goto fail8;

    Ring->Channel = XENBUS_EVTCHN(Open,
                                  &Ring->EvtchnInterface,
                                  XENBUS_EVTCHN_TYPE_UNBOUND,
                                  RingEvtchnCallback,
                                  Ring,
                                  FrontendGetBackendDomain(Frontend),
                                  TRUE);

    status = STATUS_UNSUCCESSFUL;
    if (Ring->Channel == NULL)
        goto fail9;

    XENBUS_EVTCHN(Unmask,
                  &Ring->EvtchnInterface,
                  Ring->Channel,
                  FALSE);

    status = XENBUS_DEBUG(Register,
                          &Ring->DebugInterface,
                          __MODULE__ "|RING",
                          RingDebugCallback,
                          Ring,
                          &Ring->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail10;

    Ring->Connected = TRUE;
    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    Ring->Events = 0;

fail9:
    Error("fail9\n");

    (VOID) XENBUS_GNTTAB(RevokeForeignAccess,
                         &Ring->GnttabInterface,
                         Ring->GnttabCache,
                         TRUE,
                         Ring->Entry);
    Ring->Entry = NULL;

fail8:
    Error("fail8\n");

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

fail7:
    Error("fail7\n");

fail6:
    Error("fail6\n");

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

fail5:
    Error("fail5\n");

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

fail4:
    Error("fail4\n");

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

fail3:
    Error("fail3\n");

    XENBUS_STORE(Release, &Ring->StoreInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

fail1:
    Error("fail1 %08x\n", status);

    return status;
}

NTSTATUS
RingStoreWrite(
    IN  PXENVKBD_RING               Ring,
    IN  PXENBUS_STORE_TRANSACTION   Transaction
    )
{
    ULONG                           Port;
    NTSTATUS                        status;

    Trace("=====>\n");
    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "page-gref",
                          "%u",
                          XENBUS_GNTTAB(GetReference,
                                        &Ring->GnttabInterface,
                                        Ring->Entry));
    if (!NT_SUCCESS(status))
        goto fail1;

    // this should not be required - QEMU should use grant references
    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "page-ref",
                          "%llu",
                          (ULONG64)MmGetMdlPfnArray(Ring->Mdl)[0]);
    if (!NT_SUCCESS(status))
        goto fail2;

    Port = XENBUS_EVTCHN(GetPort,
                         &Ring->EvtchnInterface,
                         Ring->Channel);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "event-channel",
                          "%u",
                          Port);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "request-abs-pointer",
                          "%u",
                          Ring->AbsPointer);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "request-raw-pointer",
                          "%u",
                          Ring->RawPointer);
    if (!NT_SUCCESS(status))
        goto fail5;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");
fail4:
    Error("fail4\n");
fail3:
    Error("fail3\n");
fail2:
    Error("fail2\n");
fail1:
    Error("fail1 %08x\n", status);
    return status;
}

NTSTATUS
RingEnable(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    ASSERT(!Ring->Enabled);
    Ring->Enabled = TRUE;

    KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);

    Trace("<=====\n");
    return STATUS_SUCCESS;
}

VOID
RingDisable(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    ASSERT(Ring->Enabled);
    Ring->Enabled = FALSE;

    Trace("<=====\n");
}

VOID
RingDisconnect(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");

    XENBUS_DEBUG(Deregister,
                 &Ring->DebugInterface,
                 Ring->DebugCallback);
    Ring->DebugCallback = NULL;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    Ring->Events = 0;

    (VOID) XENBUS_GNTTAB(RevokeForeignAccess,
                         &Ring->GnttabInterface,
                         Ring->GnttabCache,
                         TRUE,
                         Ring->Entry);
    Ring->Entry = NULL;

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

    RtlZeroMemory(&Ring->KeyboardReport,
                  sizeof(XENVKBD_HID_KEYBOARD));
    RtlZeroMemory(&Ring->AbsMouseReport,
                  sizeof(XENVKBD_HID_ABSMOUSE));
    Ring->KeyboardPending = FALSE;
    Ring->AbsMousePending = FALSE;

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);
    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);
    XENBUS_STORE(Release, &Ring->StoreInterface);
    XENBUS_DEBUG(Release, &Ring->DebugInterface);

    Trace("<=====\n");
}

VOID
RingTeardown(
    IN  PXENVKBD_RING   Ring
    )
{
    Trace("=====>\n");
    Ring->Dpcs = 0;

    Ring->AbsPointer = FALSE;
    Ring->RawPointer = FALSE;

    RtlZeroMemory(&Ring->Dpc, sizeof (KDPC));

    RtlZeroMemory(&Ring->Lock,
                  sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Ring->GnttabInterface,
                  sizeof (XENBUS_GNTTAB_INTERFACE));

    RtlZeroMemory(&Ring->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Ring->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Ring->EvtchnInterface,
                  sizeof (XENBUS_EVTCHN_INTERFACE));

    Ring->Frontend = NULL;
    Ring->Hid = NULL;

    ASSERT(IsZeroMemory(Ring, sizeof (XENVKBD_RING)));
    __RingFree(Ring);
    Trace("<=====\n");
}

VOID
RingNotify(
    IN  PXENVKBD_RING   Ring
    )
{
    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;
}

NTSTATUS
RingGetInputReport(
    IN  PXENVKBD_RING   Ring,
    IN  ULONG           ReportId,
    IN  PVOID           Buffer,
    IN  ULONG           Length,
    OUT PULONG          Returned
    )
{
    switch (ReportId) {
    case 1:
        return __RingCopyBuffer(Buffer,
                                Length,
                                &Ring->KeyboardReport,
                                sizeof(XENVKBD_HID_KEYBOARD),
                                Returned);
    case 2:
        return __RingCopyBuffer(Buffer,
                                Length,
                                &Ring->AbsMouseReport,
                                sizeof(XENVKBD_HID_ABSMOUSE),
                                Returned);
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

VOID
RingReadReport(
    IN  PXENVKBD_RING   Ring
    )
{
    // Check for pending reports, push 1 pending report to subscriber
    if (Ring->KeyboardPending)
        Ring->KeyboardPending = HidSendReadReport(Ring->Hid,
                                                  &Ring->KeyboardReport,
                                                  sizeof(XENVKBD_HID_KEYBOARD));
    else if (Ring->AbsMousePending)
        Ring->AbsMousePending = HidSendReadReport(Ring->Hid,
                                                  &Ring->AbsMouseReport,
                                                  sizeof(XENVKBD_HID_ABSMOUSE));
}
