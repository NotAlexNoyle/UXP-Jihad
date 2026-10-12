/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppShell.h"
#include "nsThreadUtils.h"

using mozilla::NewRunnableMethod;

// There is no native event source in the headless backend. XPCOM events are
// processed via the XPCOM thread event queue (NS_ProcessNextEvent), so we wake
// the base app shell by posting a runnable that fires its NativeEventCallback.
void
nsAppShell::ScheduleNativeEventCallback()
{
  nsCOMPtr<nsIRunnable> event =
    NewRunnableMethod(this, &nsAppShell::NativeEventCallback);
  NS_DispatchToCurrentThread(event.forget());
}

bool
nsAppShell::ProcessNextNativeEvent(bool aMayWait)
{
  // No native events in a headless process.
  return false;
}
