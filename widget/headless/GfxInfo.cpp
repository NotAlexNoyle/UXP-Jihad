/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GfxInfo.h"
#include "nsUnicharUtils.h"

namespace mozilla {
namespace widget {

#ifdef DEBUG
NS_IMPL_ISUPPORTS_INHERITED(GfxInfo, GfxInfoBase, nsIGfxInfoDebug)
#endif

// No Direct2D / DirectWrite on a headless unix target.
nsresult GfxInfo::GetD2DEnabled(bool *aEnabled)             { return NS_ERROR_FAILURE; }
nsresult GfxInfo::GetDWriteEnabled(bool *aEnabled)          { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetDWriteVersion(nsAString&)         { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetCleartypeParameters(nsAString&)   { return NS_ERROR_FAILURE; }

// No adapter to describe in a headless build.
NS_IMETHODIMP GfxInfo::GetAdapterDescription(nsAString&)    { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDescription2(nsAString&)   { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriver(nsAString&)         { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriver2(nsAString&)        { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterVendorID(nsAString&)       { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterVendorID2(nsAString&)      { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDeviceID(nsAString&)       { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDeviceID2(nsAString&)      { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterSubsysID(nsAString&)       { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterSubsysID2(nsAString&)      { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterRAM(nsAString&)            { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterRAM2(nsAString&)           { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriverVersion(nsAString&)  { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriverVersion2(nsAString&) { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriverDate(nsAString&)     { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetAdapterDriverDate2(nsAString&)    { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::GetIsGPU2Active(bool*)               { return NS_ERROR_FAILURE; }

const nsTArray<GfxDriverInfo>&
GfxInfo::GetGfxDriverInfo()
{
  // Empty blocklist: nothing to block, we always run software.
  if (!mDriverInfo) {
    mDriverInfo = new nsTArray<GfxDriverInfo>();
  }
  return *mDriverInfo;
}

nsresult
GfxInfo::GetFeatureStatusImpl(int32_t aFeature,
                              int32_t *aStatus,
                              nsAString & aSuggestedDriverVersion,
                              const nsTArray<GfxDriverInfo>& aDriverInfo,
                              nsACString& aFailureId,
                              OperatingSystem* aOS /* = nullptr */)
{
  NS_ENSURE_ARG_POINTER(aStatus);
  *aStatus = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
  aSuggestedDriverVersion.SetIsVoid(true);
  if (aOS) {
    *aOS = OperatingSystem::Linux;
  }
  // Not blocklisted, not accelerated: leave the decision to the caller's
  // software fallback.
  return NS_OK;
}

#ifdef DEBUG

NS_IMETHODIMP GfxInfo::SpoofVendorID(const nsAString&)      { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::SpoofDeviceID(const nsAString&)      { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::SpoofDriverVersion(const nsAString&) { return NS_ERROR_FAILURE; }
NS_IMETHODIMP GfxInfo::SpoofOSVersion(uint32_t)            { return NS_ERROR_FAILURE; }

#endif

} // namespace widget
} // namespace mozilla
