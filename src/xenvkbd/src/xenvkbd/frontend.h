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

#ifndef _XENVKBD_FRONTEND_H
#define _XENVKBD_FRONTEND_H

#include <ntddk.h>
#include <debug_interface.h>
#include <suspend_interface.h>
#include <evtchn_interface.h>
#include <store_interface.h>
#include <range_set_interface.h>
#include <cache_interface.h>
#include <gnttab_interface.h>
#include <hid_interface.h>

#include "pdo.h"

typedef struct _XENVKBD_FRONTEND XENVKBD_FRONTEND, *PXENVKBD_FRONTEND;

typedef enum _XENVKBD_FRONTEND_STATE {
    FRONTEND_UNKNOWN,
    FRONTEND_CLOSING,
    FRONTEND_CLOSED,
    FRONTEND_PREPARED,
    FRONTEND_CONNECTED,
    FRONTEND_ENABLED
} XENVKBD_FRONTEND_STATE, *PXENVKBD_FRONTEND_STATE;

__drv_requiresIRQL(PASSIVE_LEVEL)
extern NTSTATUS
FrontendInitialize(
    IN  PXENVKBD_PDO        Pdo,
    OUT PXENVKBD_FRONTEND   *Frontend
    );

extern VOID
FrontendTeardown(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern VOID
FrontendEjectFailed(
    IN PXENVKBD_FRONTEND    Frontend
    );

extern NTSTATUS
FrontendSetState(
    IN  PXENVKBD_FRONTEND       Frontend,
    IN  XENVKBD_FRONTEND_STATE  State
    );

extern NTSTATUS
FrontendResume(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern VOID
FrontendSuspend(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern PXENVKBD_PDO
FrontendGetPdo(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetPath(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetBackendPath(
    IN  PXENVKBD_FRONTEND   Frontend
    );

extern USHORT
FrontendGetBackendDomain(
    IN  PXENVKBD_FRONTEND   Frontend
    );

#include "ring.h"

extern PXENVKBD_RING
FrontendGetRing(
    IN  PXENVKBD_FRONTEND   Frontend
    );

#endif  // _XENVKBD_FRONTEND_H
