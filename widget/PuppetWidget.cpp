/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

#include "ClientLayerManager.h"
#include "gfxPlatform.h"
#include "mozilla/dom/TabChild.h"
#include "mozilla/Hal.h"
#include "mozilla/IMEStateManager.h"
#include "mozilla/layers/APZChild.h"
#include "mozilla/layers/PLayerTransactionChild.h"
#include "mozilla/Preferences.h"
#include "mozilla/TextComposition.h"
#include "mozilla/TextEvents.h"
#include "mozilla/Unused.h"
#include "PuppetWidget.h"
#include "nsContentUtils.h"
#include "nsIWidgetListener.h"
#include "imgIContainer.h"
#include "nsView.h"
// Jihad offscreen render-document support:
#include "gfxContext.h"
#include "nsIDocShell.h"
#include "nsIPresShell.h"
#include "nsPresContext.h"
#include "nsServiceManagerUtils.h"   // do_GetService (jihad_init_nss)
#include "nsThreadUtils.h"           // NS_IsMainThread
// Jihad popup overlay composite (a popup is a separate display root — see
// jihad_offscreen_composite_popups at the bottom of this file):
#include "nsXULPopupManager.h"
#include "nsLayoutUtils.h"
#include "nsRenderingContext.h"
#include "nsMenuPopupFrame.h"
#include "nsMenuFrame.h"
#include "nsGkAtoms.h"
#include "nsIFrame.h"
#include "nsDisplayList.h"       // nsDisplayListBuilderMode

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::hal;
using namespace mozilla::gfx;
using namespace mozilla::layers;
using namespace mozilla::widget;

static void
InvalidateRegion(nsIWidget* aWidget, const LayoutDeviceIntRegion& aRegion)
{
  for (auto iter = aRegion.RectIter(); !iter.Done(); iter.Next()) {
    aWidget->Invalidate(iter.Get());
  }
}

/*static*/ already_AddRefed<nsIWidget>
nsIWidget::CreatePuppetWidget(TabChild* aTabChild)
{
  MOZ_ASSERT(!aTabChild || nsIWidget::UsePuppetWidgets(),
             "PuppetWidgets not allowed in this configuration");

  nsCOMPtr<nsIWidget> widget = new PuppetWidget(aTabChild);
  return widget.forget();
}

namespace mozilla {
namespace widget {

static bool
IsPopup(const nsWidgetInitData* aInitData)
{
  return aInitData && aInitData->mWindowType == eWindowType_popup;
}

static bool
MightNeedIMEFocus(const nsWidgetInitData* aInitData)
{
  // In the puppet-widget world, popup widgets are just dummies and
  // shouldn't try to mess with IME state.
#ifdef MOZ_CROSS_PROCESS_IME
  return !IsPopup(aInitData);
#else
  return false;
#endif
}

// Arbitrary, fungible.
const size_t PuppetWidget::kMaxDimension = 4000;

NS_IMPL_ISUPPORTS_INHERITED0(PuppetWidget, nsBaseWidget)

PuppetWidget::PuppetWidget(TabChild* aTabChild)
  : mTabChild(aTabChild)
  , mMemoryPressureObserver(nullptr)
  , mDPI(-1)
  , mRounding(-1)
  , mDefaultScale(-1)
  , mCursorHotspotX(0)
  , mCursorHotspotY(0)
  , mNativeKeyCommandsValid(false)
{
  MOZ_COUNT_CTOR(PuppetWidget);

  mSingleLineCommands.SetCapacity(4);
  mMultiLineCommands.SetCapacity(4);
  mRichTextCommands.SetCapacity(4);

  // Setting 'Unknown' means "not yet cached".
  mInputContext.mIMEState.mEnabled = IMEState::UNKNOWN;
}

PuppetWidget::~PuppetWidget()
{
  MOZ_COUNT_DTOR(PuppetWidget);

  Destroy();
}

void
PuppetWidget::InfallibleCreate(nsIWidget* aParent,
                               nsNativeWidget aNativeParent,
                               const LayoutDeviceIntRect& aRect,
                               nsWidgetInitData* aInitData)
{
  MOZ_ASSERT(!aNativeParent, "got a non-Puppet native parent");

  BaseCreate(nullptr, aInitData);

  mBounds = aRect;
  mEnabled = true;
  mVisible = true;

  // JIHAD popup instrumentation (menupopup investigation 2026-08-02): today popup
  // widget creation logs NOTHING, so "no popup created" and "popup created but
  // painted nowhere" are indistinguishable — exactly the ambiguity that burned the
  // holdAt and icon investigations. One line per widget creation, offscreen only.
  if (getenv("JIHAD_OFFSCREEN")) {
    fprintf(stderr, "[jihad-widget] create type=%d popup=%d hint=%d parent=%p bounds=%d,%d %dx%d\n",
            aInitData ? (int)aInitData->mWindowType : -1,
            (int)IsPopup(aInitData),
            aInitData ? (int)aInitData->mPopupHint : -1,
            (void*)aParent, aRect.x, aRect.y, aRect.width, aRect.height);
  }

  mDrawTarget = gfxPlatform::GetPlatform()->
    CreateOffscreenContentDrawTarget(IntSize(1, 1), SurfaceFormat::B8G8R8A8);

  mNeedIMEStateInit = MightNeedIMEFocus(aInitData);

  PuppetWidget* parent = static_cast<PuppetWidget*>(aParent);
  if (parent) {
    parent->SetChild(this);
    mLayerManager = parent->GetLayerManager();
  }
  else {
    Resize(mBounds.x, mBounds.y, mBounds.width, mBounds.height, false);
  }
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    mMemoryPressureObserver = new MemoryPressureObserver(this);
    obs->AddObserver(mMemoryPressureObserver, "memory-pressure", false);
  }
}

nsresult
PuppetWidget::Create(nsIWidget* aParent,
                     nsNativeWidget aNativeParent,
                     const LayoutDeviceIntRect& aRect,
                     nsWidgetInitData* aInitData)
{
  InfallibleCreate(aParent, aNativeParent, aRect, aInitData);
  return NS_OK;
}

void
PuppetWidget::InitIMEState()
{
  MOZ_ASSERT(mTabChild);
  if (mNeedIMEStateInit) {
    mContentCache.Clear();
    mTabChild->SendUpdateContentCache(mContentCache);
    mIMEPreferenceOfParent = nsIMEUpdatePreference();
    mNeedIMEStateInit = false;
  }
}

already_AddRefed<nsIWidget>
PuppetWidget::CreateChild(const LayoutDeviceIntRect& aRect,
                          nsWidgetInitData* aInitData,
                          bool aForceUseIWidgetParent)
{
  bool isPopup = IsPopup(aInitData);
  nsCOMPtr<nsIWidget> widget = nsIWidget::CreatePuppetWidget(mTabChild);
  return ((widget &&
           NS_SUCCEEDED(widget->Create(isPopup ? nullptr: this, nullptr, aRect,
                                       aInitData))) ?
          widget.forget() : nullptr);
}

void
PuppetWidget::Destroy()
{
  if (mOnDestroyCalled) {
    return;
  }
  mOnDestroyCalled = true;

  Base::OnDestroy();
  Base::Destroy();
  mPaintTask.Revoke();
  if (mMemoryPressureObserver) {
    mMemoryPressureObserver->Remove();
  }
  mMemoryPressureObserver = nullptr;
  mChild = nullptr;
  if (mLayerManager) {
    mLayerManager->Destroy();
  }
  mLayerManager = nullptr;
  mTabChild = nullptr;
}

NS_IMETHODIMP
PuppetWidget::Show(bool aState)
{
  NS_ASSERTION(mEnabled,
               "does it make sense to Show()/Hide() a disabled widget?");

  bool wasVisible = mVisible;
  mVisible = aState;

  // JIHAD popup instrumentation: a popup that opens/closes flips its widget's
  // visibility — the cheapest reliable "did the menu actually open" signal.
  if (mWindowType == eWindowType_popup && getenv("JIHAD_OFFSCREEN")) {
    fprintf(stderr, "[jihad-widget] popup show=%d widget=%p bounds=%d,%d %dx%d\n",
            (int)aState, (void*)this, mBounds.x, mBounds.y, mBounds.width, mBounds.height);
  }

  if (mChild) {
    mChild->mVisible = aState;
  }

  if (!wasVisible && mVisible) {
    // The previously attached widget listener is handy if
    // we're transitioning from page to page without dropping
    // layers (since we'll continue to show the old layers
    // associated with that old widget listener). If the
    // PuppetWidget was hidden, those layers are dropped,
    // so the previously attached widget listener is really
    // of no use anymore (and is actually actively harmful - see
    // bug 1323586).
    mPreviouslyAttachedWidgetListener = nullptr;
    Resize(mBounds.width, mBounds.height, false);
    Invalidate(mBounds);
  }

  return NS_OK;
}

NS_IMETHODIMP
PuppetWidget::Resize(double aWidth,
                     double aHeight,
                     bool   aRepaint)
{
  LayoutDeviceIntRect oldBounds = mBounds;
  mBounds.SizeTo(LayoutDeviceIntSize(NSToIntRound(aWidth),
                                     NSToIntRound(aHeight)));

  if (JihadOffscreen()) {
    JihadEnsureDrawTarget();
  }

  if (mChild) {
    return mChild->Resize(aWidth, aHeight, aRepaint);
  }

  // XXX: roc says that |aRepaint| dictates whether or not to
  // invalidate the expanded area
  if (oldBounds.Size() < mBounds.Size() && aRepaint) {
    LayoutDeviceIntRegion dirty(mBounds);
    dirty.Sub(dirty, oldBounds);
    InvalidateRegion(this, dirty);
  }

  // call WindowResized() on both the current listener, and possibly
  // also the previous one if we're in a state where we're drawing that one
  // because the current one is paint suppressed
  if (!oldBounds.IsEqualEdges(mBounds) && mAttachedWidgetListener) {
    if (GetCurrentWidgetListener() &&
        GetCurrentWidgetListener() != mAttachedWidgetListener) {
      GetCurrentWidgetListener()->WindowResized(this, mBounds.width, mBounds.height);
    }
    mAttachedWidgetListener->WindowResized(this, mBounds.width, mBounds.height);
  }

  return NS_OK;
}

nsresult
PuppetWidget::ConfigureChildren(const nsTArray<Configuration>& aConfigurations)
{
  for (uint32_t i = 0; i < aConfigurations.Length(); ++i) {
    const Configuration& configuration = aConfigurations[i];
    PuppetWidget* w = static_cast<PuppetWidget*>(configuration.mChild.get());
    NS_ASSERTION(w->GetParent() == this,
                 "Configured widget is not a child");
    w->SetWindowClipRegion(configuration.mClipRegion, true);
    LayoutDeviceIntRect bounds = w->GetBounds();
    if (bounds.Size() != configuration.mBounds.Size()) {
      w->Resize(configuration.mBounds.x, configuration.mBounds.y,
                configuration.mBounds.width, configuration.mBounds.height,
                true);
    } else if (bounds.TopLeft() != configuration.mBounds.TopLeft()) {
      w->Move(configuration.mBounds.x, configuration.mBounds.y);
    }
    w->SetWindowClipRegion(configuration.mClipRegion, false);
  }
  return NS_OK;
}

NS_IMETHODIMP
PuppetWidget::SetFocus(bool aRaise)
{
  if (aRaise && mTabChild) {
    mTabChild->SendRequestFocus(true);
  }

  return NS_OK;
}

NS_IMETHODIMP
PuppetWidget::Invalidate(const LayoutDeviceIntRect& aRect)
{
#ifdef DEBUG
  debug_DumpInvalidate(stderr, this, &aRect, "PuppetWidget", 0);
#endif

  if (mChild) {
    return mChild->Invalidate(aRect);
  }

  // Jihad: record that content changed so the offscreen render daemon repaints
  // (jihad_offscreen_take_dirty). Sticky — only the daemon's drain clears it.
  mJihadDirty = true;
  mJihadDirtyRect = mJihadDirtyRect.IsEmpty() ? aRect : mJihadDirtyRect.Union(aRect);

  mDirtyRegion.Or(mDirtyRegion, aRect);

  if (!mDirtyRegion.IsEmpty() && !mPaintTask.IsPending()) {
    mPaintTask = new PaintTask(this);
    return NS_DispatchToCurrentThread(mPaintTask.get());
  }

  return NS_OK;
}

void
PuppetWidget::InitEvent(WidgetGUIEvent& event, LayoutDeviceIntPoint* aPoint)
{
  if (nullptr == aPoint) {
    event.mRefPoint = LayoutDeviceIntPoint(0, 0);
  } else {
    // use the point override if provided
    event.mRefPoint = *aPoint;
  }
  event.mTime = PR_Now() / 1000;
}

NS_IMETHODIMP
PuppetWidget::DispatchEvent(WidgetGUIEvent* event, nsEventStatus& aStatus)
{
#ifdef DEBUG
  debug_DumpEvent(stdout, event->mWidget, event, "PuppetWidget", 0);
#endif

  MOZ_ASSERT(!mChild || mChild->mWindowType == eWindowType_popup,
             "Unexpected event dispatch!");

  AutoCacheNativeKeyCommands autoCache(this);
  if (event->mFlags.mIsSynthesizedForTests && !mNativeKeyCommandsValid) {
    WidgetKeyboardEvent* keyEvent = event->AsKeyboardEvent();
    // JIHAD: mTabChild is null in the standalone offscreen render daemon (there is no parent
    // content process). Native key bindings are supplied by that parent, so when it is absent
    // simply leave no cached native commands (fine for character input) rather than null-deref
    // on RequestNativeKeyBindings — that deref SIGSEGV'd the daemon on every synthesized key
    // event, which is exactly the crash that made typing into web inputs impossible.
    if (keyEvent && mTabChild) {
      mTabChild->RequestNativeKeyBindings(&autoCache, keyEvent);
    }
  }

  if (event->mClass == eCompositionEventClass) {
    // Store the latest native IME context of parent process's widget or
    // TextEventDispatcher if it's in this process.
    WidgetCompositionEvent* compositionEvent = event->AsCompositionEvent();
#ifdef DEBUG
    if (mNativeIMEContext.IsValid() &&
        mNativeIMEContext != compositionEvent->mNativeIMEContext) {
      RefPtr<TextComposition> composition =
        IMEStateManager::GetTextCompositionFor(this);
      MOZ_ASSERT(!composition,
        "When there is composition caused by old native IME context, "
        "composition events caused by different native IME context are not "
        "allowed");
    }
#endif // #ifdef DEBUG
    mNativeIMEContext = compositionEvent->mNativeIMEContext;
  }

  aStatus = nsEventStatus_eIgnore;

  if (GetCurrentWidgetListener()) {
    aStatus = GetCurrentWidgetListener()->HandleEvent(event, mUseAttachedEvents);
  }

  return NS_OK;
}

nsEventStatus
PuppetWidget::DispatchInputEvent(WidgetInputEvent* aEvent)
{
  if (!AsyncPanZoomEnabled()) {
    nsEventStatus status = nsEventStatus_eIgnore;
    DispatchEvent(aEvent, status);
    return status;
  }

  if (!mTabChild) {
    return nsEventStatus_eIgnore;
  }

  switch (aEvent->mClass) {
    case eWheelEventClass:
      Unused <<
        mTabChild->SendDispatchWheelEvent(*aEvent->AsWheelEvent());
      break;
    case eMouseEventClass:
      Unused <<
        mTabChild->SendDispatchMouseEvent(*aEvent->AsMouseEvent());
      break;
    case eKeyboardEventClass:
      Unused <<
        mTabChild->SendDispatchKeyboardEvent(*aEvent->AsKeyboardEvent());
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unsupported event type");
  }

  return nsEventStatus_eIgnore;
}

nsresult
PuppetWidget::SynthesizeNativeKeyEvent(int32_t aNativeKeyboardLayout,
                                       int32_t aNativeKeyCode,
                                       uint32_t aModifierFlags,
                                       const nsAString& aCharacters,
                                       const nsAString& aUnmodifiedCharacters,
                                       nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "keyevent");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeKeyEvent(aNativeKeyboardLayout, aNativeKeyCode,
    aModifierFlags, nsString(aCharacters), nsString(aUnmodifiedCharacters),
    notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::SynthesizeNativeMouseEvent(mozilla::LayoutDeviceIntPoint aPoint,
                                         uint32_t aNativeMessage,
                                         uint32_t aModifierFlags,
                                         nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "mouseevent");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeMouseEvent(aPoint, aNativeMessage,
    aModifierFlags, notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::SynthesizeNativeMouseMove(mozilla::LayoutDeviceIntPoint aPoint,
                                        nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "mousemove");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeMouseMove(aPoint, notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::SynthesizeNativeMouseScrollEvent(mozilla::LayoutDeviceIntPoint aPoint,
                                               uint32_t aNativeMessage,
                                               double aDeltaX,
                                               double aDeltaY,
                                               double aDeltaZ,
                                               uint32_t aModifierFlags,
                                               uint32_t aAdditionalFlags,
                                               nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "mousescrollevent");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeMouseScrollEvent(aPoint, aNativeMessage,
    aDeltaX, aDeltaY, aDeltaZ, aModifierFlags, aAdditionalFlags,
    notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::SynthesizeNativeTouchPoint(uint32_t aPointerId,
                                         TouchPointerState aPointerState,
                                         LayoutDeviceIntPoint aPoint,
                                         double aPointerPressure,
                                         uint32_t aPointerOrientation,
                                         nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "touchpoint");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeTouchPoint(aPointerId, aPointerState,
    aPoint, aPointerPressure, aPointerOrientation,
    notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::SynthesizeNativeTouchTap(LayoutDeviceIntPoint aPoint,
                                       bool aLongTap,
                                       nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "touchtap");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendSynthesizeNativeTouchTap(aPoint, aLongTap,
    notifier.SaveObserver());
  return NS_OK;
}

nsresult
PuppetWidget::ClearNativeTouchSequence(nsIObserver* aObserver)
{
  AutoObserverNotifier notifier(aObserver, "cleartouch");
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendClearNativeTouchSequence(notifier.SaveObserver());
  return NS_OK;
}
 
void
PuppetWidget::SetConfirmedTargetAPZC(uint64_t aInputBlockId,
                                     const nsTArray<ScrollableLayerGuid>& aTargets) const
{
  if (mTabChild) {
    mTabChild->SetTargetAPZC(aInputBlockId, aTargets);
  }
}

void
PuppetWidget::UpdateZoomConstraints(const uint32_t& aPresShellId,
                                    const FrameMetrics::ViewID& aViewId,
                                    const Maybe<ZoomConstraints>& aConstraints)
{
  if (mTabChild) {
    mTabChild->DoUpdateZoomConstraints(aPresShellId, aViewId, aConstraints);
  }
}

bool
PuppetWidget::AsyncPanZoomEnabled() const
{
  return mTabChild && mTabChild->AsyncPanZoomEnabled();
}

NS_IMETHODIMP_(bool)
PuppetWidget::ExecuteNativeKeyBinding(NativeKeyBindingsType aType,
                                      const mozilla::WidgetKeyboardEvent& aEvent,
                                      DoCommandCallback aCallback,
                                      void* aCallbackData)
{
  AutoCacheNativeKeyCommands autoCache(this);
  if (!aEvent.mWidget && !mNativeKeyCommandsValid) {
    MOZ_ASSERT(!aEvent.mFlags.mIsSynthesizedForTests);
    // Abort if untrusted to avoid leaking system settings
    if (NS_WARN_IF(!aEvent.IsTrusted())) {
      return false;
    }
    mTabChild->RequestNativeKeyBindings(&autoCache, &aEvent);
  }

  MOZ_ASSERT(mNativeKeyCommandsValid);

  const nsTArray<mozilla::CommandInt>* commands = nullptr;
  switch (aType) {
    case nsIWidget::NativeKeyBindingsForSingleLineEditor:
      commands = &mSingleLineCommands;
      break;
    case nsIWidget::NativeKeyBindingsForMultiLineEditor:
      commands = &mMultiLineCommands;
      break;
    case nsIWidget::NativeKeyBindingsForRichTextEditor:
      commands = &mRichTextCommands;
      break;
    default:
      MOZ_CRASH("Invalid type");
      break;
  }

  if (commands->IsEmpty()) {
    return false;
  }

  for (uint32_t i = 0; i < commands->Length(); i++) {
    aCallback(static_cast<mozilla::Command>((*commands)[i]), aCallbackData);
  }
  return true;
}

LayerManager*
PuppetWidget::GetLayerManager(PLayerTransactionChild* aShadowManager,
                              LayersBackend aBackendHint,
                              LayerManagerPersistence aPersistence)
{
  if (!mLayerManager) {
    if (JihadOffscreen()) {
      // Jihad: no compositor/IPC — paint on this thread with a BasicLayerManager
      // into mDrawTarget (see PuppetWidget::Paint LAYERS_BASIC branch).
      mLayerManager = CreateBasicLayerManager();
    } else {
      mLayerManager = new ClientLayerManager(this);
    }
  }
  ShadowLayerForwarder* lf = mLayerManager->AsShadowForwarder();
  if (lf && !lf->HasShadowManager() && aShadowManager) {
    lf->SetShadowManager(aShadowManager);
  }
  return mLayerManager;
}

LayerManager*
PuppetWidget::RecreateLayerManager(PLayerTransactionChild* aShadowManager)
{
  mLayerManager = new ClientLayerManager(this);
  if (ShadowLayerForwarder* lf = mLayerManager->AsShadowForwarder()) {
    lf->SetShadowManager(aShadowManager);
  }
  return mLayerManager;
}

// ---------------------------------------------------------------------------
// Jihad offscreen render support.
// ---------------------------------------------------------------------------
bool
PuppetWidget::JihadOffscreen()
{
  static int sMode = -1;
  if (sMode < 0) {
    const char* e = getenv("JIHAD_OFFSCREEN");
    sMode = (e && *e) ? 1 : 0;
  }
  return sMode == 1;
}

// Jihad headless: dimensions of the offscreen "screen", set by
// jihad_offscreen_create/resize and returned directly by PuppetScreen::GetRect.
// PuppetScreen must NOT ask hal for the screen config: the fallback hal
// implementation (hal/fallback/FallbackScreenConfiguration.cpp) resolves the
// config by querying the screen manager — i.e. PuppetScreen — which would
// recurse into hal again and overflow the stack.
static int32_t sJihadScreenWidth = 1024;
static int32_t sJihadScreenHeight = 768;

void
PuppetWidget::JihadEnsureDrawTarget()
{
  int32_t w = mBounds.width > 0 ? mBounds.width : 1;
  int32_t h = mBounds.height > 0 ? mBounds.height : 1;
  if (mDrawTarget) {
    IntSize have = mDrawTarget->GetSize();
    if (have.width >= w && have.height >= h) {
      return;
    }
  }
  mDrawTarget = gfxPlatform::GetPlatform()->
    CreateOffscreenContentDrawTarget(IntSize(w, h), SurfaceFormat::B8G8R8A8);
}

already_AddRefed<SourceSurface>
PuppetWidget::JihadSnapshot()
{
  if (!mDrawTarget) {
    return nullptr;
  }
  return mDrawTarget->Snapshot();
}

bool
PuppetWidget::JihadRenderDocument(nsIDocShell* aDocShell, double aZoom,
                                  double aPanX, double aPanY)
{
  if (!aDocShell) {
    return false;
  }
  JihadEnsureDrawTarget();
  if (!mDrawTarget) {
    return false;
  }
  nsCOMPtr<nsIPresShell> presShell = aDocShell->GetPresShell();
  if (!presShell) {
    return false;
  }
  // Make sure layout is up to date before we snapshot the document.
  presShell->FlushPendingNotifications(Flush_Layout);

  int32_t w = mBounds.width  > 0 ? mBounds.width  : 1;
  int32_t h = mBounds.height > 0 ? mBounds.height : 1;

  // Clear the whole target to white first so any area RenderDocument does not cover
  // (e.g. a viewport smaller than the buffer) reads white, not the initial black.
  mDrawTarget->FillRect(mozilla::gfx::Rect(0, 0, mDrawTarget->GetSize().width,
                                           mDrawTarget->GetSize().height),
                        mozilla::gfx::ColorPattern(mozilla::gfx::Color(1.f, 1.f, 1.f, 1.f)));

  RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(mDrawTarget);
  if (!ctx) {
    return false;
  }
  // Pinch/fit ZOOM (Jihad): magnify the capture by aZoom WITHOUT reflowing layout. Pre-scale
  // the target context by Z and render only the 1/Z-reduced visible region, so w/Z x h/Z CSS
  // px fill the w x h device buffer at Z x magnification. RenderDocument's own internal scale
  // (AppUnitsPerDevPixel/AppUnitsPerCSSPixel) is 1 here (layout viewport left at the window
  // width, no engine zoom), so the net paint scale is exactly Z. RenderDocument clips to the
  // rect using the CURRENT (pre-scaled) matrix, so the clip lands at w x h device px. Z==1 is
  // the identity path (unchanged). This is why SetFullZoom/SetResolution were wrong: they made
  // the engine zoom, which RenderDocument's internal scale then cancels back into a 1/Z quadrant.
  // RENDER_CARET draws the text caret when an editable is focused (Jihad: the on-screen
  // keyboard needs a visible cursor).
  double Z = (aZoom > 0.0) ? aZoom : 1.0;
  uint32_t flags = nsIPresShell::RENDER_CARET;
  nsRect r;
  if (Z < 0.99 || Z > 1.01 || aPanX != 0.0 || aPanY != 0.0) {
    // Zoomed / panned (|Z-1|>1%, matching the daemon's is-zoomed threshold so a near-1 fit-zoom
    // like 1.0052 stays on the engine-scroll path and keeps window.onscroll/fixed/sticky correct):
    // magnify by Z and render an ABSOLUTE DOCUMENT rect (aPanX,aPanY,w/Z,h/Z),
    // IGNORING the engine scroll (RENDER_IGNORE_VIEWPORT_SCROLLING | RENDER_DOCUMENT_RELATIVE).
    // This is a pure visual viewport: the whole page is reachable in BOTH axes with no display-
    // list culling and no dependence on the engine scroll, so there is no blank-past-the-first-
    // screen, no engine-parking echo, and zoom-OUT (Z<1, rect > viewport) is not clipped.
    // RenderDocument clips to aRect.width/height in the (pre-scaled) matrix, so the clip lands at
    // exactly w x h device px. RenderDocument's internal AppUnitsPerDevPixel/AppUnitsPerCSSPixel
    // scale is 1 (no engine zoom — layout viewport stays device-width), so the net paint scale
    // is exactly Z (zoom fix 2026-07-27; Codex review F2-F6 -> document-relative).
    ctx->SetMatrix(ctx->CurrentMatrix().Scale(Z, Z));
    flags |= nsIPresShell::RENDER_IGNORE_VIEWPORT_SCROLLING | nsIPresShell::RENDER_DOCUMENT_RELATIVE;
    r = nsRect(nsPresContext::CSSPixelsToAppUnits((float)aPanX),
               nsPresContext::CSSPixelsToAppUnits((float)aPanY),
               nsPresContext::CSSPixelsToAppUnits((float)(w / Z)),
               nsPresContext::CSSPixelsToAppUnits((float)(h / Z)));
  } else {
    // Unzoomed (Z==1, no pan): viewport-relative, the engine scroll drives position (so
    // window.onscroll + position:fixed/sticky stay correct). Byte-identical to the pre-zoom-fix
    // capture.
    r = nsRect(0, 0,
               nsPresContext::CSSPixelsToAppUnits((float)w),
               nsPresContext::CSSPixelsToAppUnits((float)h));
  }
  nsresult rv = presShell->RenderDocument(r, flags, NS_RGB(255, 255, 255), ctx);
  return NS_SUCCEEDED(rv);
}

// C entry points for the frozen-API render daemon. Exported from libxul so the
// daemon (which cannot see internal classes like PuppetWidget) can create an
// offscreen widget, hand it to nsIBaseWindow::InitWindow as the parent nsIWidget,
// drive a paint, and read the rendered ARGB32 pixels into its shared buffer.
extern "C" {

MOZ_EXPORT nsIWidget*
jihad_offscreen_create(int aWidth, int aHeight)
{
  if (aWidth > 0)  sJihadScreenWidth = aWidth;
  if (aHeight > 0) sJihadScreenHeight = aHeight;
  RefPtr<PuppetWidget> w = new PuppetWidget(nullptr);
  LayoutDeviceIntRect bounds(0, 0, aWidth, aHeight);
  nsWidgetInitData initData;
  initData.mWindowType = eWindowType_toplevel;
  w->Create(nullptr, nullptr, bounds, &initData);
  w->Resize(aWidth, aHeight, false);
  w->Show(true);
  w->JihadEnsureDrawTarget();
  return w.forget().take();
}

MOZ_EXPORT void
jihad_offscreen_resize(nsIWidget* aWidget, int aWidth, int aHeight)
{
  if (aWidget) {
    if (aWidth > 0)  sJihadScreenWidth = aWidth;
    if (aHeight > 0) sJihadScreenHeight = aHeight;
    aWidget->Resize(aWidth, aHeight, true);
  }
}

MOZ_EXPORT void
jihad_offscreen_paint(nsIWidget* aWidget)
{
  if (aWidget) {
    static_cast<PuppetWidget*>(aWidget)->PaintNowIfNeeded();
  }
}

// True (and clears the flag) if content invalidated since the last call — layout
// invalidations from incremental page render, JS/SPA DOM updates, async image
// decode, CSS animation. The daemon polls this each tick and repaints on dirty,
// giving the offscreen embedding the engine-driven frame delivery the stock
// QtWebKit BrowserServer got from Qt paint events (BrowserPage::paintEvent).
MOZ_EXPORT bool
jihad_offscreen_take_dirty(nsIWidget* aWidget)
{
  if (!aWidget) {
    return false;
  }
  return static_cast<PuppetWidget*>(aWidget)->JihadTakeDirty();
}

// The bounding box of what changed, so the daemon can repaint only those rows. See
// PuppetWidget::JihadTakeDirtyRect. Returns false when nothing is dirty, leaving the outs
// untouched; a caller that ignores this simply keeps doing full-frame repaints.
MOZ_EXPORT bool
jihad_offscreen_take_dirty_rect(nsIWidget* aWidget, int32_t* aX, int32_t* aY,
                                int32_t* aW, int32_t* aH)
{
  if (!aWidget) {
    return false;
  }
  return static_cast<PuppetWidget*>(aWidget)->JihadTakeDirtyRect(aX, aY, aW, aH);
}

// Render the docShell's document into the widget's offscreen DrawTarget via the
// presShell (canonical offscreen render). The daemon passes the content nsIDocShell
// it already resolves. Returns true if the document rendered.
MOZ_EXPORT bool
jihad_offscreen_render_document_v2(nsIWidget* aWidget, nsIDocShell* aDocShell, double aZoom,
                                   double aPanX, double aPanY)
{
  // Versioned symbol (Codex F1): the zoom/pan args were added to an extern-C function, which has
  // no parameter mangling — an old daemon vs new libxul (or vice versa) would pass garbage or
  // silently drop the args. The rename makes any stale-binary mismatch fail LOUDLY at RTLD_NOW
  // load (undefined symbol) instead of mis-rendering.
  if (!aWidget) {
    return false;
  }
  return static_cast<PuppetWidget*>(aWidget)->JihadRenderDocument(aDocShell, aZoom, aPanX, aPanY);
}

MOZ_EXPORT bool
jihad_offscreen_readback(nsIWidget* aWidget, void* aDest, int aStride,
                         int aWidth, int aHeight)
{
  if (!aWidget || !aDest) {
    return false;
  }
  PuppetWidget* pw = static_cast<PuppetWidget*>(aWidget);
  // NOTE: do NOT call PaintNowIfNeeded() here — the widget Paint() path does not
  // paint the embedded document into this offscreen widget (it clears mDrawTarget to
  // black). The daemon renders the document via jihad_offscreen_render_document
  // (presShell->RenderDocument) just before this readback; we only snapshot.
  RefPtr<SourceSurface> snap = pw->JihadSnapshot();
  if (!snap) {
    return false;
  }
  RefPtr<DataSourceSurface> data = snap->GetDataSurface();
  if (!data) {
    return false;
  }
  size_t length;
  int32_t srcStride;
  mozilla::UniquePtr<char[]> buf =
    nsContentUtils::GetSurfaceData(WrapNotNull(data.get()), &length, &srcStride);
  if (!buf) {
    return false;
  }
  IntSize sz = data->GetSize();
  int rows = aHeight < sz.height ? aHeight : sz.height;
  int rowBytes = (aWidth < sz.width ? aWidth : sz.width) * 4;
  for (int y = 0; y < rows; ++y) {
    memcpy(static_cast<uint8_t*>(aDest) + (size_t)y * aStride,
           buf.get() + (size_t)y * srcStride, rowBytes);
  }
  return true;
}

// Render an ABSOLUTE document region straight into caller-owned memory (the
// daemon's shared buffer). Unlike jihad_offscreen_render_document_v2 this is not
// bounded by the widget's DrawTarget size, so the daemon can paint a region
// TALLER than the viewport (viewport + overscan) and give the adapter real pan
// headroom (scroll pan headroom fix 2026-08-02: painting exactly the viewport
// left zero headroom, so one pixel of pan exposed undrawn grey).
// aDocX/aDocY are CSS px (document-absolute); the region is aWidth x aHeight
// DEVICE px rendered at aZoom, i.e. it covers aWidth/Z x aHeight/Z CSS px.
// Always RENDER_DOCUMENT_RELATIVE + RENDER_IGNORE_VIEWPORT_SCROLLING: the engine
// scroll does not shift the output, and position:fixed content is NOT placed at
// the current scroll — at z~1 the daemon overlays a viewport-relative band
// (render_document_v2 + readback) over the visible rows to keep fixed/sticky
// correct on screen; the overscan strips only ever show transiently mid-pan.
MOZ_EXPORT bool
jihad_offscreen_render_region(nsIWidget* /*aWidget*/, nsIDocShell* aDocShell, double aZoom,
                              double aDocX, double aDocY, void* aDest, int aStride,
                              int aWidth, int aHeight)
{
  if (!aDocShell || !aDest || aWidth <= 0 || aHeight <= 0 || aStride < aWidth * 4) {
    return false;
  }
  nsCOMPtr<nsIPresShell> presShell = aDocShell->GetPresShell();
  if (!presShell) {
    return false;
  }
  presShell->FlushPendingNotifications(Flush_Layout);
  RefPtr<DrawTarget> dt = Factory::CreateDrawTargetForData(
      BackendType::CAIRO, static_cast<unsigned char*>(aDest),
      IntSize(aWidth, aHeight), aStride, SurfaceFormat::B8G8R8A8);
  if (!dt || !dt->IsValid()) {
    return false;
  }
  // Opaque white base: RenderDocument's over-op on an opaque base keeps alpha at
  // 255 everywhere, which the adapter's raw-word blit requires (no post-pass
  // alpha forcing on this path — the pixels land directly in the shared buffer).
  dt->FillRect(mozilla::gfx::Rect(0, 0, aWidth, aHeight),
               mozilla::gfx::ColorPattern(mozilla::gfx::Color(1.f, 1.f, 1.f, 1.f)));
  RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(dt);
  if (!ctx) {
    return false;
  }
  double Z = (aZoom > 0.0) ? aZoom : 1.0;
  ctx->SetMatrix(ctx->CurrentMatrix().Scale(Z, Z));
  uint32_t flags = nsIPresShell::RENDER_CARET |
                   nsIPresShell::RENDER_IGNORE_VIEWPORT_SCROLLING |
                   nsIPresShell::RENDER_DOCUMENT_RELATIVE;
  nsRect r(nsPresContext::CSSPixelsToAppUnits((float)aDocX),
           nsPresContext::CSSPixelsToAppUnits((float)aDocY),
           nsPresContext::CSSPixelsToAppUnits((float)(aWidth / Z)),
           nsPresContext::CSSPixelsToAppUnits((float)(aHeight / Z)));
  nsresult rv = presShell->RenderDocument(r, flags, NS_RGB(255, 255, 255), ctx);
  if (NS_FAILED(rv)) {
    return false;
  }
  // ENFORCE alpha == 255 over every pixel (adversarial review 2026-08-02 F3): the
  // adapter raw-blits these words into the card surface, so any a<255 pixel is a
  // see-through hole. The opaque-white base + OP_OVER argument holds on the paths
  // we traced, but it is an engine-internal invariant across the whole display
  // list — enforce it here rather than trust it (a few ms of byte stores).
  dt->Flush();
  for (int y = 0; y < aHeight; ++y) {
    unsigned char* row = static_cast<unsigned char*>(aDest) + (size_t)y * aStride;
    for (int x = 0; x < aWidth; ++x) {
      row[(size_t)x * 4 + 3] = 0xff;
    }
  }
  return true;
}

// Composite every OPEN XUL popup over an already-painted buffer.
//
// WHY THIS EXISTS. A popup (menupopup: the about:addons tools menu, context menus)
// is a SEPARATE DISPLAY ROOT — nsLayoutUtils::GetDisplayRootFrame returns the popup
// frame itself, not the document root — so RenderDocument on the content docShell
// never contains it, no matter how it is sized or positioned. On a desktop build the
// popup would be its own toplevel OS window and the window manager would put it on
// screen; headless there is no such window, so the pixels have to be composited by
// hand. The daemon calls this after its main paint, so a popup lands on top of the
// page exactly where the engine placed it.
//
// aOriginX/aOriginY are the document-space CSS coords of the buffer's top-left (the
// same origin the main render used) and aZoom the scale it rendered at, so a popup
// is placed with the identical transform as the content underneath it.
//
// Returns the number of popups composited (0 = nothing open, the overwhelmingly
// common case, and the reason this is cheap enough to call every frame).
MOZ_EXPORT int
jihad_offscreen_composite_popups(nsIWidget* /*aWidget*/, void* aDest, int aStride,
                                 int aWidth, int aHeight, double aOriginX,
                                 double aOriginY, double aZoom)
{
  if (!aDest || aWidth <= 0 || aHeight <= 0 || aStride < aWidth * 4) {
    return 0;
  }
  nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
  if (!pm) {
    return 0;
  }
  nsTArray<nsIFrame*> popups;
  pm->GetVisiblePopups(popups);
  if (popups.IsEmpty()) {
    return 0;      // no popup open: pay nothing beyond this check
  }

  RefPtr<DrawTarget> dt = Factory::CreateDrawTargetForData(
      BackendType::CAIRO, static_cast<unsigned char*>(aDest),
      IntSize(aWidth, aHeight), aStride, SurfaceFormat::B8G8R8A8);
  if (!dt || !dt->IsValid()) {
    return 0;
  }
  const double Z = (aZoom > 0.0) ? aZoom : 1.0;
  int painted = 0;

  // GetVisiblePopups walks the chain innermost-first; paint in reverse so a submenu
  // lands on top of the menu that opened it.
  for (int32_t i = popups.Length() - 1; i >= 0; --i) {
    nsIFrame* frame = popups[i];
    if (!frame) {
      continue;
    }
    // The popup's own rect, in the same document space the buffer was rendered in.
    // GetScreenRectInAppUnits() is what the widget code positions the popup window
    // with, and in this embedding the "screen" IS the document surface: PuppetScreen
    // reports the offscreen bounds, so screen coords and document coords agree.
    nsRect screen = frame->GetScreenRectInAppUnits();
    const double cssX = nsPresContext::AppUnitsToFloatCSSPixels(screen.x);
    const double cssY = nsPresContext::AppUnitsToFloatCSSPixels(screen.y);
    const double cssW = nsPresContext::AppUnitsToFloatCSSPixels(screen.width);
    const double cssH = nsPresContext::AppUnitsToFloatCSSPixels(screen.height);
    if (cssW <= 0 || cssH <= 0) {
      // A popup that is open but has no size is the "0x0 popup" failure this whole
      // path was built for — say so rather than silently drawing nothing.
      if (getenv("JIHAD_OFFSCREEN")) {
        fprintf(stderr, "[jihad-popup] composite: frame=%p is OPEN but %gx%g — skipped\n",
                (void*)frame, cssW, cssH);
      }
      continue;
    }
    // Device px in the destination buffer.
    const double dx = (cssX - aOriginX) * Z;
    const double dy = (cssY - aOriginY) * Z;
    if (dx >= aWidth || dy >= aHeight || dx + cssW * Z <= 0 || dy + cssH * Z <= 0) {
      continue;                                   // entirely off this buffer
    }

    RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(dt);
    if (!ctx) {
      continue;
    }
    // Clip to the popup's own box so a display list that paints outside its bounds
    // (shadows, overflow) cannot scribble over the page around it.
    ctx->Save();
    ctx->Clip(mozilla::gfx::Rect(dx, dy, cssW * Z, cssH * Z));
    // cairo semantics: each call pre-multiplies, i.e. it applies FIRST in user
    // space. Scale then translate => device = css * Z + (dx, dy), which places the
    // popup's own coordinate space at its spot in this buffer.
    ctx->SetMatrix(ctx->CurrentMatrix().Translate(gfxPoint(dx, dy)).Scale(Z, Z));
    nsRenderingContext rc(ctx);
    // Frame-relative dirty region covering the whole popup. PAINT_DOCUMENT_RELATIVE
    // is deliberately NOT set: we want the popup drawn at its own origin, which the
    // transform above has already placed.
    nsRect dirty(nsPoint(0, 0), screen.Size());
    nsresult rv = nsLayoutUtils::PaintFrame(
        &rc, frame, nsRegion(dirty), NS_RGBA(0, 0, 0, 0),
        nsDisplayListBuilderMode::PAINTING,
        nsLayoutUtils::PaintFrameFlags::PAINT_SYNC_DECODE_IMAGES |
        nsLayoutUtils::PaintFrameFlags::PAINT_IGNORE_SUPPRESSION |
        nsLayoutUtils::PaintFrameFlags::PAINT_HIDE_CARET);
    ctx->Restore();
    if (NS_FAILED(rv)) {
      if (getenv("JIHAD_OFFSCREEN")) {
        fprintf(stderr, "[jihad-popup] composite: PaintFrame failed frame=%p rv=0x%x\n",
                (void*)frame, (unsigned)rv);
      }
      continue;
    }
    ++painted;
    if (getenv("JIHAD_MENU_DEBUG")) {
      // Which row does the engine consider active, and does it carry the attribute the
      // highlight rule keys on? Without this, "no highlight" cannot be told apart from
      // "no active item" — the two look identical on screen.
      nsMenuPopupFrame* mpf = do_QueryFrame(frame);
      nsMenuFrame* cur = mpf ? mpf->GetCurrentMenuItem() : nullptr;
      nsIContent* c = cur ? cur->GetContent() : nullptr;
      nsAutoString act;
      if (c) c->GetAttr(kNameSpaceID_None, nsGkAtoms::menuactive, act);
      fprintf(stderr, "[jihad-popup] active item=%p content=%p _moz-menuactive=[%s]\n",
              (void*)cur, (void*)c, NS_ConvertUTF16toUTF8(act).get());
    }
    if (getenv("JIHAD_OFFSCREEN")) {
      fprintf(stderr, "[jihad-popup] composite: frame=%p at %g,%g %gx%g (dev %g,%g z=%g)\n",
              (void*)frame, cssX, cssY, cssW, cssH, dx, dy, Z);
    }
  }

  if (painted) {
    // Same alpha invariant the region render enforces: the adapter raw-blits these
    // words into the card surface, so a popup pixel with a<255 is a see-through hole.
    dt->Flush();
    for (int y = 0; y < aHeight; ++y) {
      unsigned char* row = static_cast<unsigned char*>(aDest) + (size_t)y * aStride;
      for (int x = 0; x < aWidth; ++x) {
        row[(size_t)x * 4 + 3] = 0xff;
      }
    }
  }
  return painted;
}

// How many popups are open right now. The daemon uses this straight after dispatching a
// tap to tell "that tap opened a menu" from "it did not", which is what lets it drop the
// DUPLICATE delivery of the same physical tap instead of letting it close the menu again.
MOZ_EXPORT int
jihad_offscreen_popup_count()
{
  nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
  if (!pm) {
    return 0;
  }
  nsTArray<nsIFrame*> popups;
  pm->GetVisiblePopups(popups);
  return (int)popups.Length();
}

// Close every open popup ("roll up").
//
// On a desktop build this happens by itself: the popup's widget takes a native mouse
// capture, so a click anywhere else reaches the popup manager as a rollup. There is no
// native capture in this headless embedding — the daemon synthesises taps straight into
// the content document's presShell — so tapping outside a menu left it open (measured;
// it closed only sometimes, depending on what the tap happened to hit). The daemon calls
// this explicitly for a tap that lands outside every popup, which is what a user means.
MOZ_EXPORT bool
jihad_offscreen_popup_rollup()
{
  nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
  if (!pm) {
    return false;
  }
  nsTArray<nsIFrame*> popups;
  pm->GetVisiblePopups(popups);
  if (popups.IsEmpty()) {
    return false;
  }
  // aCount 0 = roll up the whole chain; aFlush = run the close synchronously so the next
  // paint no longer contains the popup.
  bool rolled = pm->Rollup(0, true, nullptr, nullptr);
  if (getenv("JIHAD_OFFSCREEN")) {
    fprintf(stderr, "[jihad-popup] rollup: closed=%d (%u were open)\n",
            (int)rolled, (unsigned)popups.Length());
  }
  return rolled;
}

// Route a mouse event into an OPEN popup.
//
// The same separate-display-root property that keeps a popup out of the main render
// also keeps it out of the main document's hit-testing: the daemon's normal input path
// resolves a tap with elementFromPoint on the CONTENT document, which walks the content
// display root and cannot see popup frames. Measured: with the tools menu open, a tap on
// its first row resolved to the <vbox> UNDER the menu. So a popup that is visible is not
// operable unless its events are dispatched against its own display root, which is what
// this does.
//
// aX/aY are document CSS px (same space the composite places the popup in). Returns true
// if a visible popup contained the point and the event was dispatched to it — the caller
// then does NOT run its normal content-document dispatch for that tap.
// aMsg: 0 = move, 1 = down, 2 = up.
MOZ_EXPORT bool
jihad_offscreen_popup_mouse(int aMsg, double aX, double aY)
{
  nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
  if (!pm) {
    return false;
  }
  nsTArray<nsIFrame*> popups;
  pm->GetVisiblePopups(popups);
  if (popups.IsEmpty()) {
    return false;
  }
  const nscoord appX = nsPresContext::CSSPixelsToAppUnits((float)aX);
  const nscoord appY = nsPresContext::CSSPixelsToAppUnits((float)aY);

  // Innermost first: GetVisiblePopups walks the chain from the deepest submenu out, so
  // the first hit is the one the user is actually pointing at.
  for (uint32_t i = 0; i < popups.Length(); ++i) {
    nsIFrame* frame = popups[i];
    if (!frame) {
      continue;
    }
    nsRect screen = frame->GetScreenRectInAppUnits();
    if (screen.width <= 0 || screen.height <= 0 ||
        appX < screen.x || appX >= screen.XMost() ||
        appY < screen.y || appY >= screen.YMost()) {
      continue;
    }
    nsIPresShell* presShell = frame->PresContext()->PresShell();
    if (!presShell) {
      return false;
    }
    // The event's refPoint is widget-relative; a popup's own widget has the popup's
    // top-left as its origin, so subtract the popup's screen position.
    nsIWidget* widget = frame->GetNearestWidget();
    if (!widget) {
      return false;
    }
    const int32_t localX = nsPresContext::AppUnitsToIntCSSPixels(appX - screen.x);
    const int32_t localY = nsPresContext::AppUnitsToIntCSSPixels(appY - screen.y);

    EventMessage msg = (aMsg == 1) ? eMouseDown : (aMsg == 2) ? eMouseUp : eMouseMove;
    WidgetMouseEvent event(true, msg, widget, WidgetMouseEvent::eReal);
    event.mRefPoint = LayoutDeviceIntPoint(localX, localY);
    event.mClickCount = (msg == eMouseMove) ? 0 : 1;
    event.button = WidgetMouseEvent::eLeftButton;
    if (msg != eMouseMove) {
      event.mFlags.mIsSynthesizedForTests = false;
    }
    nsEventStatus status = nsEventStatus_eIgnore;
    presShell->HandleEvent(frame, &event, false, &status);
    if (getenv("JIHAD_OFFSCREEN")) {
      fprintf(stderr, "[jihad-popup] mouse msg=%d doc=%g,%g -> popup-local %d,%d status=%d\n",
              aMsg, aX, aY, localX, localY, (int)status);
    }
    return true;
  }
  return false;
}

MOZ_EXPORT void
jihad_offscreen_release(nsIWidget* aWidget)
{
  if (aWidget) {
    PuppetWidget* pw = static_cast<PuppetWidget*>(aWidget);
    pw->Destroy();
    pw->Release();
  }
}

// Force PSM/NSS (TLS) to initialize on the CURRENT (main) thread. The frozen-API
// render daemon can't resolve the PSM contract via do_GetService, but this internal
// libxul code can (EnsureNSSInitializedChromeOrContent -> do_GetService(PSM)). Without
// this, the first https:// request lazily constructs nsNSSComponent from the necko
// socket thread, whose ctor does MOZ_RELEASE_ASSERT(NS_IsMainThread()) -> daemon abort
// ("loads forever"). The daemon calls this on its main thread before an https load.
MOZ_EXPORT bool
jihad_init_nss()
{
  // Per the crash backtrace: on the first https:// load, necko's SOCKET thread does
  // nsSocketTransport::Init -> GetSocketProvider("ssl") -> nsSSLSocketProviderConstructor
  // -> EnsureNSSInitialized -> do_GetService(PSM) -> nsNSSComponent ctor, which asserts
  // NS_IsMainThread() and crashes. Direct do_GetService(PSM) fails on the main thread
  // (PSM registers only when the ssl socket provider is first built). So build the SSL
  // socket provider HERE on the main thread — it runs the exact same EnsureNSSInitialized
  // chain, constructing nsNSSComponent on the main thread; the socket thread then reuses
  // the singleton and no longer constructs it. (Idempotent — cached after first call.)
  nsresult rv = NS_ERROR_FAILURE;
  nsCOMPtr<nsISupports> ssl =
    do_GetService("@mozilla.org/network/socket;2?type=ssl", &rv);
  nsCOMPtr<nsISupports> tls =
    do_GetService("@mozilla.org/network/socket;2?type=starttls");
  fprintf(stderr, "[jihad-bs] jihad_init_nss(ssl provider): main=%d rv=0x%x got=%p\n",
          (int)NS_IsMainThread(), (unsigned)rv, (void*)ssl.get());
  return NS_SUCCEEDED(rv);
}

} // extern "C"

nsresult
PuppetWidget::RequestIMEToCommitComposition(bool aCancel)
{
#ifdef MOZ_CROSS_PROCESS_IME
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(!Destroyed());

  // There must not be composition which is caused by the PuppetWidget instance.
  if (NS_WARN_IF(!mNativeIMEContext.IsValid())) {
    return NS_OK;
  }

  RefPtr<TextComposition> composition =
    IMEStateManager::GetTextCompositionFor(this);
  // This method shouldn't be called when there is no text composition instance.
  if (NS_WARN_IF(!composition)) {
    return NS_OK;
  }

  bool isCommitted = false;
  nsAutoString committedString;
  if (NS_WARN_IF(!mTabChild->SendRequestIMEToCommitComposition(
                               aCancel, &isCommitted, &committedString))) {
    return NS_ERROR_FAILURE;
  }

  // If the composition wasn't committed synchronously, we need to wait async
  // composition events for destroying the TextComposition instance.
  if (!isCommitted) {
    return NS_OK;
  }

  // Dispatch eCompositionCommit event.
  WidgetCompositionEvent compositionCommitEvent(true, eCompositionCommit, this);
  InitEvent(compositionCommitEvent, nullptr);
  compositionCommitEvent.mData = committedString;
  nsEventStatus status = nsEventStatus_eIgnore;
  DispatchEvent(&compositionCommitEvent, status);

  // NOTE: PuppetWidget might be destroyed already.

#endif // #ifdef MOZ_CROSS_PROCESS_IME

  return NS_OK;
}

nsresult
PuppetWidget::NotifyIMEInternal(const IMENotification& aIMENotification)
{
  switch (aIMENotification.mMessage) {
    case REQUEST_TO_COMMIT_COMPOSITION:
      return RequestIMEToCommitComposition(false);
    case REQUEST_TO_CANCEL_COMPOSITION:
      return RequestIMEToCommitComposition(true);
    case NOTIFY_IME_OF_FOCUS:
    case NOTIFY_IME_OF_BLUR:
      return NotifyIMEOfFocusChange(aIMENotification);
    case NOTIFY_IME_OF_SELECTION_CHANGE:
      return NotifyIMEOfSelectionChange(aIMENotification);
    case NOTIFY_IME_OF_TEXT_CHANGE:
      return NotifyIMEOfTextChange(aIMENotification);
    case NOTIFY_IME_OF_COMPOSITION_EVENT_HANDLED:
      return NotifyIMEOfCompositionUpdate(aIMENotification);
    case NOTIFY_IME_OF_MOUSE_BUTTON_EVENT:
      return NotifyIMEOfMouseButtonEvent(aIMENotification);
    case NOTIFY_IME_OF_POSITION_CHANGE:
      return NotifyIMEOfPositionChange(aIMENotification);
    default:
      return NS_ERROR_NOT_IMPLEMENTED;
  }
}

NS_IMETHODIMP
PuppetWidget::StartPluginIME(const mozilla::WidgetKeyboardEvent& aKeyboardEvent,
                             int32_t aPanelX, int32_t aPanelY,
                             nsString& aCommitted)
{
  if (!mTabChild ||
      !mTabChild->SendStartPluginIME(aKeyboardEvent, aPanelX,
                                     aPanelY, &aCommitted)) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

void
PuppetWidget::SetPluginFocused(bool& aFocused)
{
  if (mTabChild) {
    mTabChild->SendSetPluginFocused(aFocused);
  }
}

void
PuppetWidget::DefaultProcOfPluginEvent(const WidgetPluginEvent& aEvent)
{
  if (!mTabChild) {
    return;
  }
  mTabChild->SendDefaultProcOfPluginEvent(aEvent);
}

NS_IMETHODIMP_(void)
PuppetWidget::SetInputContext(const InputContext& aContext,
                              const InputContextAction& aAction)
{
  mInputContext = aContext;
  // Any widget instances cannot cache IME open state because IME open state
  // can be changed by user but native IME may not notify us of changing the
  // open state on some platforms.
  mInputContext.mIMEState.mOpen = IMEState::OPEN_STATE_NOT_SUPPORTED;

#ifndef MOZ_CROSS_PROCESS_IME
  return;
#endif

  if (!mTabChild) {
    return;
  }
  mTabChild->SendSetInputContext(
    static_cast<int32_t>(aContext.mIMEState.mEnabled),
    static_cast<int32_t>(aContext.mIMEState.mOpen),
    aContext.mHTMLInputType,
    aContext.mHTMLInputInputmode,
    aContext.mActionHint,
    static_cast<int32_t>(aAction.mCause),
    static_cast<int32_t>(aAction.mFocusChange));
}

NS_IMETHODIMP_(InputContext)
PuppetWidget::GetInputContext()
{
#ifndef MOZ_CROSS_PROCESS_IME
  return InputContext();
#endif

  // XXX Currently, we don't support retrieving IME open state from child
  //     process.

  // When this widget caches input context and currently managed by
  // IMEStateManager, the cache is valid.  Only in this case, we can
  // avoid to use synchronous IPC.
  if (mInputContext.mIMEState.mEnabled != IMEState::UNKNOWN &&
      IMEStateManager::GetWidgetForActiveInputContext() == this) {
    return mInputContext;
  }

  NS_WARNING("PuppetWidget::GetInputContext() needs to retrieve it with IPC");

  // Don't cache InputContext here because this process isn't managing IME
  // state of the chrome widget.  So, we cannot modify mInputContext when
  // chrome widget is set to new context.
  InputContext context;
  if (mTabChild) {
    int32_t enabled, open;
    mTabChild->SendGetInputContext(&enabled, &open);
    context.mIMEState.mEnabled = static_cast<IMEState::Enabled>(enabled);
    context.mIMEState.mOpen = static_cast<IMEState::Open>(open);
  }
  return context;
}

NS_IMETHODIMP_(NativeIMEContext)
PuppetWidget::GetNativeIMEContext()
{
  return mNativeIMEContext;
}

nsresult
PuppetWidget::NotifyIMEOfFocusChange(const IMENotification& aIMENotification)
{
#ifndef MOZ_CROSS_PROCESS_IME
  return NS_OK;
#endif

  if (!mTabChild)
    return NS_ERROR_FAILURE;

  bool gotFocus = aIMENotification.mMessage == NOTIFY_IME_OF_FOCUS;
  if (gotFocus) {
    if (mInputContext.mIMEState.mEnabled != IMEState::PLUGIN) {
      // When IME gets focus, we should initalize all information of the
      // content.
      if (NS_WARN_IF(!mContentCache.CacheAll(this, &aIMENotification))) {
        return NS_ERROR_FAILURE;
      }
    } else {
      // However, if a plugin has focus, only the editor rect information is
      // available.
      if (NS_WARN_IF(!mContentCache.CacheEditorRect(this, &aIMENotification))) {
        return NS_ERROR_FAILURE;
      }
    }
  } else {
    // When IME loses focus, we don't need to store anything.
    mContentCache.Clear();
  }

  mIMEPreferenceOfParent = nsIMEUpdatePreference();
  if (!mTabChild->SendNotifyIMEFocus(mContentCache, aIMENotification,
                                     &mIMEPreferenceOfParent)) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult
PuppetWidget::NotifyIMEOfCompositionUpdate(
                const IMENotification& aIMENotification)
{
#ifndef MOZ_CROSS_PROCESS_IME
  return NS_OK;
#endif

  NS_ENSURE_TRUE(mTabChild, NS_ERROR_FAILURE);

  if (mInputContext.mIMEState.mEnabled != IMEState::PLUGIN &&
      NS_WARN_IF(!mContentCache.CacheSelection(this, &aIMENotification))) {
    return NS_ERROR_FAILURE;
  }
  mTabChild->SendNotifyIMECompositionUpdate(mContentCache, aIMENotification);
  return NS_OK;
}

nsIMEUpdatePreference
PuppetWidget::GetIMEUpdatePreference()
{
#ifdef MOZ_CROSS_PROCESS_IME
  // e10s requires IME content cache in in the TabParent for handling query
  // content event only with the parent process.  Therefore, this process
  // needs to receive a lot of information from the focused editor to sent
  // the latest content to the parent process.
  if (mInputContext.mIMEState.mEnabled == IMEState::PLUGIN) {
    // But if a plugin has focus, we cannot receive text nor selection change
    // in the plugin.  Therefore, PuppetWidget needs to receive only position
    // change event for updating the editor rect cache.
    return nsIMEUpdatePreference(mIMEPreferenceOfParent.mWantUpdates |
                                 nsIMEUpdatePreference::NOTIFY_POSITION_CHANGE);
  }
  return nsIMEUpdatePreference(mIMEPreferenceOfParent.mWantUpdates |
                               nsIMEUpdatePreference::NOTIFY_TEXT_CHANGE |
                               nsIMEUpdatePreference::NOTIFY_POSITION_CHANGE );
#else
  // B2G doesn't handle IME as widget-level.
  return nsIMEUpdatePreference();
#endif
}

nsresult
PuppetWidget::NotifyIMEOfTextChange(const IMENotification& aIMENotification)
{
  MOZ_ASSERT(aIMENotification.mMessage == NOTIFY_IME_OF_TEXT_CHANGE,
             "Passed wrong notification");

#ifndef MOZ_CROSS_PROCESS_IME
  return NS_OK;
#endif

  if (!mTabChild)
    return NS_ERROR_FAILURE;

  // While a plugin has focus, text change notification shouldn't be available.
  if (NS_WARN_IF(mInputContext.mIMEState.mEnabled == IMEState::PLUGIN)) {
    return NS_ERROR_FAILURE;
  }

  // FYI: text change notification is the first notification after
  //      a user operation changes the content.  So, we need to modify
  //      the cache as far as possible here.

  if (NS_WARN_IF(!mContentCache.CacheText(this, &aIMENotification))) {
    return NS_ERROR_FAILURE;
  }

  // TabParent doesn't this this to cache.  we don't send the notification
  // if parent process doesn't request NOTIFY_TEXT_CHANGE.
  if (mIMEPreferenceOfParent.WantTextChange()) {
    mTabChild->SendNotifyIMETextChange(mContentCache, aIMENotification);
  } else {
    mTabChild->SendUpdateContentCache(mContentCache);
  }
  return NS_OK;
}

nsresult
PuppetWidget::NotifyIMEOfSelectionChange(
                const IMENotification& aIMENotification)
{
  MOZ_ASSERT(aIMENotification.mMessage == NOTIFY_IME_OF_SELECTION_CHANGE,
             "Passed wrong notification");

#ifndef MOZ_CROSS_PROCESS_IME
  return NS_OK;
#endif

  if (!mTabChild)
    return NS_ERROR_FAILURE;

  // While a plugin has focus, selection change notification shouldn't be
  // available.
  if (NS_WARN_IF(mInputContext.mIMEState.mEnabled == IMEState::PLUGIN)) {
    return NS_ERROR_FAILURE;
  }

  // Note that selection change must be notified after text change if it occurs.
  // Therefore, we don't need to query text content again here.
  mContentCache.SetSelection(
    this, 
    aIMENotification.mSelectionChangeData.mOffset,
    aIMENotification.mSelectionChangeData.Length(),
    aIMENotification.mSelectionChangeData.mReversed,
    aIMENotification.mSelectionChangeData.GetWritingMode());

  mTabChild->SendNotifyIMESelection(mContentCache, aIMENotification);

  return NS_OK;
}

nsresult
PuppetWidget::NotifyIMEOfMouseButtonEvent(
                const IMENotification& aIMENotification)
{
  if (!mTabChild) {
    return NS_ERROR_FAILURE;
  }

  // While a plugin has focus, mouse button event notification shouldn't be
  // available.
  if (NS_WARN_IF(mInputContext.mIMEState.mEnabled == IMEState::PLUGIN)) {
    return NS_ERROR_FAILURE;
  }


  bool consumedByIME = false;
  if (!mTabChild->SendNotifyIMEMouseButtonEvent(aIMENotification,
                                                &consumedByIME)) {
    return NS_ERROR_FAILURE;
  }

  return consumedByIME ? NS_SUCCESS_EVENT_CONSUMED : NS_OK;
}

nsresult
PuppetWidget::NotifyIMEOfPositionChange(const IMENotification& aIMENotification)
{
#ifndef MOZ_CROSS_PROCESS_IME
  return NS_OK;
#endif
  if (NS_WARN_IF(!mTabChild)) {
    return NS_ERROR_FAILURE;
  }

  if (NS_WARN_IF(!mContentCache.CacheEditorRect(this, &aIMENotification))) {
    return NS_ERROR_FAILURE;
  }
  // While a plugin has focus, selection range isn't available.  So, we don't
  // need to cache it at that time.
  if (mInputContext.mIMEState.mEnabled != IMEState::PLUGIN &&
      NS_WARN_IF(!mContentCache.CacheSelection(this, &aIMENotification))) {
    return NS_ERROR_FAILURE;
  }
  if (mIMEPreferenceOfParent.WantPositionChanged()) {
    mTabChild->SendNotifyIMEPositionChange(mContentCache, aIMENotification);
  } else {
    mTabChild->SendUpdateContentCache(mContentCache);
  }
  return NS_OK;
}

NS_IMETHODIMP
PuppetWidget::SetCursor(nsCursor aCursor)
{
  // Don't cache on windows, Windowless flash breaks this via async cursor updates.
#if !defined(XP_WIN)
  if (mCursor == aCursor && !mCustomCursor && !mUpdateCursor) {
    return NS_OK;
  }
#endif

  mCustomCursor = nullptr;

  if (mTabChild &&
      !mTabChild->SendSetCursor(aCursor, mUpdateCursor)) {
    return NS_ERROR_FAILURE;
  }

  mCursor = aCursor;
  mUpdateCursor = false;

  return NS_OK;
}

NS_IMETHODIMP
PuppetWidget::SetCursor(imgIContainer* aCursor,
                        uint32_t aHotspotX, uint32_t aHotspotY)
{
  if (!aCursor || !mTabChild) {
    return NS_OK;
  }

#if !defined(XP_WIN)
  if (mCustomCursor == aCursor &&
      mCursorHotspotX == aHotspotX &&
      mCursorHotspotY == aHotspotY &&
      !mUpdateCursor) {
    return NS_OK;
  }
#endif

  RefPtr<mozilla::gfx::SourceSurface> surface =
    aCursor->GetFrame(imgIContainer::FRAME_CURRENT,
                      imgIContainer::FLAG_SYNC_DECODE);
  if (!surface) {
    return NS_ERROR_FAILURE;
  }

  RefPtr<mozilla::gfx::DataSourceSurface> dataSurface =
    surface->GetDataSurface();
  if (!dataSurface) {
    return NS_ERROR_FAILURE;
  }

  size_t length;
  int32_t stride;
  mozilla::UniquePtr<char[]> surfaceData =
    nsContentUtils::GetSurfaceData(WrapNotNull(dataSurface), &length, &stride);

  nsDependentCString cursorData(surfaceData.get(), length);
  mozilla::gfx::IntSize size = dataSurface->GetSize();
  if (!mTabChild->SendSetCustomCursor(cursorData, size.width, size.height, stride,
                                      static_cast<uint8_t>(dataSurface->GetFormat()),
                                      aHotspotX, aHotspotY, mUpdateCursor)) {
    return NS_ERROR_FAILURE;
  }

  mCursor = nsCursor(-1);
  mCustomCursor = aCursor;
  mCursorHotspotX = aHotspotX;
  mCursorHotspotY = aHotspotY;
  mUpdateCursor = false;

  return NS_OK;
}

void
PuppetWidget::ClearCachedCursor()
{
  nsBaseWidget::ClearCachedCursor();
  mCustomCursor = nullptr;
}

nsresult
PuppetWidget::Paint()
{
  MOZ_ASSERT(!mDirtyRegion.IsEmpty(), "paint event logic messed up");

  if (!GetCurrentWidgetListener())
    return NS_OK;

  LayoutDeviceIntRegion region = mDirtyRegion;

  // reset repaint tracking
  mDirtyRegion.SetEmpty();
  mPaintTask.Revoke();

  RefPtr<PuppetWidget> strongThis(this);

  GetCurrentWidgetListener()->WillPaintWindow(this);

  if (GetCurrentWidgetListener()) {
#ifdef DEBUG
    debug_DumpPaintEvent(stderr, this, region.ToUnknownRegion(),
                         "PuppetWidget", 0);
#endif

    if (mozilla::layers::LayersBackend::LAYERS_CLIENT == mLayerManager->GetBackendType()) {
      // Do nothing, the compositor will handle drawing
      if (mTabChild) {
        mTabChild->NotifyPainted();
      }
    } else if (mozilla::layers::LayersBackend::LAYERS_BASIC == mLayerManager->GetBackendType()) {
      if (JihadOffscreen()) {
        JihadEnsureDrawTarget();
      }
      RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(mDrawTarget);
      if (!ctx) {
        gfxDevCrash(LogReason::InvalidContext) << "PuppetWidget context problem " << gfx::hexa(mDrawTarget);
        return NS_ERROR_FAILURE;
      }
      if (JihadOffscreen()) {
        // Jihad offscreen render: paint the FULL widget area into mDrawTarget. The
        // stock path below clips to an EMPTY rect because in the content process the
        // real pixels come from the compositor and this BASIC branch intentionally
        // draws nothing — but we bypass the compositor and need the actual pixels.
        ctx->Rectangle(gfxRect(0, 0, mBounds.width, mBounds.height));
        ctx->Clip();
      } else {
        ctx->Rectangle(gfxRect(0,0,0,0));
        ctx->Clip();
      }
      AutoLayerManagerSetup setupLayerManager(this, ctx,
                                              BufferMode::BUFFER_NONE);
      GetCurrentWidgetListener()->PaintWindow(this, region);
      if (mTabChild) {
        mTabChild->NotifyPainted();
      }
    }
  }

  if (GetCurrentWidgetListener()) {
    GetCurrentWidgetListener()->DidPaintWindow();
  }

  return NS_OK;
}

void
PuppetWidget::SetChild(PuppetWidget* aChild)
{
  MOZ_ASSERT(this != aChild, "can't parent a widget to itself");
  MOZ_ASSERT(!aChild->mChild,
             "fake widget 'hierarchy' only expected to have one level");

  mChild = aChild;
}

NS_IMETHODIMP
PuppetWidget::PaintTask::Run()
{
  if (mWidget) {
    mWidget->Paint();
  }
  return NS_OK;
}

void
PuppetWidget::PaintNowIfNeeded()
{
  if (IsVisible() && mPaintTask.IsPending()) {
    Paint();
  }
}

NS_IMPL_ISUPPORTS(PuppetWidget::MemoryPressureObserver, nsIObserver)

NS_IMETHODIMP
PuppetWidget::MemoryPressureObserver::Observe(nsISupports* aSubject,
                                              const char* aTopic,
                                              const char16_t* aData)
{
  if (!mWidget) {
    return NS_OK;
  }

  if (strcmp("memory-pressure", aTopic) == 0 &&
      !NS_LITERAL_STRING("lowering-priority").Equals(aData)) {
    if (!mWidget->mVisible && mWidget->mLayerManager &&
        XRE_IsContentProcess()) {
      mWidget->mLayerManager->ClearCachedResources();
    }
  }
  return NS_OK;
}

void
PuppetWidget::MemoryPressureObserver::Remove()
{
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->RemoveObserver(this, "memory-pressure");
  }
  mWidget = nullptr;
}

bool
PuppetWidget::NeedsPaint()
{
  return mVisible;
}

float
PuppetWidget::GetDPI()
{
  if (mDPI < 0) {
    if (mTabChild) {
      mTabChild->GetDPI(&mDPI);
    } else {
      mDPI = 96.0;
    }
  }

  return mDPI;
}

double
PuppetWidget::GetDefaultScaleInternal()
{
  if (mDefaultScale < 0) {
    if (mTabChild) {
      mTabChild->GetDefaultScale(&mDefaultScale);
    } else {
      mDefaultScale = 1;
    }
  }

  return mDefaultScale;
}

int32_t
PuppetWidget::RoundsWidgetCoordinatesTo()
{
  if (mRounding < 0) {
    if (mTabChild) {
      mTabChild->GetWidgetRounding(&mRounding);
    } else {
      mRounding = 1;
    }
  }

  return mRounding;
}

void*
PuppetWidget::GetNativeData(uint32_t aDataType)
{
  switch (aDataType) {
  case NS_NATIVE_SHAREABLE_WINDOW: {
    MOZ_ASSERT(mTabChild, "Need TabChild to get the nativeWindow from!");
    mozilla::WindowsHandle nativeData = 0;
    if (mTabChild) {
      mTabChild->SendGetWidgetNativeData(&nativeData);
    }
    return (void*)nativeData;
  }
  case NS_NATIVE_WINDOW:
  case NS_NATIVE_WIDGET:
  case NS_NATIVE_DISPLAY:
    // These types are ignored (see bug 1183828, bug 1240891).
    break;
  case NS_RAW_NATIVE_IME_CONTEXT:
    MOZ_CRASH("You need to call GetNativeIMEContext() instead");
  case NS_NATIVE_PLUGIN_PORT:
  case NS_NATIVE_GRAPHIC:
  case NS_NATIVE_SHELLWIDGET:
  default:
    NS_WARNING("nsWindow::GetNativeData called with bad value");
    break;
  }
  return nullptr;
}

#if defined(XP_WIN)
void
PuppetWidget::SetNativeData(uint32_t aDataType, uintptr_t aVal)
{
  switch (aDataType) {
  case NS_NATIVE_CHILD_OF_SHAREABLE_WINDOW:
    MOZ_ASSERT(mTabChild, "Need TabChild to send the message.");
    if (mTabChild) {
      mTabChild->SendSetNativeChildOfShareableWindow(aVal);
    }
    break;
  default:
    NS_WARNING("SetNativeData called with unsupported data type.");
  }
}
#endif

nsIntPoint
PuppetWidget::GetChromeDimensions()
{
  if (!GetOwningTabChild()) {
    NS_WARNING("PuppetWidget without Tab does not have chrome information.");
    return nsIntPoint();
  }
  return GetOwningTabChild()->GetChromeDisplacement().ToUnknownPoint();
}

nsIntPoint
PuppetWidget::GetWindowPosition()
{
  if (!GetOwningTabChild()) {
    return nsIntPoint();
  }

  int32_t winX, winY, winW, winH;
  NS_ENSURE_SUCCESS(GetOwningTabChild()->GetDimensions(0, &winX, &winY, &winW, &winH), nsIntPoint());
  return nsIntPoint(winX, winY) + GetOwningTabChild()->GetClientOffset().ToUnknownPoint();
}

LayoutDeviceIntRect
PuppetWidget::GetScreenBounds()
{
  return LayoutDeviceIntRect(WidgetToScreenOffset(), mBounds.Size());
}

uint32_t PuppetWidget::GetMaxTouchPoints() const
{
  static uint32_t sTouchPoints = 0;
  static bool sIsInitialized = false;
  if (sIsInitialized) {
    return sTouchPoints;
  }
  if (mTabChild) {
    mTabChild->GetMaxTouchPoints(&sTouchPoints);
    sIsInitialized = true;
  }
  return sTouchPoints;
}

void
PuppetWidget::GetPointerCapabilities(PointerCapabilities& aCaps) const
{
  WidgetUtils::GetPointerCapabilities(aCaps);
}

void
PuppetWidget::StartAsyncScrollbarDrag(const AsyncDragMetrics& aDragMetrics)
{
  mTabChild->StartScrollbarDrag(aDragMetrics);
}

PuppetScreen::PuppetScreen(void *nativeScreen)
{
}

PuppetScreen::~PuppetScreen()
{
}

static ScreenConfiguration
ScreenConfig()
{
  ScreenConfiguration config;
  hal::GetCurrentScreenConfiguration(&config);
  return config;
}

nsIntSize
PuppetWidget::GetScreenDimensions()
{
  nsIntRect r = ScreenConfig().rect();
  return nsIntSize(r.width, r.height);
}

NS_IMETHODIMP
PuppetScreen::GetId(uint32_t *outId)
{
  *outId = 1;
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreen::GetRect(int32_t *outLeft,  int32_t *outTop,
                      int32_t *outWidth, int32_t *outHeight)
{
  if (PuppetWidget::JihadOffscreen()) {
    // Answer directly from the stored offscreen dimensions. Going through
    // ScreenConfig()/hal here would recurse back into the fallback hal, which
    // queries this same screen manager, overflowing the stack.
    *outLeft = 0;
    *outTop = 0;
    *outWidth = sJihadScreenWidth;
    *outHeight = sJihadScreenHeight;
    return NS_OK;
  }
  nsIntRect r = ScreenConfig().rect();
  *outLeft = r.x;
  *outTop = r.y;
  *outWidth = r.width;
  *outHeight = r.height;
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreen::GetAvailRect(int32_t *outLeft,  int32_t *outTop,
                           int32_t *outWidth, int32_t *outHeight)
{
  return GetRect(outLeft, outTop, outWidth, outHeight);
}

NS_IMETHODIMP
PuppetScreen::GetPixelDepth(int32_t *aPixelDepth)
{
  if (PuppetWidget::JihadOffscreen()) {
    // Fixed depth; ScreenConfig()/hal would recurse back into this object.
    *aPixelDepth = 24;
    return NS_OK;
  }
  *aPixelDepth = ScreenConfig().pixelDepth();
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreen::GetColorDepth(int32_t *aColorDepth)
{
  if (PuppetWidget::JihadOffscreen()) {
    *aColorDepth = 24;
    return NS_OK;
  }
  *aColorDepth = ScreenConfig().colorDepth();
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreen::GetRotation(uint32_t* aRotation)
{
  NS_WARNING("Attempt to get screen rotation through nsIScreen::GetRotation().  Nothing should know or care this in sandboxed contexts.  If you want *orientation*, use hal.");
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
PuppetScreen::SetRotation(uint32_t aRotation)
{
  NS_WARNING("Attempt to set screen rotation through nsIScreen::GetRotation().  Nothing should know or care this in sandboxed contexts.  If you want *orientation*, use hal.");
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMPL_ISUPPORTS(PuppetScreenManager, nsIScreenManager)

PuppetScreenManager::PuppetScreenManager()
{
    mOneScreen = new PuppetScreen(nullptr);
}

PuppetScreenManager::~PuppetScreenManager()
{
}

NS_IMETHODIMP
PuppetScreenManager::ScreenForId(uint32_t aId,
                                 nsIScreen** outScreen)
{
  NS_IF_ADDREF(*outScreen = mOneScreen.get());
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreenManager::GetPrimaryScreen(nsIScreen** outScreen)
{
  NS_IF_ADDREF(*outScreen = mOneScreen.get());
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreenManager::ScreenForRect(int32_t inLeft,
                                   int32_t inTop,
                                   int32_t inWidth,
                                   int32_t inHeight,
                                   nsIScreen** outScreen)
{
  return GetPrimaryScreen(outScreen);
}

NS_IMETHODIMP
PuppetScreenManager::ScreenForNativeWidget(void* aWidget,
                                           nsIScreen** outScreen)
{
  return GetPrimaryScreen(outScreen);
}

NS_IMETHODIMP
PuppetScreenManager::GetNumberOfScreens(uint32_t* aNumberOfScreens)
{
  *aNumberOfScreens = 1;
  return NS_OK;
}

NS_IMETHODIMP
PuppetScreenManager::GetSystemDefaultScale(float *aDefaultScale)
{
  *aDefaultScale = 1.0f;
  return NS_OK;
}

nsIWidgetListener*
PuppetWidget::GetCurrentWidgetListener()
{
  // JIHAD: fall back to mWidgetListener when nothing is ATTACHED, exactly as the real
  // widget does (widget/gtk/nsWindow.cpp GetListener:
  // `mAttachedWidgetListener ? mAttachedWidgetListener : mWidgetListener`). PuppetWidget
  // was written for the content-process case, where the view is always attached to a
  // top-level widget, so it only ever consulted the attached listener — and in this
  // standalone offscreen embedding nothing attaches, leaving mAttachedWidgetListener null.
  //
  // Everything routed through DispatchEvent was therefore silently DROPPED. Mouse input
  // hid the problem because nsIDOMWindowUtils has `…SendMouseEventToWindow`, which goes to
  // the presShell directly; there is no `SendKeyEventToWindow`, so SendKeyEvent went
  // widget->DispatchEvent and vanished. That is why text could only be inserted through
  // the editor (SetValue/SetSelectionRange) and why anything reacting to real key events —
  // XUL <key> elements, tree type-ahead, Escape/Enter handling — got nothing at all.
  if (!mAttachedWidgetListener) {
    return mWidgetListener;
  }
  if (!mPreviouslyAttachedWidgetListener) {
    return mAttachedWidgetListener;
  }

  if (mAttachedWidgetListener->GetView()->IsPrimaryFramePaintSuppressed()) {
    return mPreviouslyAttachedWidgetListener;
  }

  return mAttachedWidgetListener;
}

void
PuppetWidget::SetCandidateWindowForPlugin(
                const CandidateWindowPosition& aPosition)
{
  if (!mTabChild) {
    return;
  }

  mTabChild->SendSetCandidateWindowForPlugin(aPosition);
}

void
PuppetWidget::ZoomToRect(const uint32_t& aPresShellId,
                         const FrameMetrics::ViewID& aViewId,
                         const CSSRect& aRect,
                         const uint32_t& aFlags)
{
  if (!mTabChild) {
    return;
  }

  mTabChild->ZoomToRect(aPresShellId, aViewId, aRect, aFlags);
}

void
PuppetWidget::LookUpDictionary(
                const nsAString& aText,
                const nsTArray<mozilla::FontRange>& aFontRangeArray,
                const bool aIsVertical,
                const LayoutDeviceIntPoint& aPoint)
{
  if (!mTabChild) {
    return;
  }

  mTabChild->SendLookUpDictionary(nsString(aText), aFontRangeArray, aIsVertical, aPoint);
}

bool
PuppetWidget::HasPendingInputEvent()
{
  if (!mTabChild) {
    return false;
  }

  bool ret = false;

  mTabChild->GetIPCChannel()->PeekMessages(
    [&ret](const IPC::Message& aMsg) -> bool {
      if ((aMsg.type() & mozilla::dom::PBrowser::PBrowserStart)
          == mozilla::dom::PBrowser::PBrowserStart) {
        switch (aMsg.type()) {
          case mozilla::dom::PBrowser::Msg_RealMouseMoveEvent__ID:
          case mozilla::dom::PBrowser::Msg_SynthMouseMoveEvent__ID:
          case mozilla::dom::PBrowser::Msg_RealMouseButtonEvent__ID:
          case mozilla::dom::PBrowser::Msg_RealKeyEvent__ID:
          case mozilla::dom::PBrowser::Msg_MouseWheelEvent__ID:
          case mozilla::dom::PBrowser::Msg_RealTouchEvent__ID:
          case mozilla::dom::PBrowser::Msg_RealTouchMoveEvent__ID:
          case mozilla::dom::PBrowser::Msg_RealDragEvent__ID:
          case mozilla::dom::PBrowser::Msg_UpdateDimensions__ID:
          case mozilla::dom::PBrowser::Msg_MouseEvent__ID:
          case mozilla::dom::PBrowser::Msg_KeyEvent__ID:
            ret = true;
            return false;  // Stop peeking.
        }
      }
      return true;
    }
  );

  return ret;
}

void
PuppetWidget::HandledWindowedPluginKeyEvent(
                const NativeEventData& aKeyEventData,
                bool aIsConsumed)
{
  if (NS_WARN_IF(mKeyEventInPluginCallbacks.IsEmpty())) {
    return;
  }
  nsCOMPtr<nsIKeyEventInPluginCallback> callback =
    mKeyEventInPluginCallbacks[0];
  MOZ_ASSERT(callback);
  mKeyEventInPluginCallbacks.RemoveElementAt(0);
  callback->HandledWindowedPluginKeyEvent(aKeyEventData, aIsConsumed);
}

nsresult
PuppetWidget::OnWindowedPluginKeyEvent(const NativeEventData& aKeyEventData,
                                       nsIKeyEventInPluginCallback* aCallback)
{
  if (NS_WARN_IF(!mTabChild)) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  if (NS_WARN_IF(!mTabChild->SendOnWindowedPluginKeyEvent(aKeyEventData))) {
    return NS_ERROR_FAILURE;
  }
  mKeyEventInPluginCallbacks.AppendElement(aCallback);
  return NS_SUCCESS_EVENT_HANDLED_ASYNCHRONOUSLY;
}

} // namespace widget
} // namespace mozilla
