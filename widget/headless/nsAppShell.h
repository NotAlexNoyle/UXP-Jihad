/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Jihad: headless app shell — no native (GTK/X) event loop. XPCOM events are
// driven by the embedder pumping NS_ProcessNextEvent; there are no native
// events to process.
#ifndef nsAppShell_h__
#define nsAppShell_h__

#include "nsBaseAppShell.h"

class nsAppShell : public nsBaseAppShell
{
public:
  nsAppShell() {}
  nsresult Init() { return nsBaseAppShell::Init(); }

  virtual void ScheduleNativeEventCallback() override;
  virtual bool ProcessNextNativeEvent(bool aMayWait) override;

protected:
  virtual ~nsAppShell() {}
};

#endif // nsAppShell_h__
