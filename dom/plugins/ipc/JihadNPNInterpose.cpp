/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * NPN entry points exported as GLOBAL SYMBOLS, for plugins that expect the old Netscape
 * convention where the host browser exports them rather than only passing an NPNetscapeFuncs
 * table to NP_Initialize.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT. The obvious place for these is PluginModuleChild.cpp,
 * and it does not work: npapi.h already declares NPN_InvalidateRect, and every header in this
 * build is compiled under `-include config/gcc_hidden.h`, which pushes hidden visibility. GCC
 * takes visibility from the FIRST declaration it sees, so a later definition carrying
 * __attribute__((visibility("default"))) is silently ignored and the symbol lands in the
 * object as GLOBAL HIDDEN — present to `nm`, absent from the dynamic table, and therefore
 * impossible to interpose. Verified with readelf. This file deliberately includes no NPAPI
 * headers at all and describes the functions with void* so there is no prior declaration to
 * inherit hidden visibility from.
 *
 * WHY IT MATTERS. webOS's Flash imports NPN_CreateObject and NPN_InvalidateRect as undefined
 * dynamic symbols. Left alone they bind into libWebKitLuna.so — another browser engine that
 * is in this process because Flash (and our Piranha drawing path) pull it in. Two consequences,
 * both measured:
 *   1. Flash's repaint requests go to LunaSysMgr's WebKit and never reach our engine, so the
 *      plugin is only ever painted when something else happens to invalidate it. That alone
 *      makes an animating plugin look frozen.
 *   2. It is a latent crash: UXP stores a PluginInstanceChild* in NPP::ndata, while
 *      libWebKitLuna's NPN_InvalidateRect casts that same field to a WebCore::PluginView* and
 *      makes a virtual call on it — a wild vtable dispatch on the first repaint.
 *
 * The forwarding pointers are installed by PluginModuleChild once the browser function table
 * exists; until then these are safe no-ops.
 */

#include <stdio.h>

extern "C" {

void  (*gJihadNPNInvalidateRect)(void* aNPP, void* aRect) = 0;
void* (*gJihadNPNCreateObject)(void* aNPP, void* aClass)  = 0;

// Read by the host repaint clock in PluginInstanceChild.cpp: while this is zero the plugin has
// never asked to be repainted, so the host has to drive it.
unsigned gJihadNPNInvalidateRectCalls = 0;

__attribute__((visibility("default"))) void
NPN_InvalidateRect(void* aNPP, void* aRect)
{
    // The first call is the signal that the plugin's frame clock is finally running; without
    // it the plugin is only ever painted by an unrelated invalidation, which is
    // indistinguishable on screen from a plugin that draws nothing. Log it once, and log the
    // no-forwarder case too — arriving before PluginModuleChild installs the pointers is a
    // dropped repaint, not a no-op.
    gJihadNPNInvalidateRectCalls++;
    static int sLogged = 0;
    if (sLogged < 8) {
        sLogged++;
        fprintf(stderr, "[jihad-npapi-child] NPN_InvalidateRect npp=%p rect=%p fwd=%p\n",
                aNPP, aRect, (void*)gJihadNPNInvalidateRect);
        fflush(stderr);
    }
    if (gJihadNPNInvalidateRect) {
        gJihadNPNInvalidateRect(aNPP, aRect);
    }
}

__attribute__((visibility("default"))) void*
NPN_CreateObject(void* aNPP, void* aClass)
{
    return gJihadNPNCreateObject ? gJihadNPNCreateObject(aNPP, aClass) : 0;
}

} // extern "C"
