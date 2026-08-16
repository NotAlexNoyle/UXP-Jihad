/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsLookAndFeel.h"

nsLookAndFeel::nsLookAndFeel() : nsXPLookAndFeel()
{
}

nsLookAndFeel::~nsLookAndFeel()
{
}

nsresult
nsLookAndFeel::NativeGetColor(ColorID aID, nscolor& aResult)
{
  // No native widget theme in headless mode. Use the built-in standin
  // palette that nsXPLookAndFeel maintains for exactly this purpose.
  aResult = GetStandinForNativeColor(aID);
  return NS_OK;
}

bool
nsLookAndFeel::GetFontImpl(FontID aID, nsString& aFontName,
                           gfxFontStyle& aFontStyle,
                           float aDevPixPerCSSPixel)
{
  // No system font database in headless mode. Report "no native font" so
  // callers fall back to their default font selection.
  return false;
}
