/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Jihad: minimal headless widget factory. Registers the app shell, a fake
// screen manager (PuppetScreenManager), the shared transferable/format
// converter, and a stub GfxInfo (required: gfxPlatform::InitAcceleration()
// dereferences services::GetGfxInfo() at engine startup). Other widget
// components (theme, clipboard, idle, …) are still TODO.

#include "mozilla/ModuleUtils.h"
#include "nsWidgetsCID.h"

#include "nsAppShell.h"
#include "PuppetWidget.h"          // PuppetScreenManager
#include "nsTransferable.h"
#include "nsHTMLFormatConverter.h"
#include "GfxInfo.h"

using namespace mozilla;
using namespace mozilla::widget;

NS_GENERIC_FACTORY_CONSTRUCTOR(nsAppShell)
NS_GENERIC_FACTORY_CONSTRUCTOR(PuppetScreenManager)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsTransferable)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsHTMLFormatConverter)

namespace mozilla {
namespace widget {
NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(GfxInfo, Init)
}
}

NS_DEFINE_NAMED_CID(NS_APPSHELL_CID);
NS_DEFINE_NAMED_CID(NS_SCREENMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_TRANSFERABLE_CID);
NS_DEFINE_NAMED_CID(NS_HTMLFORMATCONVERTER_CID);
NS_DEFINE_NAMED_CID(NS_GFXINFO_CID);

static const mozilla::Module::CIDEntry kWidgetCIDs[] = {
    { &kNS_APPSHELL_CID, false, nullptr, nsAppShellConstructor, Module::ALLOW_IN_GPU_PROCESS },
    { &kNS_SCREENMANAGER_CID, false, nullptr, PuppetScreenManagerConstructor, Module::MAIN_PROCESS_ONLY },
    { &kNS_TRANSFERABLE_CID, false, nullptr, nsTransferableConstructor },
    { &kNS_HTMLFORMATCONVERTER_CID, false, nullptr, nsHTMLFormatConverterConstructor },
    { &kNS_GFXINFO_CID, false, nullptr, mozilla::widget::GfxInfoConstructor },
    { nullptr }
};

static const mozilla::Module::ContractIDEntry kWidgetContracts[] = {
    { "@mozilla.org/widget/appshell/headless;1", &kNS_APPSHELL_CID },
    { "@mozilla.org/gfx/screenmanager;1", &kNS_SCREENMANAGER_CID },
    { "@mozilla.org/widget/transferable;1", &kNS_TRANSFERABLE_CID },
    { "@mozilla.org/widget/htmlformatconverter;1", &kNS_HTMLFORMATCONVERTER_CID },
    { "@mozilla.org/gfx/info;1", &kNS_GFXINFO_CID },
    { nullptr }
};

static const mozilla::Module kWidgetModule = {
    mozilla::Module::kVersion,
    kWidgetCIDs,
    kWidgetContracts
};

NSMODULE_DEFN(nsWidgetHeadlessModule) = &kWidgetModule;
