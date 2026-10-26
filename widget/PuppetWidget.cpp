/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <unistd.h>   // Jihad T-502: write() to the done fd
#include "mozilla/Maybe.h"   // Jihad T-515
#include <sys/time.h>        // Jihad T-515: the timeline's clock
#include <atomic>            // Jihad T-517
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
#include "nsViewManager.h"        // T-502: the root view for PresShell::Paint
#include "mozilla/TimeStamp.h"
#include "mozilla/layers/CompositorBridgeParent.h"   // T-502
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/CompositorSession.h"
#include "mozilla/Monitor.h"
#include "nsPIDOMWindow.h"   // T-502 device rounds
#include "nsIDocument.h"   // T-502 device rounds
#include "mozilla/layers/CompositorOGL.h"   // T-502: JihadFrameRequest

#include "mozilla/gfx/JihadGpuLog.h"   // Jihad (T-512): JIHAD_GPU_LOG
namespace mozilla { namespace gl { extern std::atomic<uint32_t> gJihadUploads; extern std::atomic<uint64_t> gJihadUploadUs; } }   // T-517, GLUploadHelpers.cpp
namespace mozilla { namespace layers { extern std::atomic<uint32_t> gJihadLateWrites; } }   // T-513, CompositorOGL.cpp
#include "base/task.h"
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
#include "nsRefreshDriver.h"       // T-527: a paint already scheduled (JihadPaintLayers)
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
  JihadComposeAbandonAll();   // T-502
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
    mJihadSharedLayerManager = true;
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
  // Jihad T-502: a child widget shares the parent's layer manager (Create above). Destroying
  // it here left the parent with a ClientLayerManager whose widget was null, and the next
  // compose crashed in BeginTransactionWithTarget (the view re-creates its child widget on a
  // size change). The parent destroys it; the child only drops its reference.
  if (mLayerManager && !mJihadSharedLayerManager) {
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
      if (JihadGpuCompositor() && !mJihadCompositorTried && !mTabChild &&
          gfxPlatform::UsesOffMainThreadCompositing()) {
        // Jihad T-502: an in-process compositor behind a ClientLayerManager. Tried once: on
        // failure CreateCompositor leaves mLayerManager null and the software path below
        // takes over, so a device whose EGL refuses still paints.
        mJihadCompositorTried = true;
        CreateCompositor(mBounds.width, mBounds.height);
        JIHAD_GPU_LOG("[jihad-gpu] compositor %s for %dx%d (compositor backend %d)\n",
                mLayerManager ? "created" : "NOT created: software layers",
                mBounds.width, mBounds.height,
                mLayerManager ? (int)mLayerManager->GetCompositorBackendType() : -1);
      }
      if (!mLayerManager) {
        if (JihadGpuCompositor() && !mJihadCompositorTried) {
          JIHAD_GPU_LOG("[jihad-gpu] compositor not attempted: OMTC %s, TabChild %p\n",
                  gfxPlatform::UsesOffMainThreadCompositing() ? "on" : "off", (void*)mTabChild);
        }
        // Jihad: no compositor/IPC — paint on this thread with a BasicLayerManager
        // into mDrawTarget (see PuppetWidget::Paint LAYERS_BASIC branch).
        mLayerManager = CreateBasicLayerManager();
      }
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
/* static */ bool
PuppetWidget::JihadGpuCompositor()
{
  static const bool sOn = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
  return sOn;
}

} // namespace widget
} // namespace mozilla
// T-502: one frame pulled from the compositor thread. Heap-allocated and owned by whichever side
// finishes last, so a waiter that gives up (abandoned) leaves nothing dangling for the task.
// Global, as PuppetWidget.h forward-declares it.
struct JihadReadJob {
  mozilla::layers::CompositorBridgeParent* bridge;
  uint8_t* dest; int stride, w, h; float scale, offX, offY;
  int rowLo, rowHi;      // the rows the daemon asked for, top-down ((0,0) = none)
  int row2Lo = 0, row2Hi = 0;   // a second band asked for
  int shift = 0;                // rows the destination is shifted by before the read
  const uint8_t* src = nullptr; // the frame the kept rows come from (round 113), or dest itself
  int srcY = 0; bool engineShift = false;   // round 115
  bool verify = false;   // T-503
  bool colsKnown = false; int colX0 = 0, colX1 = 0;   // T-511: the bands' stale columns (flag 4)
  bool windowMoved = false;   // T-517 (flag 8): the shift is the daemon's window move, the rows in place
  bool bandsOnly = false;     // T-516 (flag 16): only the bands asked for are read
  bool overview = false;      // T-504 (flag 32): a window of the daemon's overview
  bool overviewParts = false; // T-504 (flag 64): the overview's fixed and stuck parts alone
  int invLo, invHi;      // the rows the composite changed
  float scrollX = 0.0f, scrollY = 0.0f; bool scrollValid = false;   // the card's scroll for the compositor
  uint32_t token;
  mozilla::TimeStamp posted;   // device rounds: the compositor loop's queue delay
  mozilla::layers::JihadFrameRequest req;   // what the compositor and the read thread work on
  mozilla::Monitor mon;
  bool done, ok, abandoned;
  JihadReadJob(mozilla::layers::CompositorBridgeParent* b, uint8_t* d, int st, int wi, int he, float sc, float ox, float oy, int lo, int hi, uint32_t t)
    : bridge(b), dest(d), stride(st), w(wi), h(he), scale(sc), offX(ox), offY(oy), rowLo(lo), rowHi(hi), invLo(0), invHi(0), token(t),
      mon("jihad-compose"), done(false), ok(false), abandoned(false) {}
};
namespace mozilla {
namespace widget {
// T-502 round 108: a byte to this fd when a compose job finishes (jihad_offscreen_set_done_fd),
// so the daemon's loop takes the frame at once instead of at its next tick
static int sJihadDoneFd = -1;
namespace {
// the read thread (or the compositor thread, when the read could not be handed off) finished
void JihadJobFinish(JihadReadJob* job, bool ok)
{
  bool abandoned;
  {
    mozilla::MonitorAutoLock lock(job->mon);
    job->ok = ok; job->invLo = job->req.invLo; job->invHi = job->req.invHi; job->done = true; abandoned = job->abandoned;
    job->mon.Notify();
  }
  if (sJihadDoneFd >= 0) {
    const char c = 1;
    ssize_t r = write(sJihadDoneFd, &c, 1);   // non-blocking; a full pipe already wakes the loop
    (void)r;
  }
  if (abandoned) {
    delete job;
  }
}
void JihadJobDone(mozilla::layers::JihadFrameRequest* req)
{
  JihadJobFinish(static_cast<JihadReadJob*>(req->cookie), req->ok);
}
void JihadReadOnCompositorThread(JihadReadJob* job)
{
  const mozilla::TimeStamp t0 = mozilla::TimeStamp::Now();
  job->req.dest = job->dest; job->req.stride = job->stride; job->req.w = job->w; job->req.h = job->h;
  job->req.lo = job->rowLo; job->req.hi = job->rowHi; job->req.lo2 = job->row2Lo; job->req.hi2 = job->row2Hi;
  job->req.shift = job->shift; job->req.src = job->src; job->req.srcY = job->srcY; job->req.engineShift = job->engineShift; job->req.verify = job->verify;
  job->req.colsKnown = job->colsKnown; job->req.colX0 = job->colX0; job->req.colX1 = job->colX1;
  job->req.windowMoved = job->windowMoved;
  job->req.bandsOnly = job->bandsOnly;
  job->req.overview = job->overview;
  job->req.overviewParts = job->overviewParts;
  job->req.scrollX = job->scrollX; job->req.scrollY = job->scrollY; job->req.scrollValid = job->scrollValid;
  job->req.done = &JihadJobDone; job->req.cookie = job;
  // Once done() has run the daemon may forget (delete) the job at any moment (round 108's wake
  // pipe made that immediate): nothing below may touch it unless the compose never started.
  const uint32_t token = job->token;
  const double queuedMs = (t0 - job->posted).ToMilliseconds();
  const uint32_t up0 = mozilla::gl::gJihadUploads;
  const uint64_t upUs0 = mozilla::gl::gJihadUploadUs;
  const bool started = job->bridge->JihadComposeAndRead(&job->req, job->scale, job->offX, job->offY);
  static uint32_t sN = 0;
  static const bool sTimeline = getenv("JIHAD_GPU_TIMELINE") != nullptr;   // T-515 device rounds: every job, on the daemon's clock
  if (sTimeline) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    fprintf(stderr, "[jihad-tl] t=%lld job %u: queued %.1f ms on the compositor loop, composed in %.1f ms (%u uploads %.1f)\n",
            (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000, token, queuedMs, (mozilla::TimeStamp::Now() - t0).ToMilliseconds(),
            mozilla::gl::gJihadUploads - up0, (mozilla::gl::gJihadUploadUs - upUs0) / 1000.0);
  } else if (sN++ < 30 || (sN % 50) == 0) {
    JIHAD_GPU_LOG("[jihad-gpu] job %u: queued %.1f ms on the compositor loop, composed in %.1f ms\n", token,
            queuedMs, (mozilla::TimeStamp::Now() - t0).ToMilliseconds());
  }
  if (!started) {
    JihadJobFinish(job, false);
  }
}
} // namespace
mozilla::layers::CompositorBridgeParent*
PuppetWidget::JihadBridge()
{
  return mCompositorSession ? mCompositorSession->GetInProcessBridge() : nullptr;
}

uint32_t
PuppetWidget::JihadComposeStart(nsIDocShell* aDocShell, void* aDest, int aStride,
                                int aWidth, int aHeight, int aRowLo, int aRowHi, int aRow2Lo, int aRow2Hi, int aShift,
                                const void* aSrc, int aSrcY, int aFlags, int aColX0, int aColX1)
{
  if (!aDocShell || !aDest || aWidth <= 0 || aHeight <= 0 || aStride < aWidth * 4) {
    return 0;
  }
  if (!JihadPaintLayers(aDocShell)) {
    return 0;
  }
  return JihadPostJob(aDest, aStride, aWidth, aHeight, aRowLo, aRowHi, aRow2Lo, aRow2Hi, aShift, aSrc, aSrcY, aFlags,
                      nullptr, false, aColX0, aColX1);
}

bool
PuppetWidget::JihadPaintLayers(nsIDocShell* aDocShell)
{
  if (!aDocShell) {
    return false;
  }
  nsCOMPtr<nsIPresShell> presShell = aDocShell->GetPresShell();
  if (!presShell) {
    return false;
  }
  presShell->FlushPendingNotifications(Flush_Layout);
  nsViewManager* vm = presShell->GetViewManager();
  nsView* root = vm ? vm->GetRootView() : nullptr;
  nsIWidget* viewWidget = root ? root->GetWidget() : nullptr;
  if (!viewWidget) {
    return false;
  }
  LayerManager* lm = viewWidget->GetLayerManager();
  ClientLayerManager* clm = lm ? lm->AsClientLayerManager() : nullptr;
  mozilla::layers::CompositorBridgeParent* bridge = JihadBridge();
  if (!clm || !bridge) {
    return false;
  }
  if (!mozilla::layers::CompositorThreadHolder::Loop()) {
    return false;
  }
  // T-527: nothing to paint — no invalidation waiting (the widget's region, its paint task), no paint
  // the refresh driver has scheduled, the layers painted before: the layer tree is current, and a paint
  // would only build the whole display list again (desktop Wikipedia: 16-90 ms of the main thread; the
  // daemon asks for a layer paint every 40 ms during a pan and right after the refresh driver's own
  // paint of a zoom). JIHAD_GPU_PAINT_ALWAYS=1 paints every time, as before.
  {
    static const bool sAlways = getenv("JIHAD_GPU_PAINT_ALWAYS") != nullptr;
    nsPresContext* pc = presShell->GetPresContext();
    nsRefreshDriver* rd = pc ? pc->RefreshDriver() : nullptr;
    // the invalidations land on the child widget (Invalidate forwards to mChild): the whole chain is
    // asked, as JihadTakeDirty drains it (review 2026-10-26)
    bool pending = false;
    for (PuppetWidget* w = this; w && !pending; w = w->mChild.get()) {
      pending = !w->mDirtyRegion.IsEmpty() || w->mPaintTask.IsPending();
    }
    // a new resolution schedules no paint by itself (SetResolution): on a page with no root scroll frame
    // (about:addons) nothing else does, and a zoom across 1.1 stayed painted at the old one (review 2026-10-26)
    const float res = presShell->GetResolution();
    if (!sAlways && mJihadLayersPainted && !pending && res == mJihadPaintedRes && rd &&
        !rd->ViewManagerFlushIsPending()) {
      static uint32_t sSkipped = 0;
      if ((++sSkipped % 100) == 1) {
        JIHAD_GPU_LOG("[jihad-gpu] layer paint skipped: nothing pending (%u so far)\n", sSkipped);
      }
      return true;
    }
  }
  mJihadLayersPainted = true;
  mJihadPaintedRes = presShell->GetResolution();
  mozilla::TimeStamp t0 = mozilla::TimeStamp::Now();
  nsRegion dirty(root->GetDimensions());
  presShell->Paint(root, dirty, nsIPresShell::PAINT_LAYERS);
  mozilla::TimeStamp t1 = mozilla::TimeStamp::Now();
  static uint32_t sFrames = 0;
  if (sFrames < 20 || (sFrames % 50) == 0) {
    JIHAD_GPU_LOG("[jihad-gpu] frame %u: layers %.1f ms\n", sFrames, (t1 - t0).ToMilliseconds());
  }
  if (sFrames == 5 || (sFrames % 200) == 0) {
    // T-502 device rounds: every flag that throttles the page's JavaScript (timers to a second,
    // requestAnimationFrame to a frame a second)
    nsIDocument* doc = presShell->GetDocument();
    bool dsActive = false;
    aDocShell->GetIsActive(&dsActive);
    nsIFrame* rootFrame = presShell->GetRootFrame();
    nsIFrame* displayRoot = rootFrame ? nsLayoutUtils::GetDisplayRootFrame(rootFrame) : nullptr;
    nsPIDOMWindowOuter* win = aDocShell->GetWindow();
    JIHAD_GPU_LOG("[jihad-gpu] activity: docshell %s, pres shell %s, document %s, frame requests %s, painted-pres-shell %d, window %s\n",
            dsActive ? "active" : "INACTIVE", presShell->IsActive() ? "active" : "INACTIVE",
            doc && doc->Hidden() ? "HIDDEN" : "visible",
            doc && doc->ShouldThrottleFrameRequests() ? "THROTTLED" : "full rate",
            (int)(displayRoot && displayRoot->DidPaintPresShell(presShell)),
            win && win->IsBackground() ? "BACKGROUND" : "foreground");
  }
  ++sFrames;
  return true;
}

// T-515: a job posted to the compositor thread, from the main thread (after its paint) or from the
// express lane (no paint); the list is shared, so under its lock
uint32_t
PuppetWidget::JihadPostJob(void* aDest, int aStride, int aWidth, int aHeight, int aRowLo, int aRowHi, int aRow2Lo,
                           int aRow2Hi, int aShift, const void* aSrc, int aSrcY, int aFlags,
                           const float* aView, bool aScrollValid, int aColX0, int aColX1)
{
  mozilla::layers::CompositorBridgeParent* bridge = JihadBridge();
  MessageLoop* loop = mozilla::layers::CompositorThreadHolder::Loop();
  if (!bridge || !loop || !aDest) {
    return 0;
  }
  mozilla::MutexAutoLock listLock(mJihadJobsLock);
  uint32_t token = mJihadNextToken++;
  if (mJihadNextToken == 0) {
    mJihadNextToken = 1;
  }
  JihadReadJob* job = new JihadReadJob(bridge, static_cast<uint8_t*>(aDest), aStride, aWidth, aHeight,
                                       aView ? (aView[0] > 0.0f ? aView[0] : 1.0f) : mJihadScale,
                                       aView ? aView[1] : mJihadOffX, aView ? aView[2] : mJihadOffY, aRowLo, aRowHi, token);
  job->scrollX = aView ? aView[3] : mJihadScrollX; job->scrollY = aView ? aView[4] : mJihadScrollY;
  job->scrollValid = aView ? aScrollValid : mJihadScrollValid;
  job->row2Lo = aRow2Lo; job->row2Hi = aRow2Hi; job->shift = aShift; job->src = static_cast<const uint8_t*>(aSrc); job->srcY = aSrcY;
  job->engineShift = (aFlags & 1) != 0; job->verify = (aFlags & 2) != 0;
  job->colsKnown = (aFlags & 4) != 0;   // T-511: the column arguments are given (else garbage from an older daemon)
  job->windowMoved = (aFlags & 8) != 0;   // T-517
  job->bandsOnly = (aFlags & 16) != 0;    // T-516
  job->overview = (aFlags & 32) != 0;     // T-504
  job->overviewParts = (aFlags & 64) != 0;
  if (job->colsKnown) {
    job->colX0 = std::max(0, std::min(aColX0, aWidth)); job->colX1 = std::max(0, std::min(aColX1, aWidth));
  }
  job->posted = mozilla::TimeStamp::Now();
  mJihadJobs.push_back(job);
  loop->PostTask(NewRunnableFunction(&JihadReadOnCompositorThread, job));
  return token;
}

uint32_t
PuppetWidget::JihadComposePost(void* aDest, int aStride, int aWidth, int aHeight, int aRowLo, int aRowHi,
                               int aRow2Lo, int aRow2Hi, int aShift, const void* aSrc, int aSrcY, int aFlags,
                               const float* aView, bool aScrollValid, int aColX0, int aColX1)
{
  if (!aDest || aWidth <= 0 || aHeight <= 0 || aStride < aWidth * 4) {
    return 0;
  }
  return JihadPostJob(aDest, aStride, aWidth, aHeight, aRowLo, aRowHi, aRow2Lo, aRow2Hi, aShift, aSrc, aSrcY, aFlags,
                      aView, aScrollValid, aColX0, aColX1);
}

int
PuppetWidget::JihadComposePoll(uint32_t aToken, bool aWait, int* aOut, bool* aScrollOnly, int aOutLen)
{
  // T-515: the job list is shared with the express lane. Without a wait the whole poll holds the
  // list's lock: a forget from another thread between the find and the job's monitor would let the
  // compositor task free the job under it. A wait (the synchronous compose, the main thread's own
  // token) releases it while waiting.
  mozilla::Maybe<mozilla::MutexAutoLock> heldLock;
  if (!aWait) {
    heldLock.emplace(mJihadJobsLock);
  }
  JihadReadJob* job = nullptr;
  {
    mozilla::Maybe<mozilla::MutexAutoLock> findLock;
    if (aWait) {
      findLock.emplace(mJihadJobsLock);
    }
    for (size_t i = 0; i < mJihadJobs.size(); ++i) {
      if (mJihadJobs[i]->token == aToken) { job = mJihadJobs[i]; break; }
    }
  }
  if (!job) {
    return -1;
  }
  auto removeJob = [&]() {
    mozilla::Maybe<mozilla::MutexAutoLock> eraseLock;
    if (aWait) {
      eraseLock.emplace(mJihadJobsLock);
    }
    for (size_t i = 0; i < mJihadJobs.size(); ++i) {
      if (mJihadJobs[i] == job) { mJihadJobs.erase(mJihadJobs.begin() + i); break; }
    }
  };
  bool finished = false, ok = false;
  {
    mozilla::MonitorAutoLock lock(job->mon);
    if (aWait) {
      const mozilla::TimeStamp deadline = mozilla::TimeStamp::Now() + mozilla::TimeDuration::FromSeconds(3);
      while (!job->done) {
        mozilla::TimeDuration left = deadline - mozilla::TimeStamp::Now();
        if (left <= mozilla::TimeDuration()) {
          break;
        }
        lock.Wait(PR_MillisecondsToInterval((uint32_t)left.ToMilliseconds()));
      }
    }
    if (job->done) {
      finished = true; ok = job->ok;
      if (aOut) {
        aOut[0] = job->invLo; aOut[1] = job->invHi;
        aOut[2] = job->req.fixedLo; aOut[3] = job->req.fixedHi;
        aOut[4] = job->req.lo; aOut[5] = job->req.hi;
        aOut[6] = job->req.lo2; aOut[7] = job->req.hi2;
        aOut[8] = job->req.frameY; aOut[9] = job->req.frameYValid ? 1 : 0;
        if (job->req.colsKnown) { aOut[10] = job->req.invX0; aOut[11] = job->req.invX1; }   // T-511: twelve ints
        if (aOutLen >= 12 + 1 + 16) {   // T-544: the stuck rects, a count and four ints each
          aOut[12] = job->req.nStuck;
          for (int k = 0; k < job->req.nStuck && k < 4; k++)
            for (int c = 0; c < 4; c++) aOut[13 + 4 * k + c] = job->req.stuck[k][c];
        }
      }
      if (aScrollOnly) *aScrollOnly = job->req.scrollOnly;
    } else if (!aWait && job->req.composed) {
      // composed: the next frame may be started, and it must read what this one changed (round 116)
      if (aOut) {
        aOut[0] = job->req.invLo; aOut[1] = job->req.invHi; aOut[8] = job->req.frameY; aOut[9] = job->req.frameYValid ? 1 : 0;
        if (job->req.colsKnown) { aOut[10] = job->req.invX0; aOut[11] = job->req.invX1; }
      }
      return 2;
    } else if (aWait) {
      job->abandoned = true; job->req.cancelled = true;
      fprintf(stderr, "[jihad-gpu] compose: no answer from the compositor thread in 3 s\n");
    }
  }
  if (finished) {
    removeJob();
    delete job;
    return ok ? 1 : -1;
  }
  if (aWait) {
    removeJob();   // abandoned: the task frees it
    return -1;
  }
  return 0;
}

int
PuppetWidget::JihadComposePeek(uint32_t aToken)
{
  mozilla::MutexAutoLock listLock(mJihadJobsLock);
  for (size_t i = 0; i < mJihadJobs.size(); ++i) {
    JihadReadJob* job = mJihadJobs[i];
    if (job->token != aToken) {
      continue;
    }
    mozilla::MonitorAutoLock lock(job->mon);
    return job->done ? 1 : (job->req.composed ? 2 : 0);
  }
  return -1;
}

void
PuppetWidget::JihadComposeForget(uint32_t aToken)
{
  mozilla::MutexAutoLock listLock(mJihadJobsLock);   // T-515
  for (size_t i = 0; i < mJihadJobs.size(); ++i) {
    JihadReadJob* job = mJihadJobs[i];
    if (job->token != aToken) {
      continue;
    }
    bool done;
    {
      mozilla::MonitorAutoLock lock(job->mon);
      done = job->done;
      if (!done) { job->abandoned = true; job->req.cancelled = true; }
    }
    mJihadJobs.erase(mJihadJobs.begin() + i);
    if (done) delete job;
    return;
  }
}

void
PuppetWidget::JihadComposeAbandonAll()
{
  mozilla::MutexAutoLock listLock(mJihadJobsLock);   // T-515
  for (size_t i = 0; i < mJihadJobs.size(); ++i) {
    JihadReadJob* job = mJihadJobs[i];
    bool done;
    {
      mozilla::MonitorAutoLock lock(job->mon);
      done = job->done;
      if (!done) { job->abandoned = true; job->req.cancelled = true; }
    }
    if (done) delete job;
  }
  mJihadJobs.clear();
}

bool
PuppetWidget::JihadComposeTo(nsIDocShell* aDocShell, void* aDest, int aStride,
                             int aWidth, int aHeight)
{
  static const bool sSnapshot = getenv("JIHAD_GPU_SNAPSHOT") != nullptr;
  if (sSnapshot) {
    // the round-5 snapshot road, kept for comparison
    if (!aDocShell || !aDest || aWidth <= 0 || aHeight <= 0 || aStride < aWidth * 4) {
      return false;
    }
    nsCOMPtr<nsIPresShell> presShell = aDocShell->GetPresShell();
    if (!presShell) {
      return false;
    }
    presShell->FlushPendingNotifications(Flush_Layout);
    nsViewManager* vm = presShell->GetViewManager();
    nsView* root = vm ? vm->GetRootView() : nullptr;
    nsIWidget* viewWidget = root ? root->GetWidget() : nullptr;
    if (!viewWidget) {
      return false;
    }
    LayerManager* lm = viewWidget->GetLayerManager();
    ClientLayerManager* clm = lm ? lm->AsClientLayerManager() : nullptr;
    if (!clm) {
      return false;
    }
    mozilla::TimeStamp t0 = mozilla::TimeStamp::Now();
    nsRegion dirty(root->GetDimensions());
    presShell->Paint(root, dirty, nsIPresShell::PAINT_LAYERS);
    mozilla::TimeStamp t1 = mozilla::TimeStamp::Now();
    RefPtr<DrawTarget> dt = Factory::CreateDrawTargetForData(
        BackendType::CAIRO, static_cast<unsigned char*>(aDest),
        IntSize(aWidth, aHeight), aStride, SurfaceFormat::B8G8R8A8);
    if (!dt || !dt->IsValid()) {
      return false;
    }
    dt->FillRect(mozilla::gfx::Rect(0, 0, aWidth, aHeight),
                 mozilla::gfx::ColorPattern(mozilla::gfx::Color(1.f, 1.f, 1.f, 1.f)));
    RefPtr<gfxContext> ctx = gfxContext::CreateOrNull(dt);
    if (!ctx) {
      return false;
    }
    bool okSnap = false;
    if (clm->BeginTransactionWithTarget(ctx)) {
      okSnap = clm->EndEmptyTransaction();
    }
    static uint32_t sSnapFrames = 0;
    if (sSnapFrames++ < 20) {
      JIHAD_GPU_LOG("[jihad-gpu] snapshot road frame %u: layers %.1f ms, snapshot %.1f ms, ok=%d\n",
              sSnapFrames, (t1 - t0).ToMilliseconds(), (mozilla::TimeStamp::Now() - t1).ToMilliseconds(), (int)okSnap);
    }
    return okSnap;
  }
  uint32_t token = JihadComposeStart(aDocShell, aDest, aStride, aWidth, aHeight, 0, aHeight);
  if (!token) {
    return false;
  }
  return JihadComposePoll(token, true, nullptr) > 0;
}

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

// T-502: paint the document as layers and fetch the frame the in-process compositor composes,
// into aDest (B8G8R8A8, aStride bytes per row). False when the widget has no compositor (the
// CPU exports above are then the path), or nothing is laid out yet.
MOZ_EXPORT bool
jihad_offscreen_compose(nsIWidget* aWidget, nsIDocShell* aDocShell, void* aDest, int aStride,
                        int aWidth, int aHeight)
{
  if (!aWidget) {
    return false;
  }
  return static_cast<PuppetWidget*>(aWidget)->JihadComposeTo(aDocShell, aDest, aStride,
                                                             aWidth, aHeight);
}

// T-515: the frame's halves on their own (see PuppetWidget::JihadPaintLayers, JihadComposePost).
// paint_layers: the main thread; compose_post: any thread. aUseView 0: the view last set.
MOZ_EXPORT int
jihad_offscreen_paint_layers(nsIWidget* aWidget, nsIDocShell* aDocShell)
{
  return (aWidget && static_cast<PuppetWidget*>(aWidget)->JihadPaintLayers(aDocShell)) ? 1 : 0;
}
// T-511 (review): the daemon gave the GPU path up (a stall past its bound, refused jobs): WebGL
// makes no more EGL contexts on that GPU. jihad_gpu_compositor_on() is what the engine's GPU-path
// checks ask (JIHAD_GPU_COMPOSITOR in the environment, and not given up); the environment is not
// changed, other threads read it.
static mozilla::Atomic<bool> sJihadGpuGivenUp(false);
MOZ_EXPORT void
jihad_offscreen_gpu_given_up()
{
  sJihadGpuGivenUp = true;
}
// T-513 (review): how many reads of a given-up frame had already written into its slot (a GPU stall
// inside glReadPixels outlasting the daemon's bound); the daemon repaints in software when it moves
MOZ_EXPORT uint32_t
jihad_offscreen_late_writes()
{
  return mozilla::layers::gJihadLateWrites;
}
MOZ_EXPORT bool
jihad_gpu_compositor_on()
{
  static const bool sEnv = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
  return sEnv && !sJihadGpuGivenUp;
}
// T-504: compose_post takes flag 32, a window of the daemon's overview (the scrolled content alone, fixed and
// stuck layers not drawn, cleared transparent, read with its alpha), and flag 64, the fixed and stuck layers alone;
// a daemon asks before it posts one. Not with the swizzle pass (JIHAD_GPU_PASS: its shader forces the alpha)
MOZ_EXPORT int
jihad_offscreen_overview_ok()
{
  static const bool sPass = getenv("JIHAD_GPU_PASS") != nullptr;
  return sPass ? 0 : 1;
}
MOZ_EXPORT int
jihad_offscreen_compose_peek(nsIWidget* aWidget, uint32_t aToken)
{
  return aWidget ? static_cast<PuppetWidget*>(aWidget)->JihadComposePeek(aToken) : -1;
}
MOZ_EXPORT uint32_t
jihad_offscreen_compose_post(nsIWidget* aWidget, void* aDest, int aStride, int aWidth, int aHeight,
                             int aRowLo, int aRowHi, int aRow2Lo, int aRow2Hi, int aShift,
                             const void* aSrc, int aSrcY, int aFlags, int aUseView, double aScale,
                             double aOffX, double aOffY, double aScrollX, double aScrollY, int aScrollValid,
                             int aColX0, int aColX1)
{
  // aColX0, aColX1 (T-511): read only with flag 4 (a daemon from before passes neither)
  if (!aWidget) {
    return 0;
  }
  const float view[5] = { (float)aScale, (float)aOffX, (float)aOffY, (float)aScrollX, (float)aScrollY };
  return static_cast<PuppetWidget*>(aWidget)->JihadComposePost(aDest, aStride, aWidth, aHeight, aRowLo, aRowHi,
                                                               aRow2Lo, aRow2Hi, aShift, aSrc, aSrcY, aFlags,
                                                               aUseView ? view : nullptr, aScrollValid != 0,
                                                               (aFlags & 4) ? aColX0 : 0, (aFlags & 4) ? aColX1 : 0);
}

// T-502: the frame in two halves (see PuppetWidget::JihadComposeStart).
MOZ_EXPORT uint32_t
jihad_offscreen_compose_start(nsIWidget* aWidget, nsIDocShell* aDocShell, void* aDest, int aStride,
                              int aWidth, int aHeight, int aRowLo, int aRowHi, int aRow2Lo, int aRow2Hi, int aShift,
                              const void* aSrc, int aSrcY, int aEngineShift, int aColX0, int aColX1)
{
  if (!aWidget) {
    return 0;
  }
  // flags: 1 engine shift, 2 verify (T-503), 4 the column arguments given (T-511; a daemon from
  // before passes neither, and its poll array holds ten ints), 8 the shift is the daemon's window
  // move (T-517: the rows are in place, only the kept rects copied)
  return static_cast<PuppetWidget*>(aWidget)->JihadComposeStart(aDocShell, aDest, aStride, aWidth, aHeight, aRowLo, aRowHi, aRow2Lo, aRow2Hi, aShift, aSrc, aSrcY, aEngineShift,
                                                                (aEngineShift & 4) ? aColX0 : 0, (aEngineShift & 4) ? aColX1 : 0);
}
MOZ_EXPORT int
jihad_offscreen_compose_poll(nsIWidget* aWidget, uint32_t aToken, int* aOut, int* aScrollOnly)
{
  // aOut: ten ints: the rows the tree changed, the rows fixed layers cover, the two bands read, and
  // this frame's position with a valid flag (round 115); twelve for a job posted with flag 4 (T-511):
  // the columns the composite changed
  if (!aWidget) {
    return -1;
  }
  bool scrollOnly = false;
  const int r = static_cast<PuppetWidget*>(aWidget)->JihadComposePoll(aToken, false, aOut, &scrollOnly);
  if (aScrollOnly) *aScrollOnly = scrollOnly ? 1 : 0;
  return r;
}
MOZ_EXPORT int
jihad_offscreen_compose_poll2(nsIWidget* aWidget, uint32_t aToken, int* aOut, int aOutLen, int* aScrollOnly)
{
  // T-544: the poll with aOutLen ints: past the twelve above, the frame's fixed and stuck rects
  // (aOut[12] of them, then x0, y0, x1, y1 each, frame px) when aOutLen holds 29
  if (!aWidget || aOutLen < 12) {
    return -1;
  }
  bool scrollOnly = false;
  const int r = static_cast<PuppetWidget*>(aWidget)->JihadComposePoll(aToken, false, aOut, &scrollOnly, aOutLen);
  if (aScrollOnly) *aScrollOnly = scrollOnly ? 1 : 0;
  return r;
}
MOZ_EXPORT void
jihad_offscreen_compose_forget(nsIWidget* aWidget, uint32_t aToken)
{
  if (aWidget) {
    static_cast<PuppetWidget*>(aWidget)->JihadComposeForget(aToken);
  }
}
// T-502: the card's scroll (CSS px, absolute), applied by the compositor to the root scroll
// frame's content against the engine's own scroll; aValid 0 turns it off.
// T-502 round 108: the fd the compositor writes a byte to when a compose job finishes (-1: none)
MOZ_EXPORT void
jihad_offscreen_set_done_fd(int aFd)
{
  sJihadDoneFd = aFd;
}

MOZ_EXPORT void
jihad_offscreen_set_scroll(nsIWidget* aWidget, double aX, double aY, int aValid)
{
  if (aWidget) {
    static_cast<PuppetWidget*>(aWidget)->JihadSetScroll(aX, aY, aValid != 0);
  }
}
// T-502: the card's zoom and the visual viewport's residual offset (CSS px), applied by the
// compositor to the whole layer tree.
MOZ_EXPORT void
jihad_offscreen_set_view(nsIWidget* aWidget, double aScale, double aOffsetX, double aOffsetY)
{
  if (aWidget) {
    static_cast<PuppetWidget*>(aWidget)->JihadSetView(aScale, aOffsetX, aOffsetY);
  }
}

// T-502: how many composites the compositor has asked for (layer updates, animations); the
// daemon pulls a frame whenever this moves. 0 without a compositor.
MOZ_EXPORT uint32_t
jihad_offscreen_compositor_seq(nsIWidget* aWidget)
{
  if (!aWidget) {
    return 0;
  }
  mozilla::layers::CompositorBridgeParent* bridge = static_cast<PuppetWidget*>(aWidget)->JihadBridge();
  return bridge ? bridge->JihadRequests() : 0;
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
      if (JihadOffscreen()) {
        // Jihad T-502: no TabChild drives painting here; paint the layer tree (a transaction
        // forwarded to the in-process compositor). JihadComposeTo fetches the frame.
        GetCurrentWidgetListener()->PaintWindow(this, region);
      } else if (mTabChild) {
        // Do nothing, the compositor will handle drawing
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
