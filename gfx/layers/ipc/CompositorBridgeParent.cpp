/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorBridgeParent.h"
#include <stdio.h>                      // for fprintf, stdout
#include <unistd.h>                     // Jihad T-509: sleep() for JIHAD_GPU_TEST_STALL
#include <stdint.h>                     // for uint64_t
#include <map>                          // for _Rb_tree_iterator, etc
#include <utility>                      // for pair
#include "LayerTransactionParent.h"     // for LayerTransactionParent
#include "RenderTrace.h"                // for RenderTraceLayers
#include "base/message_loop.h"          // for MessageLoop
#include "base/process.h"               // for ProcessId
#include "base/task.h"                  // for CancelableTask, etc
#include "base/thread.h"                // for Thread
#include "gfxContext.h"                 // for gfxContext
#include "gfxPlatform.h"                // for gfxPlatform
#include "TreeTraversal.h"              // for ForEachNode
#include "LayerTreeInvalidation.h"   // Jihad T-502: JihadDiffScrollShift
namespace mozilla { namespace layers { extern double gJihadPhase[12]; } }   // LayerManagerComposite.cpp (round 139)
static bool sJihadInCompose = false;   // Jihad T-502 diagnostic: inside JihadComposeAndRead's composite
#include <cmath>                       // Jihad T-502: floor, ceil
#ifdef MOZ_WIDGET_GTK
#include "gfxPlatformGtk.h"             // for gfxPlatform
#endif
#include "gfxPrefs.h"                   // for gfxPrefs
#include "mozilla/AutoRestore.h"        // for AutoRestore
#include "mozilla/ClearOnShutdown.h"    // for ClearOnShutdown
#include "mozilla/DebugOnly.h"          // for DebugOnly
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/TabParent.h"
#include "mozilla/gfx/2D.h"          // for DrawTarget
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gfx/Point.h"          // for IntSize
#include "mozilla/gfx/Rect.h"          // for IntSize
#include "mozilla/ipc/Transport.h"      // for Transport
#include "mozilla/layers/APZCTreeManager.h"  // for APZCTreeManager
#include "mozilla/layers/APZCTreeManagerParent.h"  // for APZCTreeManagerParent
#include "mozilla/layers/APZThreadUtils.h"  // for APZCTreeManager
#include "mozilla/layers/AsyncCompositionManager.h"
#include "mozilla/layers/BasicCompositor.h"  // for BasicCompositor
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/CompositorOGL.h"  // for CompositorOGL
#include "GLTextureImage.h"                 // Jihad T-502: JihadFlushRetiredTextures
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/CrossProcessCompositorBridgeParent.h"
#include "mozilla/layers/FrameUniformityData.h"
#include "mozilla/layers/ImageBridgeParent.h"
#include "mozilla/layers/LayerManagerComposite.h"
#include "mozilla/layers/LayerTreeOwnerTracker.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/PLayerTransactionParent.h"
#include "mozilla/layers/RemoteContentController.h"
#include "mozilla/layout/RenderFrameParent.h"
#include "mozilla/media/MediaSystemResourceService.h" // for MediaSystemResourceService
#include "mozilla/mozalloc.h"           // for operator new, etc
#ifdef MOZ_WIDGET_GTK
#include "basic/X11BasicCompositor.h" // for X11BasicCompositor
#endif
#include "nsCOMPtr.h"                   // for already_AddRefed
#include "nsDebug.h"                    // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"            // for MOZ_COUNT_CTOR, etc
#include "nsIWidget.h"                  // for nsIWidget
#include "nsTArray.h"                   // for nsTArray
#include "nsThreadUtils.h"              // for NS_IsMainThread
#include "nsXULAppAPI.h"                // for XRE_GetIOMessageLoop
#ifdef XP_WIN
#include "mozilla/layers/CompositorD3D11.h"
#endif
#include "GeckoProfiler.h"
#include "mozilla/ipc/ProtocolTypes.h"
#include "mozilla/Unused.h"
#include "mozilla/Hal.h"
#include "mozilla/HalTypes.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/VsyncDispatcher.h"
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
#include "VsyncSource.h"
#endif
#include "mozilla/widget/CompositorWidget.h"
#ifdef MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING
# include "mozilla/widget/CompositorWidgetParent.h"
#endif

#include "LayerScope.h"

namespace mozilla {

namespace layers {

using namespace mozilla::ipc;
using namespace mozilla::gfx;
using namespace std;

using base::ProcessId;
using base::Thread;

ProcessId
CompositorBridgeParentBase::GetChildProcessId()
{
  return OtherPid();
}

void
CompositorBridgeParentBase::NotifyNotUsed(PTextureParent* aTexture, uint64_t aTransactionId)
{
  RefPtr<TextureHost> texture = TextureHost::AsTextureHost(aTexture);
  if (!texture) {
    return;
  }

  if (!(texture->GetFlags() & TextureFlags::RECYCLE)) {
    return;
  }

  uint64_t textureId = TextureHost::GetTextureSerial(aTexture);
  mPendingAsyncMessage.push_back(
    OpNotifyNotUsed(textureId, aTransactionId));
}

void
CompositorBridgeParentBase::SendAsyncMessage(const InfallibleTArray<AsyncParentMessageData>& aMessage)
{
  Unused << SendParentAsyncMessages(aMessage);
}

bool
CompositorBridgeParentBase::AllocShmem(size_t aSize,
                                   ipc::SharedMemory::SharedMemoryType aType,
                                   ipc::Shmem* aShmem)
{
  return PCompositorBridgeParent::AllocShmem(aSize, aType, aShmem);
}

bool
CompositorBridgeParentBase::AllocUnsafeShmem(size_t aSize,
                                         ipc::SharedMemory::SharedMemoryType aType,
                                         ipc::Shmem* aShmem)
{
  return PCompositorBridgeParent::AllocUnsafeShmem(aSize, aType, aShmem);
}

void
CompositorBridgeParentBase::DeallocShmem(ipc::Shmem& aShmem)
{
  PCompositorBridgeParent::DeallocShmem(aShmem);
}

base::ProcessId
CompositorBridgeParentBase::RemotePid()
{
  return OtherPid();
}

bool
CompositorBridgeParentBase::StartSharingMetrics(ipc::SharedMemoryBasic::Handle aHandle,
                                                CrossProcessMutexHandle aMutexHandle,
                                                uint64_t aLayersId,
                                                uint32_t aApzcId)
{
  return PCompositorBridgeParent::SendSharedCompositorFrameMetrics(
    aHandle, aMutexHandle, aLayersId, aApzcId);
}

bool
CompositorBridgeParentBase::StopSharingMetrics(FrameMetrics::ViewID aScrollId,
                                               uint32_t aApzcId)
{
  return PCompositorBridgeParent::SendReleaseSharedCompositorFrameMetrics(
    aScrollId, aApzcId);
}

CompositorBridgeParent::LayerTreeState::LayerTreeState()
  : mApzcTreeManagerParent(nullptr)
  , mParent(nullptr)
  , mLayerManager(nullptr)
  , mCrossProcessParent(nullptr)
  , mLayerTree(nullptr)
  , mUpdatedPluginDataAvailable(false)
  , mPendingCompositorUpdates(0)
{
}

CompositorBridgeParent::LayerTreeState::~LayerTreeState()
{
  if (mController) {
    mController->Destroy();
  }
}

typedef map<uint64_t, CompositorBridgeParent::LayerTreeState> LayerTreeMap;
static LayerTreeMap sIndirectLayerTrees;
static StaticAutoPtr<mozilla::Monitor> sIndirectLayerTreesLock;

static void EnsureLayerTreeMapReady()
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!sIndirectLayerTreesLock) {
    sIndirectLayerTreesLock = new Monitor("IndirectLayerTree");
    mozilla::ClearOnShutdown(&sIndirectLayerTreesLock);
  }
}

template <typename Lambda>
inline void
CompositorBridgeParent::ForEachIndirectLayerTree(const Lambda& aCallback)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  for (auto it = sIndirectLayerTrees.begin(); it != sIndirectLayerTrees.end(); it++) {
    LayerTreeState* state = &it->second;
    if (state->mParent == this) {
      aCallback(state, it->first);
    }
  }
}

/**
  * A global map referencing each compositor by ID.
  *
  * This map is used by the ImageBridge protocol to trigger
  * compositions without having to keep references to the
  * compositor
  */
typedef map<uint64_t,CompositorBridgeParent*> CompositorMap;
static StaticAutoPtr<CompositorMap> sCompositorMap;

void
CompositorBridgeParent::Setup()
{
  EnsureLayerTreeMapReady();

  MOZ_ASSERT(!sCompositorMap);
  sCompositorMap = new CompositorMap;
}

void
CompositorBridgeParent::Shutdown()
{
  MOZ_ASSERT(sCompositorMap);
  MOZ_ASSERT(sCompositorMap->empty());
  sCompositorMap = nullptr;
}

void
CompositorBridgeParent::FinishShutdown()
{
  // TODO: this should be empty by now...
  sIndirectLayerTrees.clear();
}

static void SetThreadPriority()
{
  hal::SetCurrentThreadPriority(hal::THREAD_PRIORITY_COMPOSITOR);
}

#ifdef COMPOSITOR_PERFORMANCE_WARNING
static int32_t
CalculateCompositionFrameRate()
{
  // Used when layout.frame_rate is -1. Needs to be kept in sync with
  // DEFAULT_FRAME_RATE in nsRefreshDriver.cpp.
  // TODO: This should actually return the vsync rate.
  const int32_t defaultFrameRate = 60;
  int32_t compositionFrameRatePref = gfxPrefs::LayersCompositionFrameRate();
  if (compositionFrameRatePref < 0) {
    // Use the same frame rate for composition as for layout.
    int32_t layoutFrameRatePref = gfxPrefs::LayoutFrameRate();
    if (layoutFrameRatePref < 0) {
      // TODO: The main thread frame scheduling code consults the actual
      // monitor refresh rate in this case. We should do the same.
      return defaultFrameRate;
    }
    return layoutFrameRatePref;
  }
  return compositionFrameRatePref;
}
#endif

CompositorVsyncScheduler::Observer::Observer(CompositorVsyncScheduler* aOwner)
  : mMutex("CompositorVsyncScheduler.Observer.Mutex")
  , mOwner(aOwner)
{
}

CompositorVsyncScheduler::Observer::~Observer()
{
  MOZ_ASSERT(!mOwner);
}

bool
CompositorVsyncScheduler::Observer::NotifyVsync(TimeStamp aVsyncTimestamp)
{
  MutexAutoLock lock(mMutex);
  if (!mOwner) {
    return false;
  }
  return mOwner->NotifyVsync(aVsyncTimestamp);
}

void
CompositorVsyncScheduler::Observer::Destroy()
{
  MutexAutoLock lock(mMutex);
  mOwner = nullptr;
}

CompositorVsyncScheduler::CompositorVsyncScheduler(CompositorBridgeParent* aCompositorBridgeParent,
                                                   widget::CompositorWidget* aWidget)
  : mCompositorBridgeParent(aCompositorBridgeParent)
  , mLastCompose(TimeStamp::Now())
  , mIsObservingVsync(false)
  , mNeedsComposite(0)
  , mVsyncNotificationsSkipped(0)
  , mWidget(aWidget)
  , mCurrentCompositeTaskMonitor("CurrentCompositeTaskMonitor")
  , mCurrentCompositeTask(nullptr)
  , mSetNeedsCompositeMonitor("SetNeedsCompositeMonitor")
  , mSetNeedsCompositeTask(nullptr)
{
  MOZ_ASSERT(NS_IsMainThread() || XRE_GetProcessType() == GeckoProcessType_GPU);
  mVsyncObserver = new Observer(this);

  // mAsapScheduling is set on the main thread during init,
  // but is only accessed after on the compositor thread.
  mAsapScheduling = gfxPrefs::LayersCompositionFrameRate() == 0 ||
                    gfxPlatform::IsInLayoutAsapMode();
}

CompositorVsyncScheduler::~CompositorVsyncScheduler()
{
  MOZ_ASSERT(!mIsObservingVsync);
  MOZ_ASSERT(!mVsyncObserver);
  // The CompositorVsyncDispatcher is cleaned up before this in the nsBaseWidget, which stops vsync listeners
  mCompositorBridgeParent = nullptr;
}

void
CompositorVsyncScheduler::Destroy()
{
  if (!mVsyncObserver) {
    // Destroy was already called on this object.
    return;
  }
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  UnobserveVsync();
  mVsyncObserver->Destroy();
  mVsyncObserver = nullptr;

  CancelCurrentSetNeedsCompositeTask();
  CancelCurrentCompositeTask();
}

void
CompositorVsyncScheduler::PostCompositeTask(TimeStamp aCompositeTimestamp)
{
  // can be called from the compositor or vsync thread
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask == nullptr && CompositorThreadHolder::Loop()) {
    RefPtr<CancelableRunnable> task =
      NewCancelableRunnableMethod<TimeStamp>(this, &CompositorVsyncScheduler::Composite,
                                             aCompositeTimestamp);
    mCurrentCompositeTask = task;
    ScheduleTask(task.forget(), 0);
  }
}

void
CompositorVsyncScheduler::ScheduleComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  // Jihad T-502 (JIHAD_GPU_COMPOSITOR): pull mode. No vsync-driven composite; the request is
  // counted and the daemon, which owns the frame rate, pulls a frame through
  // CompositorBridgeParent::JihadComposeAndRead when the count moves. An animation keeps
  // asking after every composite, so it keeps the daemon pulling.
  static const bool sJihadPull = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
  if (sJihadPull) {
    mJihadRequests++;
    return;
  }
  if (mAsapScheduling) {
    // Used only for performance testing purposes
    PostCompositeTask(TimeStamp::Now());
  } else {
    SetNeedsComposite();
  }
}

void
CompositorVsyncScheduler::CancelCurrentSetNeedsCompositeTask()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MonitorAutoLock lock(mSetNeedsCompositeMonitor);
  if (mSetNeedsCompositeTask) {
    mSetNeedsCompositeTask->Cancel();
    mSetNeedsCompositeTask = nullptr;
  }
  mNeedsComposite = 0;
}

/**
 * TODO Potential performance heuristics:
 * If a composite takes 17 ms, do we composite ASAP or wait until next vsync?
 * If a layer transaction comes after vsync, do we composite ASAP or wait until
 * next vsync?
 * How many skipped vsync events until we stop listening to vsync events?
 */
void
CompositorVsyncScheduler::SetNeedsComposite()
{
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    RefPtr<CancelableRunnable> task =
      NewCancelableRunnableMethod(this, &CompositorVsyncScheduler::SetNeedsComposite);
    mSetNeedsCompositeTask = task;
    ScheduleTask(task.forget(), 0);
    return;
  } else {
    MonitorAutoLock lock(mSetNeedsCompositeMonitor);
    mSetNeedsCompositeTask = nullptr;
  }

  mNeedsComposite++;
  if (!mIsObservingVsync && mNeedsComposite) {
    ObserveVsync();
  }
}

bool
CompositorVsyncScheduler::NotifyVsync(TimeStamp aVsyncTimestamp)
{
  // Called from the vsync dispatch thread. When in the GPU Process, that's
  // the same as the compositor thread.
  MOZ_ASSERT_IF(XRE_IsParentProcess(), !CompositorThreadHolder::IsInCompositorThread());
  MOZ_ASSERT_IF(XRE_GetProcessType() == GeckoProcessType_GPU, CompositorThreadHolder::IsInCompositorThread());
  MOZ_ASSERT(!NS_IsMainThread());
  PostCompositeTask(aVsyncTimestamp);
  return true;
}

void
CompositorVsyncScheduler::CancelCurrentCompositeTask()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread() || NS_IsMainThread());
  MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
  if (mCurrentCompositeTask) {
    mCurrentCompositeTask->Cancel();
    mCurrentCompositeTask = nullptr;
  }
}

void
CompositorVsyncScheduler::Composite(TimeStamp aVsyncTimestamp)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  {
    MonitorAutoLock lock(mCurrentCompositeTaskMonitor);
    mCurrentCompositeTask = nullptr;
  }

  if ((aVsyncTimestamp < mLastCompose) && !mAsapScheduling) {
    // We can sometimes get vsync timestamps that are in the past
    // compared to the last compose with force composites.
    // In those cases, wait until the next vsync;
    return;
  }

  MOZ_ASSERT(mCompositorBridgeParent);
  if (!mAsapScheduling && mCompositorBridgeParent->IsPendingComposite()) {
    // If previous composite is still on going, finish it and does a next
    // composite in a next vsync.
    mCompositorBridgeParent->FinishPendingComposite();
    return;
  }

  DispatchTouchEvents(aVsyncTimestamp);

  if (mNeedsComposite || mAsapScheduling) {
    mNeedsComposite = 0;
    mLastCompose = aVsyncTimestamp;
    ComposeToTarget(nullptr);
    mVsyncNotificationsSkipped = 0;
  } else if (mVsyncNotificationsSkipped++ > gfxPrefs::CompositorUnobserveCount()) {
    UnobserveVsync();
  }
}

void
CompositorVsyncScheduler::OnForceComposeToTarget()
{
  /**
   * bug 1138502 - There are cases such as during long-running window resizing events
   * where we receive many sync RecvFlushComposites. We also get vsync notifications which
   * will increment mVsyncNotificationsSkipped because a composite just occurred. After
   * enough vsyncs and RecvFlushComposites occurred, we will disable vsync. Then at the next
   * ScheduleComposite, we will enable vsync, then get a RecvFlushComposite, which will
   * force us to unobserve vsync again. On some platforms, enabling/disabling vsync is not
   * free and this oscillating behavior causes a performance hit. In order to avoid this problem,
   * we reset the mVsyncNotificationsSkipped counter to keep vsync enabled.
   */
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mVsyncNotificationsSkipped = 0;
}

void
CompositorVsyncScheduler::ForceComposeToTarget(gfx::DrawTarget* aTarget, const IntRect* aRect)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  OnForceComposeToTarget();
  mLastCompose = TimeStamp::Now();
  ComposeToTarget(aTarget, aRect);
}

bool
CompositorVsyncScheduler::NeedsComposite()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  return mNeedsComposite;
}

void
CompositorVsyncScheduler::ObserveVsync()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(mVsyncObserver);
  mIsObservingVsync = true;
}

void
CompositorVsyncScheduler::UnobserveVsync()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mWidget->ObserveVsync(nullptr);
  mIsObservingVsync = false;
}

void
CompositorVsyncScheduler::DispatchTouchEvents(TimeStamp aVsyncTimestamp)
{
}

void
CompositorVsyncScheduler::ScheduleTask(already_AddRefed<CancelableRunnable> aTask,
                                       int aTime)
{
  MOZ_ASSERT(CompositorThreadHolder::Loop());
  MOZ_ASSERT(aTime >= 0);
  CompositorThreadHolder::Loop()->PostDelayedTask(Move(aTask), aTime);
}

void
CompositorVsyncScheduler::ResumeComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  mLastCompose = TimeStamp::Now();
  ComposeToTarget(nullptr);
}

void
CompositorVsyncScheduler::ComposeToTarget(gfx::DrawTarget* aTarget, const IntRect* aRect)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  MOZ_ASSERT(mCompositorBridgeParent);
  mCompositorBridgeParent->CompositeToTarget(aTarget, aRect);
}

static inline MessageLoop*
CompositorLoop()
{
  return CompositorThreadHolder::Loop();
}

CompositorBridgeParent::CompositorBridgeParent(CSSToLayoutDeviceScale aScale,
                                               const TimeDuration& aVsyncRate,
                                               bool aUseExternalSurfaceSize,
                                               const gfx::IntSize& aSurfaceSize)
  : mWidget(nullptr)
  , mScale(aScale)
  , mVsyncRate(aVsyncRate)
  , mIsTesting(false)
  , mPendingTransaction(0)
  , mPaused(false)
  , mUseExternalSurfaceSize(aUseExternalSurfaceSize)
  , mEGLSurfaceSize(aSurfaceSize)
  , mPauseCompositionMonitor("PauseCompositionMonitor")
  , mResumeCompositionMonitor("ResumeCompositionMonitor")
  , mResetCompositorMonitor("ResetCompositorMonitor")
  , mRootLayerTreeID(0)
  , mOverrideComposeReadiness(false)
  , mForceCompositionTask(nullptr)
  , mCompositorThreadHolder(CompositorThreadHolder::GetSingleton())
  , mCompositorScheduler(nullptr)
  , mPaintTime(TimeDuration::Forever())
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  , mLastPluginUpdateLayerTreeId(0)
  , mDeferPluginWindows(false)
  , mPluginWindowsHidden(false)
#endif
{
  // Always run destructor on the main thread
  MOZ_ASSERT(NS_IsMainThread());
}

void
CompositorBridgeParent::InitSameProcess(widget::CompositorWidget* aWidget,
                                        const uint64_t& aLayerTreeId,
                                        bool aUseAPZ)
{
  mWidget = aWidget;
  mRootLayerTreeID = aLayerTreeId;
  if (aUseAPZ) {
    mApzcTreeManager = new APZCTreeManager();
  }

  // IPDL initialization. mSelfRef is cleared in DeferredDestroy.
  SetOtherProcessId(base::GetCurrentProcId());
  mSelfRef = this;

  Initialize();
}

bool
CompositorBridgeParent::Bind(Endpoint<PCompositorBridgeParent>&& aEndpoint)
{
  if (!aEndpoint.Bind(this)) {
    return false;
  }
  mSelfRef = this;
  return true;
}

bool
CompositorBridgeParent::RecvInitialize(const uint64_t& aRootLayerTreeId)
{
  mRootLayerTreeID = aRootLayerTreeId;

  Initialize();
  return true;
}

void
CompositorBridgeParent::Initialize()
{
  MOZ_ASSERT(CompositorThread(),
             "The compositor thread must be Initialized before instanciating a CompositorBridgeParent.");

  mCompositorID = 0;
  // FIXME: This holds on the the fact that right now the only thing that
  // can destroy this instance is initialized on the compositor thread after
  // this task has been processed.
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableFunction(&AddCompositor,
                                                 this, &mCompositorID));

  CompositorLoop()->PostTask(NewRunnableFunction(SetThreadPriority));


  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees[mRootLayerTreeID].mParent = this;
  }

  LayerScope::SetPixelScale(mScale.scale);

  mCompositorScheduler = new CompositorVsyncScheduler(this, mWidget);
}

bool
CompositorBridgeParent::RecvReset(nsTArray<LayersBackend>&& aBackendHints, bool* aResult, TextureFactoryIdentifier* aOutIdentifier)
{
  Maybe<TextureFactoryIdentifier> newIdentifier;
  ResetCompositorTask(aBackendHints, &newIdentifier);
  
  if (newIdentifier) {
    *aResult = true;
    *aOutIdentifier = newIdentifier.value();
  } else {
    *aResult = false;
  }

  return true;
}

uint64_t
CompositorBridgeParent::RootLayerTreeId()
{
  MOZ_ASSERT(mRootLayerTreeID);
  return mRootLayerTreeID;
}

CompositorBridgeParent::~CompositorBridgeParent()
{
  InfallibleTArray<PTextureParent*> textures;
  ManagedPTextureParent(textures);
  // We expect all textures to be destroyed by now.
  MOZ_DIAGNOSTIC_ASSERT(textures.Length() == 0);
  for (unsigned int i = 0; i < textures.Length(); ++i) {
    RefPtr<TextureHost> tex = TextureHost::AsTextureHost(textures[i]);
    tex->DeallocateDeviceData();
  }
}

void
CompositorBridgeParent::ForceIsFirstPaint()
{
  mCompositionManager->ForceIsFirstPaint();
}

void
CompositorBridgeParent::StopAndClearResources()
{
  if (mForceCompositionTask) {
    mForceCompositionTask->Cancel();
    mForceCompositionTask = nullptr;
  }

  mPaused = true;

  // Ensure that the layer manager is destroyed before CompositorBridgeChild.
  if (mLayerManager) {
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    ForEachIndirectLayerTree([this] (LayerTreeState* lts, uint64_t) -> void {
      mLayerManager->ClearCachedResources(lts->mRoot);
      lts->mLayerManager = nullptr;
      lts->mParent = nullptr;
    });
    mLayerManager->Destroy();
    mLayerManager = nullptr;
    mCompositionManager = nullptr;
  }

  if (mCompositor) {
    mCompositor->DetachWidget();
    mCompositor->Destroy();
    mCompositor = nullptr;
  }

  // This must be destroyed now since it accesses the widget.
  if (mCompositorScheduler) {
    mCompositorScheduler->Destroy();
    mCompositorScheduler = nullptr;
  }

  // After this point, it is no longer legal to access the widget.
  mWidget = nullptr;
}

bool
CompositorBridgeParent::RecvWillClose()
{
  StopAndClearResources();
  return true;
}

void CompositorBridgeParent::DeferredDestroy()
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(mCompositorThreadHolder);
  mCompositorThreadHolder = nullptr;
  mSelfRef = nullptr;
}

bool
CompositorBridgeParent::RecvPause()
{
  PauseComposition();
  return true;
}

bool
CompositorBridgeParent::RecvResume()
{
  ResumeComposition();
  return true;
}

bool
CompositorBridgeParent::RecvMakeSnapshot(const SurfaceDescriptor& aInSnapshot,
                                         const gfx::IntRect& aRect)
{
  RefPtr<DrawTarget> target = GetDrawTargetForDescriptor(aInSnapshot, gfx::BackendType::CAIRO);
  MOZ_ASSERT(target);
  if (!target) {
    // We kill the content process rather than have it continue with an invalid
    // snapshot, that may be too harsh and we could decide to return some sort
    // of error to the child process and let it deal with it...
    return false;
  }
  ForceComposeToTarget(target, &aRect);
  return true;
}

bool
CompositorBridgeParent::RecvFlushRendering()
{
  if (mCompositorScheduler->NeedsComposite())
  {
    CancelCurrentCompositeTask();
    ForceComposeToTarget(nullptr);
  }
  return true;
}

bool
CompositorBridgeParent::RecvForcePresent()
{
  // During the shutdown sequence mLayerManager may be null
  if (mLayerManager) {
    mLayerManager->ForcePresent();
  }
  return true;
}

bool
CompositorBridgeParent::RecvNotifyRegionInvalidated(const nsIntRegion& aRegion)
{
  if (mLayerManager) {
    mLayerManager->AddInvalidRegion(aRegion);
  }
  return true;
}

void
CompositorBridgeParent::Invalidate()
{
  if (mLayerManager && mLayerManager->GetRoot()) {
    mLayerManager->AddInvalidRegion(
                                    mLayerManager->GetRoot()->GetLocalVisibleRegion().ToUnknownRegion().GetBounds());
  }
}

bool
CompositorBridgeParent::RecvClearApproximatelyVisibleRegions(const uint64_t& aLayersId,
                                                            const uint32_t& aPresShellId)
{
  ClearApproximatelyVisibleRegions(aLayersId, Some(aPresShellId));
  return true;
}

void
CompositorBridgeParent::ClearApproximatelyVisibleRegions(const uint64_t& aLayersId,
                                                         const Maybe<uint32_t>& aPresShellId)
{
  if (mLayerManager) {
    mLayerManager->ClearApproximatelyVisibleRegions(aLayersId, aPresShellId);

    // We need to recomposite to update the minimap.
    ScheduleComposition();
  }
}

bool
CompositorBridgeParent::RecvNotifyApproximatelyVisibleRegion(const ScrollableLayerGuid& aGuid,
                                                             const CSSIntRegion& aRegion)
{
  if (mLayerManager) {
    mLayerManager->UpdateApproximatelyVisibleRegion(aGuid, aRegion);

    // We need to recomposite to update the minimap.
    ScheduleComposition();
  }
  return true;
}

void
CompositorBridgeParent::ActorDestroy(ActorDestroyReason why)
{
  StopAndClearResources();

  RemoveCompositor(mCompositorID);

  mCompositionManager = nullptr;

  if (mApzcTreeManager) {
    mApzcTreeManager->ClearTree();
    mApzcTreeManager = nullptr;
  }

  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees.erase(mRootLayerTreeID);
  }

  // There are chances that the ref count reaches zero on the main thread shortly
  // after this function returns while some ipdl code still needs to run on
  // this thread.
  // We must keep the compositor parent alive untill the code handling message
  // reception is finished on this thread.
  mSelfRef = this;
  MessageLoop::current()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::DeferredDestroy));
}

void
CompositorBridgeParent::ScheduleRenderOnCompositorThread()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::ScheduleComposition));
}

void
CompositorBridgeParent::InvalidateOnCompositorThread()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::Invalidate));
}

void
CompositorBridgeParent::PauseComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "PauseComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mPauseCompositionMonitor);

  if (!mPaused) {
    mPaused = true;

    mCompositor->Pause();

    TimeStamp now = TimeStamp::Now();
    DidComposite(now, now);
  }

  // if anyone's waiting to make sure that composition really got paused, tell them
  lock.NotifyAll();
}

void
CompositorBridgeParent::ResumeComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "ResumeComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mResumeCompositionMonitor);

  if (!mCompositor->Resume()) {
    lock.NotifyAll();
    return;
  }

  mPaused = false;

  Invalidate();
  mCompositorScheduler->ResumeComposition();

  // if anyone's waiting to make sure that composition really got resumed, tell them
  lock.NotifyAll();
}

void
CompositorBridgeParent::ForceComposition()
{
  // Cancel the orientation changed state to force composition
  mForceCompositionTask = nullptr;
  ScheduleRenderOnCompositorThread();
}

void
CompositorBridgeParent::CancelCurrentCompositeTask()
{
  mCompositorScheduler->CancelCurrentCompositeTask();
}

void
CompositorBridgeParent::SetEGLSurfaceSize(int width, int height)
{
  NS_ASSERTION(mUseExternalSurfaceSize, "Compositor created without UseExternalSurfaceSize provided");
  mEGLSurfaceSize.SizeTo(width, height);
  if (mCompositor) {
    mCompositor->SetDestinationSurfaceSize(gfx::IntSize(mEGLSurfaceSize.width, mEGLSurfaceSize.height));
  }
}

void
CompositorBridgeParent::ResumeCompositionAndResize(int width, int height)
{
  SetEGLSurfaceSize(width, height);
  ResumeComposition();
}

/*
 * This will execute a pause synchronously, waiting to make sure that the compositor
 * really is paused.
 */
void
CompositorBridgeParent::SchedulePauseOnCompositorThread()
{
  MonitorAutoLock lock(mPauseCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::PauseComposition));

  // Wait until the pause has actually been processed by the compositor thread
  lock.Wait();
}

bool
CompositorBridgeParent::ScheduleResumeOnCompositorThread()
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::ResumeComposition));

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

bool
CompositorBridgeParent::ScheduleResumeOnCompositorThread(int width, int height)
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod
                             <int, int>(this,
                                        &CompositorBridgeParent::ResumeCompositionAndResize,
                                        width, height));

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

void
CompositorBridgeParent::ScheduleTask(already_AddRefed<CancelableRunnable> task, int time)
{
  if (time == 0) {
    MessageLoop::current()->PostTask(Move(task));
  } else {
    MessageLoop::current()->PostDelayedTask(Move(task), time);
  }
}

void
CompositorBridgeParent::UpdatePaintTime(LayerTransactionParent* aLayerTree,
                                        const TimeDuration& aPaintTime)
{
  // We get a lot of paint timings for things with empty transactions.
  if (!mLayerManager || aPaintTime.ToMilliseconds() < 1.0) {
    return;
  }

  mLayerManager->SetPaintTime(aPaintTime);
}

void
CompositorBridgeParent::NotifyShadowTreeTransaction(uint64_t aId, bool aIsFirstPaint,
    bool aScheduleComposite, uint32_t aPaintSequenceNumber,
    bool aIsRepeatTransaction, bool aHitTestUpdate)
{
  if (!aIsRepeatTransaction &&
      mLayerManager &&
      mLayerManager->GetRoot()) {
    // Process plugin data here to give time for them to update before the next
    // composition.
    bool pluginsUpdatedFlag = true;
    AutoResolveRefLayers resolve(mCompositionManager, this, nullptr,
                                 &pluginsUpdatedFlag);

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
    // If plugins haven't been updated, stop waiting.
    if (!pluginsUpdatedFlag) {
      mWaitForPluginsUntil = TimeStamp();
      mHaveBlockedForPlugins = false;
    }
#endif

    if (mApzcTreeManager && aHitTestUpdate) {
      mApzcTreeManager->UpdateHitTestingTree(mRootLayerTreeID,
          mLayerManager->GetRoot(), aIsFirstPaint, aId, aPaintSequenceNumber);
    }

    mLayerManager->NotifyShadowTreeTransaction();
  }
  if (aScheduleComposite) {
    ScheduleComposition();
  }
}

void
CompositorBridgeParent::ScheduleComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (mPaused) {
    return;
  }

  mCompositorScheduler->ScheduleComposition();
}

// Go down the composite layer tree, setting properties to match their
// content-side counterparts.
/* static */ void
CompositorBridgeParent::SetShadowProperties(Layer* aLayer)
{
  ForEachNode<ForwardIterator>(
      aLayer,
      [] (Layer *layer)
      {
        if (Layer* maskLayer = layer->GetMaskLayer()) {
          SetShadowProperties(maskLayer);
        }
        for (size_t i = 0; i < layer->GetAncestorMaskLayerCount(); i++) {
          SetShadowProperties(layer->GetAncestorMaskLayerAt(i));
        }

        // FIXME: Bug 717688 -- Do these updates in LayerTransactionParent::RecvUpdate.
        LayerComposite* layerComposite = layer->AsLayerComposite();
        // Set the layerComposite's base transform to the layer's base transform.
        layerComposite->SetShadowBaseTransform(layer->GetBaseTransform());
        layerComposite->SetShadowTransformSetByAnimation(false);
        layerComposite->SetShadowVisibleRegion(layer->GetVisibleRegion());
        layerComposite->SetShadowClipRect(layer->GetClipRect());
        layerComposite->SetShadowOpacity(layer->GetOpacity());
        layerComposite->SetShadowOpacitySetByAnimation(false);
      }
    );
}

void
CompositorBridgeParent::CompositeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  {
    // Jihad T-502 diagnostic: composites not started by the daemon's request
    static uint32_t sStray = 0, sOurs = 0;
    if (sJihadInCompose) ++sOurs; else { ++sStray; fprintf(stderr, "[jihad-gpu] stray composite %u (ours so far %u), target %p\n", sStray, sOurs, aTarget); }
  }
  profiler_tracing("Paint", "Composite", TRACING_INTERVAL_START);
  PROFILER_LABEL("CompositorBridgeParent", "Composite",
    js::ProfileEntry::Category::GRAPHICS);

  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "Composite can only be called on the compositor thread");
  TimeStamp start = TimeStamp::Now();

#ifdef COMPOSITOR_PERFORMANCE_WARNING
  TimeDuration scheduleDelta = TimeStamp::Now() - mCompositorScheduler->GetExpectedComposeStartTime();
  if (scheduleDelta > TimeDuration::FromMilliseconds(2) ||
      scheduleDelta < TimeDuration::FromMilliseconds(-2)) {
    printf_stderr("Compositor: Compose starting off schedule by %4.1f ms\n",
                  scheduleDelta.ToMilliseconds());
  }
#endif

  if (!CanComposite()) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
    return;
  }

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  if (!mWaitForPluginsUntil.IsNull() &&
      mWaitForPluginsUntil > start) {
    mHaveBlockedForPlugins = true;
    ScheduleComposition();
    return;
  }
#endif

  /*
   * AutoResolveRefLayers handles two tasks related to Windows and Linux
   * plugin window management:
   * 1) calculating if we have remote content in the view. If we do not have
   * remote content, all plugin windows for this CompositorBridgeParent (window)
   * can be hidden since we do not support plugins in chrome when running
   * under e10s.
   * 2) Updating plugin position, size, and clip. We do this here while the
   * remote layer tree is hooked up to to chrome layer tree. This is needed
   * since plugin clipping can depend on chrome (for example, due to tab modal
   * prompts). Updates in step 2 are applied via an async ipc message sent
   * to the main thread.
   */
  bool hasRemoteContent = false;
  bool updatePluginsFlag = true;
  AutoResolveRefLayers resolve(mCompositionManager, this,
                               &hasRemoteContent,
                               &updatePluginsFlag);

  static const bool sJihadPhases = getenv("JIHAD_GPU_PHASES") != nullptr;
  const TimeStamp jihadS0 = sJihadPhases ? TimeStamp::Now() : TimeStamp();
  double jihadPhase0[12];   // T-504: this composite's own phases, logged when it is slow
  if (sJihadPhases) for (int i = 0; i < 12; i++) jihadPhase0[i] = gJihadPhase[i];
  if (aTarget) {
    mLayerManager->BeginTransactionWithDrawTarget(aTarget, *aRect);
  } else {
    mLayerManager->BeginTransaction();
  }

  SetShadowProperties(mLayerManager->GetRoot());
  if (sJihadPhases) gJihadPhase[6] += (TimeStamp::Now() - jihadS0).ToMilliseconds();

  if (mForceCompositionTask && !mOverrideComposeReadiness) {
    if (mCompositionManager->ReadyForCompose()) {
      mForceCompositionTask->Cancel();
      mForceCompositionTask = nullptr;
    } else {
      return;
    }
  }

  mCompositionManager->ComputeRotation();

  TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
  const TimeStamp jihadT0 = sJihadPhases ? TimeStamp::Now() : TimeStamp();
  bool requestNextFrame = mCompositionManager->TransformShadowTree(time, mVsyncRate);
  if (sJihadPhases) gJihadPhase[7] += (TimeStamp::Now() - jihadT0).ToMilliseconds();
  if (requestNextFrame) {
    ScheduleComposition();
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
    // If we have visible windowed plugins then we need to wait for content (and
    // then the plugins) to have been updated by the active animation.
    if (!mPluginWindowsHidden && mCachedPluginData.Length()) {
      mWaitForPluginsUntil = mCompositorScheduler->GetLastComposeTime() + (mVsyncRate * 2);
    }
#endif
  }

  RenderTraceLayers(mLayerManager->GetRoot(), "0000");

#ifdef MOZ_DUMP_PAINTING
  if (gfxPrefs::DumpHostLayers()) {
    printf_stderr("Painting --- compositing layer tree:\n");
    mLayerManager->Dump(/* aSorted = */ true);
  }
#endif
  mLayerManager->SetDebugOverlayWantsNextFrame(false);
  mLayerManager->EndTransaction(time);
  if (sJihadPhases && sJihadInCompose) {
    const double all = (TimeStamp::Now() - jihadS0).ToMilliseconds();
    static uint32_t sSlow = 0;
    if (all >= 80.0 && sSlow++ < 40) {
      double d[12];
      for (int i = 0; i < 12; i++) d[i] = gJihadPhase[i] - jihadPhase0[i];
      fprintf(stderr, "[jihad-gpu] slow composite %.0f ms: shadow props %.1f, async transforms %.1f, post-process %.1f, transforms+diff %.1f, begin frame %.1f, prepare %.1f, render layers %.1f (draws %.1f: %.0f calls, %.0f culled; uploads %.1f), end frame %.1f\n",
              all, d[6], d[7], d[0], d[1], d[2], d[3], d[4], d[8], d[9], d[10], d[11], d[5]);
    }
    static uint32_t sN = 0;
    if ((++sN % 20) == 0) {
      size_t layers = 0, painted = 0;
      if (Layer* root = mLayerManager->GetRoot()) {
        ForEachNode<ForwardIterator>(root, [&](Layer* l) { ++layers; if (l->GetType() == Layer::TYPE_PAINTED) ++painted; });
      }
      fprintf(stderr, "[jihad-gpu] phases over 20 composites (ms each): shadow props %.2f, async transforms %.2f, post-process %.2f, transforms+diff %.2f, begin frame %.2f, prepare %.2f, render layers %.2f (draws %.2f: %.1f calls, %.1f culled; uploads %.2f), end frame %.2f; %zu layers (%zu painted)\n",
              gJihadPhase[6] / 20, gJihadPhase[7] / 20, gJihadPhase[0] / 20, gJihadPhase[1] / 20, gJihadPhase[2] / 20,
              gJihadPhase[3] / 20, gJihadPhase[4] / 20, gJihadPhase[8] / 20, gJihadPhase[9] / 20, gJihadPhase[10] / 20,
              gJihadPhase[11] / 20, gJihadPhase[5] / 20, layers, painted);
      for (double& d : gJihadPhase) d = 0;
    }
  }

  if (!aTarget) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
  } else {
    // Jihad T-502: the whole compose-to-target on the compositor thread (JIHAD_GPU_COMPOSITOR).
    static const bool sJihadGpuLog = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
    if (sJihadGpuLog) {
      fprintf(stderr, "[jihad-gpu] compose-to-target: %.1f ms on the compositor thread (incl. copy-to-target)\n",
              (TimeStamp::Now() - start).ToMilliseconds());
    }
  }

  // We're not really taking advantage of the stored composite-again-time here.
  // We might be able to skip the next few composites altogether. However,
  // that's a bit complex to implement and we'll get most of the advantage
  // by skipping compositing when we detect there's nothing invalid. This is why
  // we do "composite until" rather than "composite again at".
  if (!mCompositor->GetCompositeUntilTime().IsNull() ||
      mLayerManager->DebugOverlayWantsNextFrame()) {
    ScheduleComposition();
  }

#ifdef COMPOSITOR_PERFORMANCE_WARNING
  TimeDuration executionTime = TimeStamp::Now() - mCompositorScheduler->GetLastComposeTime();
  TimeDuration frameBudget = TimeDuration::FromMilliseconds(15);
  int32_t frameRate = CalculateCompositionFrameRate();
  if (frameRate > 0) {
    frameBudget = TimeDuration::FromSeconds(1.0 / frameRate);
  }
  if (executionTime > frameBudget) {
    printf_stderr("Compositor: Composite execution took %4.1f ms\n",
                  executionTime.ToMilliseconds());
  }
#endif

  // 0 -> Full-tilt composite
  if (gfxPrefs::LayersCompositionFrameRate() == 0
    || mLayerManager->GetCompositor()->GetDiagnosticTypes() & DiagnosticTypes::FLASH_BORDERS) {
    // Special full-tilt composite mode for performance testing
    ScheduleComposition();
  }
  mCompositor->SetCompositionTime(TimeStamp());

  profiler_tracing("Paint", "Composite", TRACING_INTERVAL_END);
}

bool
CompositorBridgeParent::RecvRemotePluginsReady()
{
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  mWaitForPluginsUntil = TimeStamp();
  if (mHaveBlockedForPlugins) {
    mHaveBlockedForPlugins = false;
    ForceComposeToTarget(nullptr);
  } else {
    ScheduleComposition();
  }
  return true;
#else
  NS_NOTREACHED("CompositorBridgeParent::RecvRemotePluginsReady calls "
                "unexpected on this platform.");
  return false;
#endif
}

void
CompositorBridgeParent::ForceComposeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  PROFILER_LABEL("CompositorBridgeParent", "ForceComposeToTarget",
    js::ProfileEntry::Category::GRAPHICS);

  AutoRestore<bool> override(mOverrideComposeReadiness);
  mOverrideComposeReadiness = true;
  mCompositorScheduler->ForceComposeToTarget(aTarget, aRect);
}

uint32_t
CompositorBridgeParent::JihadRequests() const
{
  return mCompositorScheduler ? mCompositorScheduler->JihadRequests() : 0;
}

// Round 138: the fixed and stuck layers read as their columns, not as whole rows (a stuck sidebar
// is ~540 rows of a desktop page but a fifth of its width). JIHAD_GPU_NO_FIXED_COLS=1 restores
// the whole rows (the daemon then asks for them itself, as before).
static bool
JihadFixedCols()
{
  static const bool sOn = !getenv("JIHAD_GPU_NO_FIXED_COLS") && !getenv("JIHAD_GPU_READ_THREAD");
  return sOn;
}

// the rects as column reads: each widened by the shift (the base frame's fixed pixels land shift
// rows above them on a scroll down, below on a scroll up), clipped to the frame; one a full-width
// band already covers is left out
static int
JihadColumnsFor(const gfx::IntRect* aRects, int aN, int aShift, int aW, int aH, const int aBands[2][2], int aOut[4][4])
{
  int n = 0;
  for (int k = 0; k < aN && n < 4; k++) {
    const int y0 = std::max(0, aRects[k].y - std::max(aShift, 0));
    const int y1 = std::min(aH, aRects[k].YMost() + std::max(-aShift, 0));
    const int x0 = std::max(0, aRects[k].x), x1 = std::min(aW, aRects[k].XMost());
    if (y1 <= y0 || x1 <= x0) continue;
    bool covered = false;
    for (int b = 0; b < 2; b++) {
      if (aBands[b][1] > aBands[b][0] && aBands[b][0] <= y0 && y1 <= aBands[b][1]) covered = true;
    }
    if (covered) continue;
    aOut[n][0] = x0; aOut[n][1] = x1; aOut[n][2] = y0; aOut[n][3] = y1;
    n++;
  }
  return n;
}

// the rows the columns span (composed as one band), 0 when there are none
static int
JihadColumnsSpan(const int aCols[4][4], int aN, int* aLo, int* aHi)
{
  int lo = 1 << 30, hi = 0;
  for (int k = 0; k < aN; k++) { lo = std::min(lo, aCols[k][2]); hi = std::max(hi, aCols[k][3]); }
  if (aLo) *aLo = aN ? lo : 0;
  if (aHi) *aHi = aN ? hi : 0;
  return aN && hi > lo ? hi - lo : 0;
}

// the same rect twice is one; overlapping or touching rects that are not kept are joined (a kept
// rect stays exact: a read over it is harmless, a shift of it is not); past aMax, the non-kept pair
// whose hull adds the least area, then kept ones given up (no longer kept) and joined
static int
JihadMergeFixed(gfx::IntRect* aR, gfx::IntRect* aK, int aN, int aMax)
{
  auto remove = [&](int q) { aR[q] = aR[aN - 1]; aK[q] = aK[aN - 1]; aN--; };
  for (bool merged = true; merged && aN > 1;) {
    merged = false;
    for (int p = 0; p < aN && !merged; p++) {
      for (int q = p + 1; q < aN && !merged; q++) {
        if (aR[p].IsEqualEdges(aR[q])) {
          if (!aK[p].IsEqualEdges(aK[q])) aK[p] = gfx::IntRect();
          remove(q); merged = true;
          continue;
        }
        if (!aK[p].IsEmpty() || !aK[q].IsEmpty()) continue;
        gfx::IntRect a = aR[p];
        a.Inflate(1);
        if (a.Intersects(aR[q])) { aR[p] = aR[p].Union(aR[q]); remove(q); merged = true; }
      }
    }
  }
  while (aN > aMax) {
    int bi = -1, bj = -1;
    int64_t best = INT64_MAX;
    for (int pass = 0; pass < 2 && bi < 0; pass++) {
      for (int p = 0; p < aN; p++) {
        for (int q = p + 1; q < aN; q++) {
          if (pass == 0 && (!aK[p].IsEmpty() || !aK[q].IsEmpty())) continue;
          const gfx::IntRect u = aR[p].Union(aR[q]);
          const int64_t grow = (int64_t)u.width * u.height - (int64_t)aR[p].width * aR[p].height - (int64_t)aR[q].width * aR[q].height;
          if (grow < best) { best = grow; bi = p; bj = q; }
        }
      }
    }
    aR[bi] = aR[bi].Union(aR[bj]); aK[bi] = gfx::IntRect();
    remove(bj);
  }
  return aN;
}

// the pixels whose centres a quad covers (GL rasterization, no multisampling)
static gfx::IntRect
JihadCoveredPixels(const gfx::Rect& aR)
{
  const int x0 = (int)ceilf(aR.x - 0.5f), x1 = (int)ceilf(aR.XMost() - 0.5f);
  const int y0 = (int)ceilf(aR.y - 0.5f), y1 = (int)ceilf(aR.YMost() - 0.5f);
  return gfx::IntRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

// Round 142: a column too tall for the band surface's rows left is cut into chunks placed side by
// side under the full-width bands, each composed with the world moved there and read into the
// frame at its own column
struct JihadChunk { int srcX, srcLo, w, h, atX; };

// the chunks' height, or -1 when they do not fit (aOut holds at most 12)
static int
JihadPackColumns(const int aCols[4][4], int aN, int aFullRows, int aCap, int aWidth, JihadChunk* aOut, int* aNOut)
{
  *aNOut = 0;
  if (aN == 0) return 0;
  const int hRem = aCap - aFullRows;
  if (hRem < 16) return -1;
  int cursorX = 0, rows = 0;
  for (int c = 0; c < aN; c++) {
    const int w = aCols[c][1] - aCols[c][0];
    for (int lo = aCols[c][2]; lo < aCols[c][3]; lo += hRem) {
      const int hb = std::min(hRem, aCols[c][3] - lo);
      if (cursorX + w > aWidth || *aNOut == 12) return -1;
      aOut[(*aNOut)++] = { aCols[c][0], lo, w, hb, cursorX };
      rows = std::max(rows, hb);
      cursorX += w;
    }
  }
  return rows;
}

int
CompositorBridgeParent::JihadFixedRectsNow(int aWidth, int aHeight, gfx::IntRect* aOut, gfx::IntRect* aKeep)
{
  Layer* root = mLayerManager ? mLayerManager->GetRoot() : nullptr;
  if (!root) {
    return 0;
  }
  static const bool sNoKeep = getenv("JIHAD_GPU_NO_KEEP") != nullptr;
  static const bool sLog = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
  static uint32_t sLogN = 0;
  const bool logThis = sLog && (sLogN++ % 40) == 0;
  gfx::IntRect r[16], k[16];
  int n = 0, fixedDepth = 0, outerLo = 0, outerHi = 0;
  const gfx::IntRect frame(0, 0, aWidth, aHeight);
  auto isFixed = [&](Layer* l) {
    return l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l));
  };
  // a layer's visible region on the frame, as the pixels its quads cover; false unless its
  // transform keeps rects axis-aligned
  auto addCovered = [&](Layer* x, nsIntRegion& aOutRegion) {
    gfx::Matrix m;
    if (!x->GetEffectiveTransform().Is2D(&m) || !m.PreservesAxisAlignedRectangles()) return false;
    const nsIntRegion visible = x->GetLocalVisibleRegion().ToUnknownRegion();   // held: the iterator points into it
    for (auto it = visible.RectIter(); !it.Done(); it.Next()) {
      const gfx::IntRect& q = it.Get();
      aOutRegion.OrWith(JihadCoveredPixels(x->GetEffectiveTransform().TransformBounds(gfx::Rect(q.x, q.y, q.width, q.height))));
    }
    return true;
  };
  ForEachNode<ForwardIterator>(root,
    [&](Layer* l) {
      const bool fixedHere = isFixed(l);
      if (logThis && n > 0) {
        const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
        if (!vb.IsEmpty()) {
          const gfx::IntRect ob = gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
          for (int i = 0; i < n; i++) {
            if (r[i].Intersects(ob)) {
              fprintf(stderr, "[jihad-gpu]   above rect %d (%d,%d %dx%d): layer %p type %d flags 0x%x fixed %d sticky %d depth %d at %d,%d %dx%d (%u rects)\n",
                      i, r[i].x, r[i].y, r[i].width, r[i].height, l, (int)l->GetType(), (unsigned)l->GetContentFlags(), (int)l->GetIsFixedPosition(),
                      (int)l->GetIsStickyPosition(), fixedDepth, ob.x, ob.y, ob.width, ob.height, (unsigned)l->GetLocalVisibleRegion().GetNumRects());
              break;
            }
          }
        }
      }
      if (fixedDepth == 0 && !fixedHere && n > 0) {
        // Round 141: content painted above a kept rect (outside any fixed subtree) moves over it
        // with the scroll: that rect is not kept
        const Layer::LayerType t = l->GetType();
        if (t == Layer::TYPE_PAINTED || t == Layer::TYPE_COLOR || t == Layer::TYPE_IMAGE || t == Layer::TYPE_CANVAS) {
          const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
          if (!vb.IsEmpty()) {
            const gfx::IntRect ob = gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
            for (int i = 0; i < n; i++) {
              if (!k[i].IsEmpty() && k[i].Intersects(ob)) {
                if (logThis) fprintf(stderr, "[jihad-gpu] keep %d,%d %dx%d dropped: layer %p (type %d) above it at %d,%d %dx%d\n",
                                     k[i].x, k[i].y, k[i].width, k[i].height, l, (int)t, ob.x, ob.y, ob.width, ob.height);
                k[i] = gfx::IntRect();
                r[i].Inflate(1);
                r[i] = r[i].Intersect(frame);
              }
            }
          }
        }
      }
      if (fixedHere) {
        const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
        if (!vb.IsEmpty()) {
          gfx::IntRect ir = gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
          ir.Inflate(1);   // the edges' filtered pixels
          ir = ir.Intersect(frame);
          // a fixed layer inside a kept one is that one's pixels
          bool inOuterKeep = false;
          for (int i = outerLo; fixedDepth > 0 && i < n && i < outerHi; i++) {
            if (!k[i].IsEmpty() && k[i].Contains(ir)) inOuterKeep = true;
          }
          if (!ir.IsEmpty() && !inOuterKeep) {
            // Round 141: an outermost fixed or stuck layer that is opaque over all its pixels (it
            // or its opaque children cover them) is its own pixels whatever scrolls beneath
            gfx::IntRect keep;
            nsIntRegion vis, opaque;
            if (!sNoKeep && fixedDepth == 0 && l->GetEffectiveOpacity() >= 0.999f && !l->GetMaskLayer() &&
                l->GetAncestorMaskLayerCount() == 0 && addCovered(l, vis)) {
              bool ok = true;
              if (l->GetContentFlags() & Layer::CONTENT_OPAQUE) {
                opaque = vis;
              } else {
                for (Layer* c = l->GetFirstChild(); c && ok; c = c->GetNextSibling()) {
                  if ((c->GetContentFlags() & Layer::CONTENT_OPAQUE) && c->GetEffectiveOpacity() >= 0.999f && !c->GetMaskLayer()) {
                    ok = addCovered(c, opaque);
                  }
                }
              }
              // round 142: the layer's covered pixels may be several rects (a gap where nothing is
              // painted shows the page beneath, which moves); each rect is kept on its own when
              // the opaque region holds the whole covered region
              vis.AndWith(frame);
              if (ok && !vis.IsEmpty() && opaque.Contains(vis) && vis.GetNumRects() <= 3) keep = vis.GetBounds();
            }
            if (logThis) {
              gfx::Matrix lm;
              const bool l2d = l->GetEffectiveTransform().Is2D(&lm);
              fprintf(stderr, "[jihad-gpu]   layer 2d %d axis %d, own mask %d, ancestor masks %u, vis bounds %d,%d %dx%d, transform %.3f %.3f %.3f %.3f %.1f %.1f\n",
                      (int)l2d, (int)(l2d && lm.PreservesAxisAlignedRectangles()), (int)(l->GetMaskLayer() != nullptr), (unsigned)l->GetAncestorMaskLayerCount(),
                      vis.GetBounds().x, vis.GetBounds().y, vis.GetBounds().width, vis.GetBounds().height, lm._11, lm._12, lm._21, lm._22, lm._31, lm._32);
              for (Layer* c = l->GetFirstChild(); c; c = c->GetNextSibling()) {
                gfx::Matrix cm;
                const bool c2d = c->GetEffectiveTransform().Is2D(&cm) && cm.PreservesAxisAlignedRectangles();
                const gfx::IntRect cb = c->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
                fprintf(stderr, "[jihad-gpu]   child %p type %d flags 0x%x opacity %.2f mask %d 2d %d visible %d,%d %dx%d\n", c, (int)c->GetType(),
                        (unsigned)c->GetContentFlags(), c->GetEffectiveOpacity(), (int)(c->GetMaskLayer() != nullptr), (int)c2d, cb.x, cb.y, cb.width, cb.height);
              }
              auto dumpRegion = [](const char* aName, const nsIntRegion& aRg) {
                fprintf(stderr, "[jihad-gpu]   %s (%u rects):", aName, (unsigned)aRg.GetNumRects());
                int q = 0;
                for (auto it = aRg.RectIter(); !it.Done() && q < 6; it.Next(), q++) {
                  const gfx::IntRect& rr = it.Get();
                  fprintf(stderr, " %d,%d %dx%d", rr.x, rr.y, rr.width, rr.height);
                }
                fprintf(stderr, "\n");
              };
              dumpRegion("covered", vis);
              dumpRegion("opaque", opaque);
              fprintf(stderr, "[jihad-gpu] fixed rect %d,%d %dx%d from layer %p (type %d, flags 0x%x, opacity %.2f, depth %d): keep %d,%d %dx%d (opaque bounds %d,%d %dx%d)\n",
                      ir.x, ir.y, ir.width, ir.height, l, (int)l->GetType(), (unsigned)l->GetContentFlags(), l->GetEffectiveOpacity(), fixedDepth,
                      keep.x, keep.y, keep.width, keep.height, opaque.GetBounds().x, opaque.GetBounds().y, opaque.GetBounds().width, opaque.GetBounds().height);
            }
            if (n >= 14) n = JihadMergeFixed(r, k, n, 13);
            if (fixedDepth == 0) outerLo = n;
            if (!keep.IsEmpty()) {
              // a sliver (under 32 rows) is read as a column: kept, its strip would be one more band
              for (auto it = vis.RectIter(); !it.Done(); it.Next()) {
                gfx::IntRect q = it.Get();
                if (q.height >= 32) { r[n] = q; k[n] = q; }
                else { q.Inflate(1); r[n] = q.Intersect(frame); k[n] = gfx::IntRect(); }
                n++;
              }
            } else {
              r[n] = ir; k[n] = gfx::IntRect(); n++;
            }
            if (fixedDepth == 0) outerHi = n;
          }
        }
        fixedDepth++;
      }
    },
    [&](Layer* l) { if (isFixed(l)) fixedDepth--; });
  n = JihadMergeFixed(r, k, n, 4);
  for (int i = 0; i < n; i++) { aOut[i] = r[i]; if (aKeep) aKeep[i] = k[i]; }
  return n;
}

// the last composite's rects become the one before's keeps, and this composite's the last
void
CompositorBridgeParent::JihadUpdateLastFixed(int aWidth, int aHeight)
{
  for (int i = 0; i < mJihadLastFixedN; i++) mJihadPrevKeep[i] = mJihadLastKeep[i];
  mJihadPrevKeepN = mJihadLastFixedN;
  mJihadLastFixedN = JihadFixedRectsNow(aWidth, aHeight, mJihadLastFixedRects, mJihadLastKeep);
  static const bool sLog = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
  static uint32_t sN = 0;
  if (sLog && sN++ < 80) {
    fprintf(stderr, "[jihad-gpu] last fixed now:");
    for (int i = 0; i < mJihadLastFixedN; i++) {
      fprintf(stderr, " [%d,%d %dx%d keep %d,%d %dx%d]", mJihadLastFixedRects[i].x, mJihadLastFixedRects[i].y, mJihadLastFixedRects[i].width,
              mJihadLastFixedRects[i].height, mJihadLastKeep[i].x, mJihadLastKeep[i].y, mJihadLastKeep[i].width, mJihadLastKeep[i].height);
    }
    fprintf(stderr, "\n");
  }
}

// Round 141: what a frame shifted by aShift rows does with the fixed and stuck rects: a kept one
// (opaque, and held still: its keep among aRefKeeps) stays where it is and only the rows the shift
// uncovers beside it are read (full width, added to aIv); the others go to aRest (column reads)
static void
JihadPlanKeeps(const gfx::IntRect* aRects, const gfx::IntRect* aKeeps, int aN, const gfx::IntRect* aRefKeeps, int aRefN,
               int aShift, int aH, JihadFrameRequest* aReq, int (*aIv)[2], int* aNIv, gfx::IntRect* aRest, int* aNRest)
{
  aReq->nKeep = 0;
  *aNRest = 0;
  for (int i = 0; i < aN; i++) {
    bool stable = false;
    if (aShift != 0 && !aKeeps[i].IsEmpty()) {
      for (int j = 0; j < aRefN; j++) if (aRefKeeps[j].IsEqualEdges(aKeeps[i])) stable = true;
      static const bool sLog = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
      static uint32_t sN = 0;
      if (sLog && !stable && sN++ < 60) {
        fprintf(stderr, "[jihad-gpu] keep %d,%d %dx%d not stable; the reference keeps:", aKeeps[i].x, aKeeps[i].y, aKeeps[i].width, aKeeps[i].height);
        for (int j = 0; j < aRefN; j++) fprintf(stderr, " %d,%d %dx%d", aRefKeeps[j].x, aRefKeeps[j].y, aRefKeeps[j].width, aRefKeeps[j].height);
        fprintf(stderr, "\n");
      }
    }
    if (stable && aReq->nKeep < 4) {
      const gfx::IntRect& kk = aKeeps[i];
      int* o = aReq->keep[aReq->nKeep++];
      o[0] = kk.x; o[1] = kk.XMost(); o[2] = kk.y; o[3] = kk.YMost();
      // the base frame held the layer where the shifted content now lands beside it
      const int y0 = std::max(0, aShift > 0 ? kk.y - aShift : kk.YMost());
      const int y1 = std::min(aH, aShift > 0 ? kk.y : kk.YMost() - aShift);
      if (y1 > y0) { aIv[*aNIv][0] = y0; aIv[*aNIv][1] = y1; (*aNIv)++; }
      continue;
    }
    aRest[(*aNRest)++] = aRects[i];
  }
}

// row intervals clipped to the frame, overlaps joined, then the closest pair until two remain
static int
JihadMergeIntervals(int (*aIv)[2], int aN, int aH)
{
  int n = 0;
  for (int k = 0; k < aN; k++) {
    const int lo = std::max(0, aIv[k][0]), hi = std::min(aH, aIv[k][1]);
    if (hi > lo) { aIv[n][0] = lo; aIv[n][1] = hi; n++; }
  }
  for (bool merged = true; merged && n > 1;) {
    merged = false;
    for (int p = 0; p < n && !merged; p++) for (int q = p + 1; q < n && !merged; q++) {
      if (aIv[p][0] <= aIv[q][1] && aIv[q][0] <= aIv[p][1]) {
        aIv[p][0] = std::min(aIv[p][0], aIv[q][0]); aIv[p][1] = std::max(aIv[p][1], aIv[q][1]);
        aIv[q][0] = aIv[n-1][0]; aIv[q][1] = aIv[n-1][1]; n--; merged = true;
      }
    }
  }
  while (n > 2) {
    int bi = 0, bj = 1, best = 1 << 30;
    for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) {
      const int gap = std::max(aIv[p][0], aIv[q][0]) - std::min(aIv[p][1], aIv[q][1]);
      if (gap < best) { best = gap; bi = p; bj = q; }
    }
    aIv[bi][0] = std::min(aIv[bi][0], aIv[bj][0]); aIv[bi][1] = std::max(aIv[bi][1], aIv[bj][1]);
    aIv[bj][0] = aIv[n-1][0]; aIv[bj][1] = aIv[n-1][1]; n--;
  }
  if (n == 2 && aIv[1][0] < aIv[0][0]) { std::swap(aIv[0][0], aIv[1][0]); std::swap(aIv[0][1], aIv[1][1]); }
  return n;
}

bool
CompositorBridgeParent::JihadComposeAndRead(JihadFrameRequest* aReq, float aScale, float aOffsetX, float aOffsetY)
{
  // T-509 device test (JIHAD_GPU_TEST_STALL=<n>): the nth job holds the compositor thread 6 s, as a
  // GPU wait that does not come back would; the daemon gives the frame up and goes on in software
  {
    static const int sStallAt = getenv("JIHAD_GPU_TEST_STALL") ? atoi(getenv("JIHAD_GPU_TEST_STALL")) : 0;
    static int sJobsSeen = 0;
    if (sStallAt > 0 && aReq && ++sJobsSeen == sStallAt) {
      fprintf(stderr, "[jihad-gpu] JIHAD_GPU_TEST_STALL: job %d holds the compositor thread 6 s\n", sJobsSeen);
      sleep(6);
    }
  }
  if (aReq && aReq->cancelled) {
    return false;   // T-509: given up while it waited
  }
  // T-503: a verify composite (JIHAD_GPU_VERIFY) changes nothing the next frame reads: the
  // request count, the last scroll and view, the fixed rects and keeps, the diff baseline. It
  // used to consume an anchor move's change, and the next frame then kept stale rows.
  struct JihadVerifyGuard {
    CompositorBridgeParent* b; bool on;
    gfx::Matrix4x4 world; float sx, sy; bool sv; uint32_t seq; bool seqKnown; gfx::IntSize size; bool worldKnown;
    int fixLo, fixHi, fixN, prevN; gfx::IntRect rects[4], keeps[4], prev[4];
    JihadVerifyGuard(CompositorBridgeParent* aB, bool aOn) : b(aB), on(aOn) {
      if (!on) return;
      world = b->mJihadLastWorld; sx = b->mJihadLastScrollX; sy = b->mJihadLastScrollY; sv = b->mJihadLastScrollValid;
      seq = b->mJihadSeqAtLast; seqKnown = b->mJihadSeqKnown; size = b->mJihadLastSize; worldKnown = b->mJihadWorldKnown;
      fixLo = b->mJihadLastFixedLo; fixHi = b->mJihadLastFixedHi; fixN = b->mJihadLastFixedN; prevN = b->mJihadPrevKeepN;
      for (int i = 0; i < 4; i++) { rects[i] = b->mJihadLastFixedRects[i]; keeps[i] = b->mJihadLastKeep[i]; prev[i] = b->mJihadPrevKeep[i]; }
      if (b->mLayerManager) b->mLayerManager->JihadHoldBaseline(true);
    }
    ~JihadVerifyGuard() {
      if (!on) return;
      b->mJihadLastWorld = world; b->mJihadLastScrollX = sx; b->mJihadLastScrollY = sy; b->mJihadLastScrollValid = sv;
      b->mJihadSeqAtLast = seq; b->mJihadSeqKnown = seqKnown; b->mJihadLastSize = size; b->mJihadWorldKnown = worldKnown;
      b->mJihadLastFixedLo = fixLo; b->mJihadLastFixedHi = fixHi; b->mJihadLastFixedN = fixN; b->mJihadPrevKeepN = prevN;
      for (int i = 0; i < 4; i++) { b->mJihadLastFixedRects[i] = rects[i]; b->mJihadLastKeep[i] = keeps[i]; b->mJihadPrevKeep[i] = prev[i]; }
      if (b->mLayerManager) b->mLayerManager->JihadHoldBaseline(false);
    }
  } jihadVerifyGuard(this, aReq && aReq->verify);
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!aReq || !mCompositor || !mLayerManager) {
    return false;
  }
  CompositorOGL* ogl = mCompositor->AsCompositorOGL();
  if (!ogl) {
    return false;
  }
  const TimeStamp tEntry = TimeStamp::Now();
  const int aWidth = aReq->w, aHeight = aReq->h;
  // the card's zoom about the frame's top-left, after the residual visual offset (CSS px)
  gfx::Matrix4x4 world = gfx::Matrix4x4::Scaling(aScale > 0.0f ? aScale : 1.0f,
                                                 aScale > 0.0f ? aScale : 1.0f, 1.0f);
  world.PreTranslate(-aOffsetX, -aOffsetY, 0.0f);
  mLayerManager->JihadSetWorldTransform(world);
  // The window path draws mRenderBounds, which only a layer transaction's target config or a
  // compose-to-target rect sets; the offscreen widget's transactions carry none, so without
  // this BeginFrame saw a zero-area frame and drew nothing (black frames, rounds 6-13).
  // The bounds are the FRAME's size, not the widget's: with a layout viewport wider than the
  // card (980x1202 for a 768x942 card) the world transform scales the page into the frame's
  // corner, and a view the widget's size put the card's rows 260..1202 into the read (the
  // flipped view reads from the bottom) and rendered a third more than the frame.
  const gfx::IntRect whole(0, 0, aWidth, aHeight);
  mLayerManager->UpdateRenderBounds(whole);
  // What UpdateAndRender finds changed (the tree diff and the layers' own damage) is what the
  // daemon reads back. The world transform and the frame's size are not tree properties, so a
  // change of either invalidates everything (and UpdateAndRender would otherwise skip the
  // render as "unchanged"). The compositor clears and redraws the whole frame every time, so
  // the other window target (two composites old) is complete too.
  // the card's scroll, applied by the composition manager to the root scroll frame's content;
  // a change moves every row, so the whole frame is read
  if (mCompositionManager) {
    mCompositionManager->JihadSetScroll(aReq->scrollX, aReq->scrollY, aReq->scrollValid);
  }
  const bool scrollChanged = aReq->scrollValid != mJihadLastScrollValid ||
                             aReq->scrollX != mJihadLastScrollX || aReq->scrollY != mJihadLastScrollY;
  mJihadLastScrollX = aReq->scrollX; mJihadLastScrollY = aReq->scrollY; mJihadLastScrollValid = aReq->scrollValid;
  const bool viewChanged = !mJihadWorldKnown || !(world == mJihadLastWorld) ||
                           mJihadLastSize != gfx::IntSize(aWidth, aHeight);
  if (viewChanged) {
    mLayerManager->AddInvalidRegion(whole);
    mJihadLastWorld = world;
    mJihadLastSize = gfx::IntSize(aWidth, aHeight);
    mJihadWorldKnown = true;
  }
  // Did the tree change since the last composite? Every layer transaction, image update and
  // animation step asks the scheduler for a composite (the request count); nothing else moves
  // the tree. The layer diff cannot tell: it compares shadow transforms, and the card's scroll
  // is one (the content's whole bounds every step). On a frame whose only change is the
  // scroll, the diff is dropped and exactly the bands the daemon asked for are composed and
  // read; the daemon shifts the rest of its buffer in place.
  const uint32_t seqNow = JihadRequests();
  const bool treeChanged = !mJihadSeqKnown || seqNow != mJihadSeqAtLast;
  mJihadSeqAtLast = seqNow; mJihadSeqKnown = true;
  const bool scrollOnly = scrollChanged && !viewChanged && !treeChanged;
  // Round 119: a frame whose tree changed takes the diff first, without a render. When what it
  // changed, the bands the daemon asked for, the entering band of a scroll the page made and the
  // fixed rows fit the band pass, the frame is composed there like a scroll-only one (the band
  // surface: the bands' GPU cost, not the frame's); otherwise the whole path composes it, the
  // diff's region invalidated by hand. JIHAD_GPU_NO_DIFF_FIRST=1 turns it off.
  bool diffFirst = false, diffTaken = false, dfHaveShift = false;
  int dfShift = 0, dfInvLo = 0, dfInvHi = 0, dfFixLo = 0, dfFixHi = 0;
  {
    static const bool sNoDiffFirst = getenv("JIHAD_GPU_NO_DIFF_FIRST") != nullptr;
    if (!aReq->verify && treeChanged && !viewChanged && !sNoDiffFirst && ogl->JihadBandMaxRows() > 0 && aReq->dest &&
        aReq->stride == aWidth * 4 && !getenv("JIHAD_GPU_READ_THREAD") && mCompositionManager &&
        mLayerManager->GetRoot()) {
      mLayerManager->JihadCompensateScroll(aReq->src != nullptr && aReq->engineShift);
      const nsIntRegion dfRegion = JihadDiffNow();
      mLayerManager->JihadCompensateScroll(false);
      diffTaken = true;
      nsIntRegion dfOnFrame;
      dfOnFrame.And(dfRegion, whole);   // round 124: clipped to the frame before the bounds
      const gfx::IntRect cb = dfOnFrame.GetBounds();
      if (!cb.IsEmpty()) {
        dfInvLo = std::max(0, cb.y); dfInvHi = std::min(aHeight, cb.YMost());
        if (dfInvHi < dfInvLo) dfInvHi = dfInvLo;
      }
      float sdx = 0.0f, sdy = 0.0f;
      if (aReq->src && aReq->engineShift && JihadDiffScrollShift(&sdx, &sdy)) {
        dfShift = (int)lroundf(-sdy * (aScale > 0.0f ? aScale : 1.0f));
        dfHaveShift = dfShift > -aHeight && dfShift < aHeight;
      }
      // the fixed rows, from the effective transforms the diff step computed
      {
        int fLo = aHeight, fHi = 0;
        ForEachNode<ForwardIterator>(mLayerManager->GetRoot(), [&](Layer* l) {
          if (!(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
          const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
          if (vb.IsEmpty()) return;
          const gfx::Rect r = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
          const int a = std::max(0, (int)floor(r.y)), b = std::min(aHeight, (int)ceil(r.YMost()));
          if (b > a) { fLo = std::min(fLo, a); fHi = std::max(fHi, b); }
        });
        if (fHi > fLo) { dfFixLo = fLo; dfFixHi = fHi; }
      }
      gfx::IntRect dfRects[4], dfKeeps[4];
      const int dfRectsN = JihadFixedCols() ? JihadFixedRectsNow(aWidth, aHeight, dfRects, dfKeeps) : 0;
      // the rows to read, as at most two intervals
      const int eLo = dfHaveShift && dfShift > 0 ? aHeight - dfShift : 0;
      const int eHi = dfHaveShift ? (dfShift > 0 ? aHeight : -dfShift) : 0;
      const bool fx = !JihadFixedCols() && dfHaveShift && dfShift != 0 && dfFixHi > dfFixLo;   // round 138: else columns
      // round 134: the fixed rows widened by the shift (where the base frame's fixed pixels land)
      int a[9][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2}, {dfInvLo, dfInvHi}, {eLo, eHi},
                      {fx ? dfFixLo - std::max(dfShift, 0) : 0, fx ? dfFixHi + std::max(-dfShift, 0) : 0} };
      // round 141: the kept rects' uncovered rows (a[5..8]), the rest read as columns
      int nA = 5;
      gfx::IntRect dfRest[4];
      int dfNRest = 0;
      const int dfFinalShift = dfHaveShift ? dfShift : aReq->shift;
      if (JihadFixedCols()) {
        JihadPlanKeeps(dfRects, dfKeeps, dfRectsN, mJihadLastKeep, mJihadLastFixedN, dfFinalShift, aHeight, aReq, a, &nA, dfRest, &dfNRest);
      }
      int n = 0; int iv[9][2];
      for (int k = 0; k < nA; k++) {
        if (a[k][1] <= a[k][0]) continue;
        iv[n][0] = std::max(0, a[k][0]); iv[n][1] = std::min(aHeight, a[k][1]);
        if (iv[n][1] > iv[n][0]) n++;
      }
      for (bool merged = true; merged && n > 1;) {
        merged = false;
        for (int p = 0; p < n && !merged; p++) for (int q = p + 1; q < n && !merged; q++) {
          if (iv[p][0] <= iv[q][1] && iv[q][0] <= iv[p][1]) {
            iv[p][0] = std::min(iv[p][0], iv[q][0]); iv[p][1] = std::max(iv[p][1], iv[q][1]);
            iv[q][0] = iv[n-1][0]; iv[q][1] = iv[n-1][1]; n--; merged = true;
          }
        }
      }
      while (n > 2) {
        int bi = 0, bj = 1, best = 1 << 30;
        for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) {
          const int gap = std::max(iv[p][0], iv[q][0]) - std::min(iv[p][1], iv[q][1]);
          if (gap < best) { best = gap; bi = p; bj = q; }
        }
        iv[bi][0] = std::min(iv[bi][0], iv[bj][0]); iv[bi][1] = std::max(iv[bi][1], iv[bj][1]);
        iv[bj][0] = iv[n-1][0]; iv[bj][1] = iv[n-1][1]; n--;
      }
      if (n == 2 && iv[1][0] < iv[0][0]) { std::swap(iv[0][0], iv[1][0]); std::swap(iv[0][1], iv[1][1]); }
      const int total = (n > 0 ? iv[0][1] - iv[0][0] : 0) + (n > 1 ? iv[1][1] - iv[1][0] : 0);
      if (dfHaveShift) {
        aReq->shift = dfShift;
        aReq->frameY = aReq->srcY + dfShift;
        aReq->frameYValid = true;
      }
      // round 138: the fixed and stuck layers' columns, widened by the shift the frame will use
      int dfCols[4][4];
      int dfNCols = 0, dfSpan = 0;
      if (JihadFixedCols()) {
        const int bandsNow[2][2] = { {n > 0 ? iv[0][0] : 0, n > 0 ? iv[0][1] : 0}, {n > 1 ? iv[1][0] : 0, n > 1 ? iv[1][1] : 0} };
        if (dfFinalShift != 0) dfNCols = JihadColumnsFor(dfRest, dfNRest, dfFinalShift, aWidth, aHeight, bandsNow, dfCols);
        JihadChunk dfChunks[12];
        int dfNChunks = 0;
        dfSpan = JihadPackColumns(dfCols, dfNCols, total, ogl->JihadBandRows(), aWidth, dfChunks, &dfNChunks);   // round 142
      }
      if (total > 0 && dfSpan >= 0 && total + dfSpan <= ogl->JihadBandRows()) {
        aReq->lo = iv[0][0]; aReq->hi = iv[0][1];
        aReq->lo2 = n > 1 ? iv[1][0] : 0; aReq->hi2 = n > 1 ? iv[1][1] : 0;
        aReq->nCols = dfNCols;
        memcpy(aReq->cols, dfCols, sizeof(dfCols));
        diffFirst = true;
      } else {
        aReq->nKeep = 0;   // the whole path plans its own
        mLayerManager->AddInvalidRegion(dfRegion);   // the whole path composes what the diff found
      }
      static uint32_t sN = 0;
      if (sN++ < 12 || (sN % 50) == 0) {
        fprintf(stderr, "[jihad-gpu] diff first: changed %d..%d, shift %d%s, fixed %d..%d -> %d rows, %s\n",
                dfInvLo, dfInvHi, dfShift, dfHaveShift ? "" : " (none)", dfFixLo, dfFixHi, total,
                diffFirst ? "the band pass" : "the whole path");
      }
    }
  }
  // Rounds 92/105: a scroll-only frame composes the bands it reads stacked at the top of the
  // window target, the world shifted per band (partial composites: the cost is the bands', not
  // the frame's), the rows kept are moved while the first band composes, one read takes the
  // stack. The fixed rows are the last whole composite's.
  {
    const int bandMax = ogl->JihadBandMaxRows();
    // round 138: a scroll-only frame's columns from the last composite's rects (the diff-first
    // step set its own); their rows are composed as a third band, only the columns read
    if (scrollOnly && !diffFirst && JihadFixedCols()) {
      // round 141: the kept rects' uncovered rows join the bands; the rest are columns
      int iv[6][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
      int nIv = 2;
      gfx::IntRect rest[4];
      int nRest = 0;
      JihadPlanKeeps(mJihadLastFixedRects, mJihadLastKeep, mJihadLastFixedN, mJihadPrevKeep, mJihadPrevKeepN,
                     aReq->shift, aHeight, aReq, iv, &nIv, rest, &nRest);
      if (nIv > 2) {
        const int m = JihadMergeIntervals(iv, nIv, aHeight);
        aReq->lo = m > 0 ? iv[0][0] : 0; aReq->hi = m > 0 ? iv[0][1] : 0;
        aReq->lo2 = m > 1 ? iv[1][0] : 0; aReq->hi2 = m > 1 ? iv[1][1] : 0;
      }
      const int bandsNow[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
      aReq->nCols = aReq->shift != 0 ? JihadColumnsFor(rest, nRest, aReq->shift, aWidth, aHeight, bandsNow, aReq->cols) : 0;
    }
    const bool b1 = aReq->hi > aReq->lo, b2 = aReq->hi2 > aReq->lo2;   // after round 141's strips joined them
    // round 142: the columns packed in chunks beside each other under the full-width bands
    const int fullAsked = (b1 ? std::min(aHeight, aReq->hi) - std::max(0, aReq->lo) : 0) + (b2 ? std::min(aHeight, aReq->hi2) - std::max(0, aReq->lo2) : 0);
    JihadChunk chunks[12];
    int nChunks = 0;
    const int chunkRows = JihadPackColumns(aReq->cols, aReq->nCols, fullAsked, ogl->JihadBandRows(), aWidth, chunks, &nChunks);
    const bool fits = chunkRows >= 0 && fullAsked + chunkRows <= bandMax;
    if ((scrollOnly || diffFirst) && bandMax > 0 && (b1 || b2) && fits && aReq->dest && aReq->stride == aWidth * 4 &&
        !getenv("JIHAD_GPU_READ_THREAD")) {
      // Both bands stacked in the band target (band 2 under band 1), composed one after the
      // other, read ONCE into the compositor's scratch rows and split from there: a glReadPixels
      // call costs ~4 ms of its own on this driver, whatever the rows.
      const int bands[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
      int hs[2] = {0, 0}, top = 0;
      for (int b = 0; b < 2; b++) { const int lo = std::max(0, bands[b][0]), hi = std::min(aHeight, bands[b][1]); hs[b] = hi > lo ? hi - lo : 0; top += hs[b]; }
      const int fullRows = hs[0] + hs[1];   // read whole; the chunks under them only in their columns
      top += chunkRows;
      // the band pass: its own small surface and target when available (round 106), else the
      // stack at the top of the frame-sized target (round 105)
      const TimeStamp tp0 = TimeStamp::Now();
      const int passRows = (top > 0 && top <= ogl->JihadBandRows()) ? ogl->JihadBeginBandPass(aWidth) : 0;
      const double msBegin = (TimeStamp::Now() - tp0).ToMilliseconds();
      const gfx::IntRect passBounds = passRows ? gfx::IntRect(0, 0, aWidth, passRows) : whole;
      bool ok = top > 0 && top <= bandMax;
      TimeStamp tb0 = TimeStamp::Now();
      double msCompose = 0, msRead = 0, msShift = 0;
      int at = 0;
      for (int b = 0; b < 2 && ok; b++) {
        if (!hs[b]) continue;
        const int lo = std::max(0, bands[b][0]), hb = hs[b];
        gfx::Matrix4x4 wb = world;
        wb.PostTranslate(0.0f, (float)(at - lo), 0.0f);   // frame row lo -> stacked row at
        mLayerManager->JihadSetWorldTransform(wb);
        mLayerManager->UpdateRenderBounds(passBounds);    // the target and viewport are the surface's size
        mLayerManager->JihadIgnoreDiff(true);
        mLayerManager->JihadResetLastInvalid();
        mLayerManager->JihadSetBand(gfx::IntRect(0, at, aWidth, hb), gfx::IntRect());
        if (!passRows) ogl->JihadSelectTarget();
        TimeStamp c0 = TimeStamp::Now();
        sJihadInCompose = true;
        ForceComposeToTarget(nullptr, nullptr);
        sJihadInCompose = false;
        msCompose += (TimeStamp::Now() - c0).ToMilliseconds();
        at += hb;
      }
      // round 142: the column chunks, each with the world moved so its rows land at (atX, fullRows)
      for (int c = 0; c < nChunks && ok; c++) {
        const JihadChunk& ch = chunks[c];
        gfx::Matrix4x4 wb = world;
        wb.PostTranslate((float)(ch.atX - ch.srcX), (float)(fullRows - ch.srcLo), 0.0f);
        mLayerManager->JihadSetWorldTransform(wb);
        mLayerManager->UpdateRenderBounds(passBounds);
        mLayerManager->JihadIgnoreDiff(true);
        mLayerManager->JihadResetLastInvalid();
        mLayerManager->JihadSetBand(gfx::IntRect(ch.atX, fullRows, ch.w, ch.h), gfx::IntRect());
        if (!passRows) ogl->JihadSelectTarget();
        TimeStamp c0 = TimeStamp::Now();
        sJihadInCompose = true;
        ForceComposeToTarget(nullptr, nullptr);
        sJihadInCompose = false;
        msCompose += (TimeStamp::Now() - c0).ToMilliseconds();
      }
      // the rows kept, moved while the GPU works on the bands
      {
        ogl->gl()->fFlush();
        TimeStamp c1 = TimeStamp::Now();
        if (aReq->cancelled) ok = false; else ogl->JihadShiftRowsNow(aReq);   // T-509: given up, dest untouched
        msShift = (TimeStamp::Now() - c1).ToMilliseconds();
      }
      if (ok) {
        TimeStamp r0 = TimeStamp::Now();
        uint8_t* scratch = fullRows ? ogl->JihadBandScratch(aWidth * 4 * fullRows) : nullptr;
        if (fullRows) ok = scratch && ogl->JihadReadFrame(scratch, aReq->stride, aWidth, passRows ? passRows : aHeight, 0, fullRows);
        if (ok && fullRows) {
          int from = 0;
          for (int b = 0; b < 2; b++) {
            if (!hs[b]) continue;
            memcpy(aReq->dest + (size_t)std::max(0, bands[b][0]) * aReq->stride, scratch + (size_t)from * aReq->stride, (size_t)hs[b] * aReq->stride);
            from += hs[b];
          }
        }
        // round 142: the column chunks, into the frame at their own columns
        for (int c = 0; c < nChunks && ok; c++) {
          const JihadChunk& ch = chunks[c];
          ok = ogl->JihadReadColumns(aReq->dest, aReq->stride, passRows ? passRows : aHeight, ch.atX, ch.atX + ch.w,
                                     fullRows, ch.h, ch.srcLo, ch.srcX);
        }
        msRead = (TimeStamp::Now() - r0).ToMilliseconds();
      }
      const TimeStamp te0 = TimeStamp::Now();
      if (passRows) ogl->JihadEndBandPass();
      const double msEnd = (TimeStamp::Now() - te0).ToMilliseconds();
      const double msEntry = (tp0 - tEntry).ToMilliseconds();
      mLayerManager->JihadIgnoreDiff(false);
      mLayerManager->JihadSetWorldTransform(world);
      mLayerManager->UpdateRenderBounds(whole);
      mLayerManager->JihadSetBand(gfx::IntRect(), gfx::IntRect());
      ogl->JihadSelectTarget();
      aReq->invLo = diffFirst ? dfInvLo : 0; aReq->invHi = diffFirst ? dfInvHi : 0;
      aReq->scrollOnly = !diffFirst;
      // round 127: the fixed rows as of this frame (a sticky element may have become stuck during
      // the pan), from the effective transforms at the frame's own world
      if (Layer* root = mLayerManager->GetRoot()) {
        root->ComputeEffectiveTransforms(world);
        int fLo = aHeight, fHi = 0;
        static const bool sFixLog = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
        static uint32_t sFixN = 0;
        const bool logThis = sFixLog && (sFixN++ % 20) == 0;
        ForEachNode<ForwardIterator>(root, [&](Layer* l) {
          if (!(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
          const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
          if (vb.IsEmpty()) return;
          const gfx::Rect r = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
          const int a = std::max(0, (int)floor(r.y)), b = std::min(aHeight, (int)ceil(r.YMost()));
          if (logThis) {
            fprintf(stderr, "[jihad-gpu] fixed rows from layer %p (type %d, fixed %d, sticky %d, stuck %d, parent %p, flags 0x%x, opacity %.2f, rects %u): visible %d,%d %dx%d -> x %.0f..%.0f rows %d..%d\n",
                    l, (int)l->GetType(), (int)l->GetIsFixedPosition(), (int)l->GetIsStickyPosition(),
                    (int)(mCompositionManager && mCompositionManager->JihadIsStuck(l)), l->GetParent(),
                    (unsigned)l->GetContentFlags(), l->GetEffectiveOpacity(), (unsigned)l->GetLocalVisibleRegion().GetNumRects(),
                    vb.x, vb.y, vb.width, vb.height, r.x, r.XMost(), a, b);
          }
          if (b > a) { fLo = std::min(fLo, a); fHi = std::max(fHi, b); }
        });
        if (fHi > fLo) { mJihadLastFixedLo = fLo; mJihadLastFixedHi = fHi; } else { mJihadLastFixedLo = mJihadLastFixedHi = 0; }
        JihadUpdateLastFixed(aWidth, aHeight);   // rounds 138/141
      } else if (diffFirst) {
        mJihadLastFixedLo = dfFixLo; mJihadLastFixedHi = dfFixHi;
      }
      aReq->fixedLo = mJihadLastFixedLo; aReq->fixedHi = mJihadLastFixedHi;
      aReq->composed = true;
      aReq->ok = ok;
      static uint32_t sBandFrames = 0;
      if (sBandFrames < 12 || (sBandFrames % 10) == 0) {
        fprintf(stderr, "[jihad-gpu] compositor thread: band frame %u: rows %d..%d and %d..%d, %d columns in %d chunks (%d rows), %d kept, entry %.1f ms, surface in %.1f ms, compose %.1f ms, shift %.1f ms, read %.1f ms, surface out %.1f ms, all %.1f ms, ok=%d\n",
                sBandFrames, aReq->lo, aReq->hi, aReq->lo2, aReq->hi2, aReq->nCols, nChunks, chunkRows, aReq->nKeep, msEntry, msBegin, msCompose, msShift, msRead, msEnd, (TimeStamp::Now() - tEntry).ToMilliseconds(), (int)ok);
      }
      ++sBandFrames;
      aReq->done(aReq);
      return true;
    }
  }
  ogl->JihadUseFrameSurface();   // a whole (or banded-in-place) composite: the frame's surface (round 107)
  mLayerManager->JihadIgnoreDiff(scrollOnly);
  // the texture names retired since the last frame go now, before this frame's GPU work
  ogl->JihadEnsureTargetSize(aWidth, aHeight);   // round 101: the targets at the frame's size
  gl::JihadFlushRetiredTextures(ogl->gl());
  mLayerManager->JihadResetLastInvalid();
  // the rows the daemon asked for are composed with what changed, and only those (partial)
  gfx::IntRect colBand;   // round 138: the columns' rows composed too (round 141: and the kept rects' strips)
  if (JihadFixedCols() && aReq->shift != 0 && mJihadLastFixedN > 0) {
    int pre[4][4];
    int iv[4][2];
    int nIv = 0;
    gfx::IntRect rest[4];
    int nRest = 0;
    JihadPlanKeeps(mJihadLastFixedRects, mJihadLastKeep, mJihadLastFixedN, mJihadPrevKeep, mJihadPrevKeepN,
                   aReq->shift, aHeight, aReq, iv, &nIv, rest, &nRest);
    const int bandsNow[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
    const int pn = JihadColumnsFor(rest, nRest, aReq->shift, aWidth, aHeight, bandsNow, pre);
    int sLo = 0, sHi = 0;
    if (JihadColumnsSpan(pre, pn, &sLo, &sHi)) colBand = gfx::IntRect(0, sLo, aWidth, sHi - sLo);
    for (int i = 0; i < nIv; i++) colBand = colBand.Union(gfx::IntRect(0, iv[i][0], aWidth, iv[i][1] - iv[i][0]));
    aReq->nKeep = 0;   // planned again after the compose
  }
  mLayerManager->JihadSetBand(aReq->hi > aReq->lo ? gfx::IntRect(0, aReq->lo, aWidth, aReq->hi - aReq->lo) : gfx::IntRect(),
                              aReq->hi2 > aReq->lo2 ? gfx::IntRect(0, aReq->lo2, aWidth, aReq->hi2 - aReq->lo2) : gfx::IntRect(),
                              colBand);
  ogl->JihadSelectTarget();
  mLayerManager->JihadCompensateScroll(!diffTaken && aReq->src != nullptr && aReq->engineShift);   // round 115
  TimeStamp t0 = TimeStamp::Now();
  sJihadInCompose = true;
  ForceComposeToTarget(nullptr, nullptr);
  sJihadInCompose = false;
  TimeStamp t1 = TimeStamp::Now();
  mLayerManager->JihadIgnoreDiff(false);
  mLayerManager->JihadCompensateScroll(false);
  // round 115: a scroll the page made, seen by the diff as the root-content layer's translation:
  // the kept rows are shifted by it (from src), the entering band and the fixed rows read
  int engineShiftRows = 0;
  bool haveEngineShift = false;
  {
    float sdx = 0.0f, sdy = 0.0f;
    if (diffTaken) {
      haveEngineShift = dfHaveShift;   // round 119: the diff-first step measured it (and set the request)
      engineShiftRows = dfShift;
    } else if (aReq->src && aReq->engineShift && JihadDiffScrollShift(&sdx, &sdy)) {
      engineShiftRows = (int)lroundf(-sdy * (aScale > 0.0f ? aScale : 1.0f));
      haveEngineShift = engineShiftRows > -aHeight && engineShiftRows < aHeight;
      if (haveEngineShift) {
        aReq->shift = engineShiftRows;
        aReq->frameY = aReq->srcY + engineShiftRows;
        aReq->frameYValid = true;
      }
    }
  }
  const gfx::IntRect changed = mLayerManager->JihadLastChanged();   // what the tree changed
  int invLo = 0, invHi = 0;
  if (!changed.IsEmpty()) {
    invLo = std::max(0, changed.y);
    invHi = std::min(aHeight, changed.YMost());
    if (invHi < invLo) invHi = invLo;
  }
  aReq->scrollOnly = scrollOnly;
  // the rows fixed and sticky layers cover, after this composite's effective transforms: the
  // daemon reads them again on every scroll step (they do not move with the content)
  {
    int fLo = aHeight, fHi = 0;
    if (Layer* root = mLayerManager->GetRoot()) {
      static const bool sFixLogW = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
      static uint32_t sFixNW = 0;
      const bool logThisW = sFixLogW && (sFixNW++ % 20) == 0;
      ForEachNode<ForwardIterator>(root, [&](Layer* l) {
        if (!(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
        const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
        if (vb.IsEmpty()) return;
        if (logThisW) {
          const gfx::Rect rr = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
          fprintf(stderr, "[jihad-gpu] fixed rows (whole path) from layer %p (type %d, fixed %d, sticky %d, stuck %d): visible %d,%d %dx%d -> rows %.0f..%.0f\n",
                  l, (int)l->GetType(), (int)l->GetIsFixedPosition(), (int)l->GetIsStickyPosition(),
                  (int)(mCompositionManager && mCompositionManager->JihadIsStuck(l)), vb.x, vb.y, vb.width, vb.height, rr.y, rr.YMost());
        }
        // the effective transform may or may not carry the world transform's scale (the card's
        // zoom); the rows reported are the union of both readings, so never too few
        // the effective transform carries the world transform (ComputeEffectiveTransforms(world))
        const gfx::Rect r = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
        {
          static uint32_t sN = 0;
          if ((sN++ % 50) == 0) {
            const gfx::Matrix4x4 e = l->GetEffectiveTransform();
            fprintf(stderr, "[jihad-gpu] fixed layer %p (parent %p): visible %d,%d %dx%d, effective translation %.1f,%.1f scale %.3f -> rows %.0f..%.0f\n",
                    l, l->GetParent(), vb.x, vb.y, vb.width, vb.height, e._41, e._42, e._11, r.y, r.YMost());
          }
        }
        const int a = std::max(0, (int)floor(r.y)), b = std::min(aHeight, (int)ceil(r.YMost()));
        if (b > a) { fLo = std::min(fLo, a); fHi = std::max(fHi, b); }
      });
    }
    if (fHi > fLo) { aReq->fixedLo = fLo; aReq->fixedHi = fHi; } else { aReq->fixedLo = aReq->fixedHi = 0; }
    mJihadLastFixedLo = aReq->fixedLo; mJihadLastFixedHi = aReq->fixedHi;
  }
  // round 138: the rects the columns come from: this composite's, and the last one's where the
  // layers were then (round 141: this composite's keeps, held still since the last, stay put)
  gfx::IntRect colRects[8], colKeeps[8];
  int colRectsN = 0;
  if (JihadFixedCols()) {
    gfx::IntRect lastR[4];
    const int lastN = mJihadLastFixedN;
    for (int k = 0; k < lastN; k++) lastR[k] = mJihadLastFixedRects[k];
    JihadUpdateLastFixed(aWidth, aHeight);   // prev = last, last = now
    for (int k = 0; k < mJihadLastFixedN; k++) { colRects[colRectsN] = mJihadLastFixedRects[k]; colKeeps[colRectsN] = mJihadLastKeep[k]; colRectsN++; }
    for (int k = 0; k < lastN; k++) {
      bool same = false;
      for (int j = 0; j < mJihadLastFixedN; j++) if (lastR[k].IsEqualEdges(mJihadLastFixedRects[j])) same = true;
      if (!same) { colRects[colRectsN] = lastR[k]; colKeeps[colRectsN] = gfx::IntRect(); colRectsN++; }
    }
    colRectsN = JihadMergeFixed(colRects, colKeeps, colRectsN, 4);
  }
  // the rows read: the bands asked for and the change, as at most two intervals (overlaps
  // merged, then the closest pair, until two remain)
  gfx::IntRect wholeRest[4];
  int nWholeRest = 0;
  {
    const int eLo = haveEngineShift && engineShiftRows > 0 ? aHeight - engineShiftRows : 0;
    const int eHi = haveEngineShift ? (engineShiftRows > 0 ? aHeight : -engineShiftRows) : 0;
    const bool fx = !JihadFixedCols() && haveEngineShift && engineShiftRows != 0 && aReq->fixedHi > aReq->fixedLo;   // round 138: else columns
    int a[9][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2}, {invLo, invHi}, {eLo, eHi},
                    {fx ? aReq->fixedLo - std::max(engineShiftRows, 0) : 0, fx ? aReq->fixedHi + std::max(-engineShiftRows, 0) : 0} };
    // round 141: the kept rects' uncovered rows (a[5..8]); the rest are columns below
    int nA = 5;
    nWholeRest = 0;
    if (JihadFixedCols() && colRectsN > 0) {
      JihadPlanKeeps(colRects, colKeeps, colRectsN, mJihadPrevKeep, mJihadPrevKeepN, aReq->shift, aHeight, aReq, a, &nA, wholeRest, &nWholeRest);
    }
    int n = 0; int iv[9][2];
    for (int k = 0; k < nA; k++) {
      if (a[k][1] <= a[k][0]) continue;
      iv[n][0] = std::max(0, a[k][0]); iv[n][1] = std::min(aHeight, a[k][1]);
      if (iv[n][1] > iv[n][0]) n++;
    }
    for (bool merged = true; merged && n > 1;) {
      merged = false;
      for (int p = 0; p < n && !merged; p++) for (int q = p + 1; q < n && !merged; q++) {
        if (iv[p][0] <= iv[q][1] && iv[q][0] <= iv[p][1]) {
          iv[p][0] = std::min(iv[p][0], iv[q][0]); iv[p][1] = std::max(iv[p][1], iv[q][1]);
          iv[q][0] = iv[n-1][0]; iv[q][1] = iv[n-1][1]; n--; merged = true;
        }
      }
    }
    while (n > 2) {
      int bi = 0, bj = 1, best = 1 << 30;
      for (int p = 0; p < n; p++) for (int q = p + 1; q < n; q++) {
        const int gap = std::max(iv[p][0], iv[q][0]) - std::min(iv[p][1], iv[q][1]);
        if (gap < best) { best = gap; bi = p; bj = q; }
      }
      iv[bi][0] = std::min(iv[bi][0], iv[bj][0]); iv[bi][1] = std::max(iv[bi][1], iv[bj][1]);
      iv[bj][0] = iv[n-1][0]; iv[bj][1] = iv[n-1][1]; n--;
    }
    if (n == 2 && iv[1][0] < iv[0][0]) { std::swap(iv[0][0], iv[1][0]); std::swap(iv[0][1], iv[1][1]); }
    aReq->lo = n > 0 ? iv[0][0] : 0; aReq->hi = n > 0 ? iv[0][1] : 0;
    aReq->lo2 = n > 1 ? iv[1][0] : 0; aReq->hi2 = n > 1 ? iv[1][1] : 0;
  }
  // round 138: the columns, within the rows this composite drew (the target's other rows are two
  // composites old)
  aReq->nCols = 0;
  if (JihadFixedCols() && aReq->shift != 0 && nWholeRest > 0) {
    int cols[4][4];
    const int bandsNow[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
    const int cn = JihadColumnsFor(wholeRest, nWholeRest, aReq->shift, aWidth, aHeight, bandsNow, cols);
    const gfx::IntRect drawn = mLayerManager->JihadLastInvalid();
    for (int c = 0; c < cn; c++) {
      const int y0 = std::max(cols[c][2], drawn.y), y1 = std::min(cols[c][3], drawn.YMost());
      if (y1 <= y0) continue;
      int* o = aReq->cols[aReq->nCols++];
      o[0] = cols[c][0]; o[1] = cols[c][1]; o[2] = y0; o[3] = y1;
    }
  }
  const int lo = aReq->lo, hi = aReq->hi;
  aReq->invLo = invLo; aReq->invHi = invHi;
  // the log's values now: once the read hands the frame over, aReq belongs to the daemon
  const int logLo2 = aReq->lo2, logHi2 = aReq->hi2, logFixedLo = aReq->fixedLo, logFixedHi = aReq->fixedHi, logCols = aReq->nCols, logKeep = aReq->nKeep;
  const bool logScrollOnly = aReq->scrollOnly;
  const bool started = ogl->JihadReadAsync(aReq);
  static uint32_t sFrames = 0;
  static const bool sTimeline = getenv("JIHAD_GPU_TIMELINE") != nullptr;   // T-515 device rounds: every frame
  if (sFrames < 20 || (sFrames % 50) == 0 || sTimeline) {
    fprintf(stderr, "[jihad-gpu] compositor thread: compose %.1f ms, handoff %.1f ms (frame %u, rows %d..%d and %d..%d of %d, %d columns, %d kept, changed %d..%d, fixed %d..%d%s%s, started=%d)\n",
            (t1 - t0).ToMilliseconds(), (TimeStamp::Now() - t1).ToMilliseconds(), sFrames,
            lo, hi, logLo2, logHi2, aHeight, logCols, logKeep, invLo, invHi, logFixedLo, logFixedHi,
            treeChanged ? ", tree changed" : "", logScrollOnly ? ", scroll only" : "", (int)started);
  }
  ++sFrames;
  return started;
}

nsIntRegion
CompositorBridgeParent::JihadDiffNow()
{
  // the steps of CompositeToTarget before its render: shadow properties from the layers, the
  // async transforms (the card's scroll, animations), then the diff
  mLayerManager->BeginTransaction();
  SetShadowProperties(mLayerManager->GetRoot());
  mCompositionManager->ComputeRotation();
  TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
  mCompositionManager->TransformShadowTree(time, mVsyncRate);
  nsIntRegion changed = mLayerManager->JihadDiffOnly();
  mLayerManager->JihadEndTransactionNoComposite();
  return changed;
}

PAPZCTreeManagerParent*
CompositorBridgeParent::AllocPAPZCTreeManagerParent(const uint64_t& aLayersId)
{
  // The main process should pass in 0 because we assume mRootLayerTreeID
  MOZ_ASSERT(aLayersId == 0);

  // This message doubles as initialization
  MOZ_ASSERT(!mApzcTreeManager);
  mApzcTreeManager = new APZCTreeManager();

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& state = sIndirectLayerTrees[mRootLayerTreeID];
  MOZ_ASSERT(state.mParent);
  MOZ_ASSERT(!state.mApzcTreeManagerParent);
  state.mApzcTreeManagerParent = new APZCTreeManagerParent(mRootLayerTreeID, state.mParent->GetAPZCTreeManager());

  return state.mApzcTreeManagerParent;
}

bool
CompositorBridgeParent::DeallocPAPZCTreeManagerParent(PAPZCTreeManagerParent* aActor)
{
  delete aActor;
  return true;
}

PAPZParent*
CompositorBridgeParent::AllocPAPZParent(const uint64_t& aLayersId)
{
  // The main process should pass in 0 because we assume mRootLayerTreeID
  MOZ_ASSERT(aLayersId == 0);

  RemoteContentController* controller = new RemoteContentController();

  // Increment the controller's refcount before we return it. This will keep the
  // controller alive until it is released by IPDL in DeallocPAPZParent.
  controller->AddRef();

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& state = sIndirectLayerTrees[mRootLayerTreeID];
  MOZ_ASSERT(!state.mController);
  state.mController = controller;

  return controller;
}

bool
CompositorBridgeParent::DeallocPAPZParent(PAPZParent* aActor)
{
  RemoteContentController* controller = static_cast<RemoteContentController*>(aActor);
  controller->Release();
  return true;
}

bool
CompositorBridgeParent::RecvAsyncPanZoomEnabled(const uint64_t& aLayersId, bool* aHasAPZ)
{
  // The main process should pass in 0 because we assume mRootLayerTreeID
  MOZ_ASSERT(aLayersId == 0);
  *aHasAPZ = AsyncPanZoomEnabled();
  return true;
}

RefPtr<APZCTreeManager>
CompositorBridgeParent::GetAPZCTreeManager()
{
  return mApzcTreeManager;
}

bool
CompositorBridgeParent::CanComposite()
{
  return mLayerManager &&
         mLayerManager->GetRoot() &&
         !mPaused;
}

void
CompositorBridgeParent::ScheduleRotationOnCompositorThread(const TargetConfig& aTargetConfig,
                                                     bool aIsFirstPaint)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  if (!aIsFirstPaint &&
      !mCompositionManager->IsFirstPaint() &&
      mCompositionManager->RequiresReorientation(aTargetConfig.orientation())) {
    if (mForceCompositionTask != nullptr) {
      mForceCompositionTask->Cancel();
    }
    RefPtr<CancelableRunnable> task =
      NewCancelableRunnableMethod(this, &CompositorBridgeParent::ForceComposition);
    mForceCompositionTask = task;
    ScheduleTask(task.forget(), gfxPrefs::OrientationSyncMillis());
  }
}

void
CompositorBridgeParent::ShadowLayersUpdated(LayerTransactionParent* aLayerTree,
                                            const uint64_t& aTransactionId,
                                            const TargetConfig& aTargetConfig,
                                            const InfallibleTArray<PluginWindowData>& aUnused,
                                            bool aIsFirstPaint,
                                            bool aScheduleComposite,
                                            uint32_t aPaintSequenceNumber,
                                            bool aIsRepeatTransaction,
                                            int32_t aPaintSyncId,
                                            bool aHitTestUpdate)
{
  ScheduleRotationOnCompositorThread(aTargetConfig, aIsFirstPaint);

  // Instruct the LayerManager to update its render bounds now. Since all the orientation
  // change, dimension change would be done at the stage, update the size here is free of
  // race condition.
  mLayerManager->UpdateRenderBounds(aTargetConfig.naturalBounds());
  mLayerManager->SetRegionToClear(aTargetConfig.clearRegion());
  mLayerManager->GetCompositor()->SetScreenRotation(aTargetConfig.rotation());

  mCompositionManager->Updated(aIsFirstPaint, aTargetConfig, aPaintSyncId);
  Layer* root = aLayerTree->GetRoot();
  mLayerManager->SetRoot(root);

  if (mApzcTreeManager && !aIsRepeatTransaction && aHitTestUpdate) {
    AutoResolveRefLayers resolve(mCompositionManager);

    mApzcTreeManager->UpdateHitTestingTree(mRootLayerTreeID, root, aIsFirstPaint,
        mRootLayerTreeID, aPaintSequenceNumber);
  }

  // The transaction ID might get reset to 1 if the page gets reloaded, see
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1145295#c41
  // Otherwise, it should be continually increasing.
  MOZ_ASSERT(aTransactionId == 1 || aTransactionId > mPendingTransaction);
  mPendingTransaction = aTransactionId;

  if (root) {
    SetShadowProperties(root);
  }
  if (aScheduleComposite) {
    ScheduleComposition();
    if (mPaused) {
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
  mLayerManager->NotifyShadowTreeTransaction();
}

void
CompositorBridgeParent::ForceComposite(LayerTransactionParent* aLayerTree)
{
  ScheduleComposition();
}

bool
CompositorBridgeParent::SetTestSampleTime(LayerTransactionParent* aLayerTree,
                                          const TimeStamp& aTime)
{
  if (aTime.IsNull()) {
    return false;
  }

  mIsTesting = true;
  mTestTime = aTime;

  bool testComposite = mCompositionManager &&
                       mCompositorScheduler->NeedsComposite();

  // Update but only if we were already scheduled to animate
  if (testComposite) {
    AutoResolveRefLayers resolve(mCompositionManager);
    bool requestNextFrame = mCompositionManager->TransformShadowTree(aTime, mVsyncRate);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is wating for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }

  return true;
}

void
CompositorBridgeParent::LeaveTestMode(LayerTransactionParent* aLayerTree)
{
  mIsTesting = false;
}

void
CompositorBridgeParent::ApplyAsyncProperties(LayerTransactionParent* aLayerTree)
{
  // NOTE: This should only be used for testing. For example, when mIsTesting is
  // true or when called from test-only methods like
  // LayerTransactionParent::RecvGetAnimationTransform.

  // Synchronously update the layer tree
  if (aLayerTree->GetRoot()) {
    AutoResolveRefLayers resolve(mCompositionManager);
    SetShadowProperties(mLayerManager->GetRoot());

    TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
    bool requestNextFrame =
      mCompositionManager->TransformShadowTree(time, mVsyncRate,
        AsyncCompositionManager::TransformsToSkip::APZ);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is waiting for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
}

bool
CompositorBridgeParent::RecvGetFrameUniformity(FrameUniformityData* aOutData)
{
  mCompositionManager->GetFrameUniformity(aOutData);
  return true;
}

bool
CompositorBridgeParent::RecvRequestOverfill()
{
  uint32_t overfillRatio = mCompositor->GetFillRatio();
  Unused << SendOverfill(overfillRatio);
  return true;
}

void
CompositorBridgeParent::FlushApzRepaints(const LayerTransactionParent* aLayerTree)
{
  MOZ_ASSERT(mApzcTreeManager);
  uint64_t layersId = aLayerTree->GetId();
  if (layersId == 0) {
    // The request is coming from the parent-process layer tree, so we should
    // use the compositor's root layer tree id.
    layersId = mRootLayerTreeID;
  }
  RefPtr<CompositorBridgeParent> self = this;
  APZThreadUtils::RunOnControllerThread(NS_NewRunnableFunction([=] () {
    self->mApzcTreeManager->FlushApzRepaints(layersId);
  }));
}

void
CompositorBridgeParent::GetAPZTestData(const LayerTransactionParent* aLayerTree,
                                       APZTestData* aOutData)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  *aOutData = sIndirectLayerTrees[mRootLayerTreeID].mApzTestData;
}

void
CompositorBridgeParent::SetConfirmedTargetAPZC(const LayerTransactionParent* aLayerTree,
                                         const uint64_t& aInputBlockId,
                                         const nsTArray<ScrollableLayerGuid>& aTargets)
{
  if (!mApzcTreeManager) {
    return;
  }
  // Need to specifically bind this since it's overloaded.
  void (APZCTreeManager::*setTargetApzcFunc)
        (uint64_t, const nsTArray<ScrollableLayerGuid>&) =
        &APZCTreeManager::SetTargetAPZC;
  RefPtr<Runnable> task = NewRunnableMethod
        <uint64_t, StoreCopyPassByConstLRef<nsTArray<ScrollableLayerGuid>>>
        (mApzcTreeManager.get(), setTargetApzcFunc, aInputBlockId, aTargets);
  APZThreadUtils::RunOnControllerThread(task.forget());

}

void
CompositorBridgeParent::InitializeLayerManager(const nsTArray<LayersBackend>& aBackendHints)
{
  NS_ASSERTION(!mLayerManager, "Already initialised mLayerManager");
  NS_ASSERTION(!mCompositor,   "Already initialised mCompositor");

  mCompositor = NewCompositor(aBackendHints);
  if (!mCompositor) {
    return;
  }

  mLayerManager = new LayerManagerComposite(mCompositor);

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[mRootLayerTreeID].mLayerManager = mLayerManager;
}

RefPtr<Compositor>
CompositorBridgeParent::NewCompositor(const nsTArray<LayersBackend>& aBackendHints)
{
  for (size_t i = 0; i < aBackendHints.Length(); ++i) {
    RefPtr<Compositor> compositor;
    if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL) {
      compositor = new CompositorOGL(this,
                                     mWidget,
                                     mEGLSurfaceSize.width,
                                     mEGLSurfaceSize.height,
                                     mUseExternalSurfaceSize);
    } else if (aBackendHints[i] == LayersBackend::LAYERS_BASIC) {
#ifdef MOZ_WIDGET_GTK
      if (gfxVars::UseXRender()) {
        compositor = new X11BasicCompositor(this, mWidget);
      } else
#endif
      {
        compositor = new BasicCompositor(this, mWidget);
      }
#ifdef XP_WIN
    } else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11) {
      compositor = new CompositorD3D11(this, mWidget);
#endif
    }
    nsCString failureReason;
    if (compositor && compositor->Initialize(&failureReason)) {
      if (failureReason.IsEmpty()){
        failureReason = "SUCCESS";
      }

      compositor->SetCompositorID(mCompositorID);
      return compositor;
    }

    // report any failure reasons here
    if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL){
      gfxCriticalNote << "[OPENGL] Failed to init compositor with reason: "
                      << failureReason.get();
    }
#ifdef XP_WIN
    else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11){
      gfxCriticalNote << "[D3D11] Failed to init compositor with reason: "
                      << failureReason.get();
    }
#endif
  }

  return nullptr;
}

PLayerTransactionParent*
CompositorBridgeParent::AllocPLayerTransactionParent(const nsTArray<LayersBackend>& aBackendHints,
                                                     const uint64_t& aId,
                                                     TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                               bool *aSuccess)
{
  MOZ_ASSERT(aId == 0);

  InitializeLayerManager(aBackendHints);

  if (!mLayerManager) {
    NS_WARNING("Failed to initialise Compositor");
    *aSuccess = false;
    LayerTransactionParent* p = new LayerTransactionParent(nullptr, this, 0);
    p->AddIPDLReference();
    return p;
  }

  mCompositionManager = new AsyncCompositionManager(mLayerManager);
  *aSuccess = true;

  *aTextureFactoryIdentifier = mCompositor->GetTextureFactoryIdentifier();
  LayerTransactionParent* p = new LayerTransactionParent(mLayerManager, this, 0);
  p->AddIPDLReference();
  return p;
}

bool
CompositorBridgeParent::DeallocPLayerTransactionParent(PLayerTransactionParent* actor)
{
  static_cast<LayerTransactionParent*>(actor)->ReleaseIPDLReference();
  return true;
}

CompositorBridgeParent* CompositorBridgeParent::GetCompositorBridgeParent(uint64_t id)
{
  CompositorMap::iterator it = sCompositorMap->find(id);
  return it != sCompositorMap->end() ? it->second : nullptr;
}

void CompositorBridgeParent::AddCompositor(CompositorBridgeParent* compositor, uint64_t* outID)
{
  static uint64_t sNextID = 1;

  ++sNextID;
  (*sCompositorMap)[sNextID] = compositor;
  *outID = sNextID;
}

CompositorBridgeParent* CompositorBridgeParent::RemoveCompositor(uint64_t id)
{
  CompositorMap::iterator it = sCompositorMap->find(id);
  if (it == sCompositorMap->end()) {
    return nullptr;
  }
  CompositorBridgeParent *retval = it->second;
  sCompositorMap->erase(it);
  return retval;
}

void
CompositorBridgeParent::NotifyVsync(const TimeStamp& aTimeStamp, const uint64_t& aLayersId)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_GPU);
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  auto it = sIndirectLayerTrees.find(aLayersId);
  if (it == sIndirectLayerTrees.end())
    return;

  CompositorBridgeParent* cbp = it->second.mParent;
  if (!cbp || !cbp->mWidget)
    return;

  RefPtr<VsyncObserver> obs = cbp->mWidget->GetVsyncObserver();
  if (!obs)
    return;

  obs->NotifyVsync(aTimeStamp);
}

bool
CompositorBridgeParent::RecvNotifyChildCreated(const uint64_t& child)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NotifyChildCreated(child);
  return true;
}

bool
CompositorBridgeParent::RecvNotifyChildRecreated(const uint64_t& aChild)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);

  if (sIndirectLayerTrees.find(aChild) != sIndirectLayerTrees.end()) {
    // Invalid to register the same layer tree twice.
    return false;
  }

  NotifyChildCreated(aChild);
  return true;
}

void
CompositorBridgeParent::NotifyChildCreated(uint64_t aChild)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  sIndirectLayerTrees[aChild].mParent = this;
  sIndirectLayerTrees[aChild].mLayerManager = mLayerManager;
}

bool
CompositorBridgeParent::RecvAdoptChild(const uint64_t& child)
{
  APZCTreeManagerParent* parent;
  {
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    NotifyChildCreated(child);
    if (sIndirectLayerTrees[child].mLayerTree) {
      sIndirectLayerTrees[child].mLayerTree->SetLayerManager(mLayerManager);
    }
    parent = sIndirectLayerTrees[child].mApzcTreeManagerParent;
  }

  if (mApzcTreeManager && parent) {
    parent->ChildAdopted(mApzcTreeManager);
  }
  return true;
}

static void
EraseLayerState(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);

  auto iter = sIndirectLayerTrees.find(aId);
  if (iter != sIndirectLayerTrees.end()) {
    CompositorBridgeParent* parent = iter->second.mParent;
    if (parent) {
      parent->ClearApproximatelyVisibleRegions(aId, Nothing());
    }

    sIndirectLayerTrees.erase(iter);
  }
}

/*static*/ void
CompositorBridgeParent::DeallocateLayerTreeId(uint64_t aId)
{
  MOZ_ASSERT(NS_IsMainThread());
  // Here main thread notifies compositor to remove an element from
  // sIndirectLayerTrees. This removed element might be queried soon.
  // Checking the elements of sIndirectLayerTrees exist or not before using.
  if (!CompositorLoop()) {
    gfxCriticalError() << "Attempting to post to a invalid Compositor Loop";
    return;
  }
  CompositorLoop()->PostTask(NewRunnableFunction(&EraseLayerState, aId));
}

static void
UpdateControllerForLayersId(uint64_t aLayersId,
                            GeckoContentController* aController)
{
  // Adopt ref given to us by SetControllerForLayerTree()
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mController =
    already_AddRefed<GeckoContentController>(aController);
}

ScopedLayerTreeRegistration::ScopedLayerTreeRegistration(APZCTreeManager* aApzctm,
                                                         uint64_t aLayersId,
                                                         Layer* aRoot,
                                                         GeckoContentController* aController)
    : mLayersId(aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mRoot = aRoot;
  sIndirectLayerTrees[aLayersId].mController = aController;
}

ScopedLayerTreeRegistration::~ScopedLayerTreeRegistration()
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees.erase(mLayersId);
}

/*static*/ void
CompositorBridgeParent::SetControllerForLayerTree(uint64_t aLayersId,
                                                  GeckoContentController* aController)
{
  // This ref is adopted by UpdateControllerForLayersId().
  aController->AddRef();
  CompositorLoop()->PostTask(NewRunnableFunction(&UpdateControllerForLayersId,
                                                 aLayersId,
                                                 aController));
}

/*static*/ already_AddRefed<APZCTreeManager>
CompositorBridgeParent::GetAPZCTreeManager(uint64_t aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aLayersId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  LayerTreeState* lts = &cit->second;

  RefPtr<APZCTreeManager> apzctm = lts->mParent
                                   ? lts->mParent->mApzcTreeManager.get()
                                   : nullptr;
  return apzctm.forget();
}

static void
InsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
  /*** STUB ***/
}

/*static */ void
CompositorBridgeParent::PostInsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
  // Called in the vsync thread
  if (profiler_is_active() && CompositorThreadHolder::IsActive()) {
    CompositorLoop()->PostTask(
      NewRunnableFunction(InsertVsyncProfilerMarker, aVsyncTimestamp));
  }
}

widget::PCompositorWidgetParent*
CompositorBridgeParent::AllocPCompositorWidgetParent(const CompositorWidgetInitData& aInitData)
{
#if defined(MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING)
  if (mWidget) {
    // Should not create two widgets on the same compositor.
    return nullptr;
  }

  widget::CompositorWidgetParent* widget =
    new widget::CompositorWidgetParent(aInitData);
  widget->AddRef();

  // Sending the constructor acts as initialization as well.
  mWidget = widget;
  return widget;
#else
  return nullptr;
#endif
}

bool
CompositorBridgeParent::DeallocPCompositorWidgetParent(PCompositorWidgetParent* aActor)
{
#if defined(MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING)
  static_cast<widget::CompositorWidgetParent*>(aActor)->Release();
  return true;
#else
  return false;
#endif
}

bool
CompositorBridgeParent::IsPendingComposite()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCompositor) {
    return false;
  }
  return mCompositor->IsPendingComposite();
}

void
CompositorBridgeParent::FinishPendingComposite()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCompositor) {
    return;
  }
  return mCompositor->FinishPendingComposite();
}

CompositorController*
CompositorBridgeParent::LayerTreeState::GetCompositorController() const
{
  return mParent;
}

MetricsSharingController*
CompositorBridgeParent::LayerTreeState::CrossProcessSharingController() const
{
  return mCrossProcessParent;
}

MetricsSharingController*
CompositorBridgeParent::LayerTreeState::InProcessSharingController() const
{
  return mParent;
}

void
CompositorBridgeParent::DidComposite(TimeStamp& aCompositeStart,
                                     TimeStamp& aCompositeEnd)
{
  Unused << SendDidComposite(0, mPendingTransaction, aCompositeStart, aCompositeEnd);
  mPendingTransaction = 0;

  if (mLayerManager) {
    nsTArray<ImageCompositeNotification> notifications;
    mLayerManager->ExtractImageCompositeNotifications(&notifications);
    if (!notifications.IsEmpty()) {
      Unused << ImageBridgeParent::NotifyImageComposites(notifications);
    }
  }

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([&] (LayerTreeState* lts, const uint64_t& aLayersId) -> void {
    if (lts->mCrossProcessParent && lts->mParent == this) {
      CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent;
      cpcp->DidComposite(aLayersId, aCompositeStart, aCompositeEnd);
    }
  });
}

void
CompositorBridgeParent::InvalidateRemoteLayers()
{
  MOZ_ASSERT(CompositorLoop() == MessageLoop::current());

  Unused << PCompositorBridgeParent::SendInvalidateLayers(0);

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([] (LayerTreeState* lts, const uint64_t& aLayersId) -> void {
    if (lts->mCrossProcessParent) {
      CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent;
      Unused << cpcp->SendInvalidateLayers(aLayersId);
    }
  });
}

bool
CompositorBridgeParent::ResetCompositor(const nsTArray<LayersBackend>& aBackendHints,
                                        TextureFactoryIdentifier* aOutIdentifier)
{
  Maybe<TextureFactoryIdentifier> newIdentifier;
  {
    MonitorAutoLock lock(mResetCompositorMonitor);

    CompositorLoop()->PostTask(NewRunnableMethod
                               <StoreCopyPassByConstLRef<nsTArray<LayersBackend>>,
                                Maybe<TextureFactoryIdentifier>*>(this,
                                                                  &CompositorBridgeParent::ResetCompositorTask,
                                                                  aBackendHints,
                                                                  &newIdentifier));

    mResetCompositorMonitor.Wait();
  }

  if (!newIdentifier) {
    return false;
  }

  *aOutIdentifier = newIdentifier.value();
  return true;
}

// Invoked on the compositor thread. The main thread is waiting on the given
// monitor.
void
CompositorBridgeParent::ResetCompositorTask(const nsTArray<LayersBackend>& aBackendHints,
                                            Maybe<TextureFactoryIdentifier>* aOutNewIdentifier)
{
  // Perform the reset inside a lock, so the main thread can wake up as soon as
  // possible. We notify child processes (if necessary) outside the lock.
  Maybe<TextureFactoryIdentifier> newIdentifier;
  {
    MonitorAutoLock lock(mResetCompositorMonitor);

    newIdentifier = ResetCompositorImpl(aBackendHints);
    *aOutNewIdentifier = newIdentifier;

    mResetCompositorMonitor.NotifyAll();
  }

  // NOTE: |aBackendHints|, and |aOutNewIdentifier| are now all invalid since
  // they are allocated on ResetCompositor's stack on the main thread, which
  // is no longer waiting on the lock.

  if (!newIdentifier) {
    // No compositor change; nothing to do.
    return;
  }

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([&] (LayerTreeState* lts, uint64_t layersId) -> void {
    if (CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent) {
      Unused << cpcp->SendCompositorUpdated(layersId, newIdentifier.value());

      if (LayerTransactionParent* ltp = lts->mLayerTree) {
        ltp->AddPendingCompositorUpdate();
      }
      lts->mPendingCompositorUpdates++;
    }
  });
}

Maybe<TextureFactoryIdentifier>
CompositorBridgeParent::ResetCompositorImpl(const nsTArray<LayersBackend>& aBackendHints)
{
  if (!mLayerManager) {
    return Nothing();
  }

  RefPtr<Compositor> compositor = NewCompositor(aBackendHints);
  if (!compositor) {
    MOZ_RELEASE_ASSERT(compositor, "Failed to reset compositor.");
  }

  // Don't bother changing from basic->basic.
  if (mCompositor &&
      mCompositor->GetBackendType() == LayersBackend::LAYERS_BASIC &&
      compositor->GetBackendType() == LayersBackend::LAYERS_BASIC)
  {
    return Nothing();
  }

  if (mCompositor) {
    mCompositor->SetInvalid();
  }
  mCompositor = compositor;
  mLayerManager->ChangeCompositor(compositor);

  return Some(compositor->GetTextureFactoryIdentifier());
}

static void
OpenCompositor(RefPtr<CrossProcessCompositorBridgeParent> aCompositor,
               Endpoint<PCompositorBridgeParent>&& aEndpoint)
{
  aCompositor->Bind(Move(aEndpoint));
}

/* static */ bool
CompositorBridgeParent::CreateForContent(Endpoint<PCompositorBridgeParent>&& aEndpoint)
{
  gfxPlatform::InitLayersIPC();

  RefPtr<CrossProcessCompositorBridgeParent> cpcp =
    new CrossProcessCompositorBridgeParent();

  CompositorLoop()->PostTask(NewRunnableFunction(OpenCompositor, cpcp, Move(aEndpoint)));
  return true;
}

static void
UpdateIndirectTree(uint64_t aId, Layer* aRoot, const TargetConfig& aTargetConfig)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aId].mRoot = aRoot;
  sIndirectLayerTrees[aId].mTargetConfig = aTargetConfig;
}

/* static */ CompositorBridgeParent::LayerTreeState*
CompositorBridgeParent::GetIndirectShadowTree(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  return &cit->second;
}

static CompositorBridgeParent::LayerTreeState*
GetStateForRoot(uint64_t aContentLayersId, const MonitorAutoLock& aProofOfLock)
{
  CompositorBridgeParent::LayerTreeState* state = nullptr;
  LayerTreeMap::iterator itr = sIndirectLayerTrees.find(aContentLayersId);
  if (sIndirectLayerTrees.end() != itr) {
    state = &itr->second;
  }

  // |state| is the state for the content process, but we want the APZCTMParent
  // for the parent process owning that content process. So we have to jump to
  // the LayerTreeState for the root layer tree id for that layer tree, and use
  // the mApzcTreeManagerParent from that. This should also work with nested
  // content processes, because RootLayerTreeId() will bypass any intermediate
  // processes' ids and go straight to the root.
  if (state) {
    uint64_t rootLayersId = state->mParent->RootLayerTreeId();
    itr = sIndirectLayerTrees.find(rootLayersId);
    state = (sIndirectLayerTrees.end() != itr) ? &itr->second : nullptr;
  }

  return state;
}

/* static */ APZCTreeManagerParent*
CompositorBridgeParent::GetApzcTreeManagerParentForRoot(uint64_t aContentLayersId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState* state =
      GetStateForRoot(aContentLayersId, lock);
  return state ? state->mApzcTreeManagerParent : nullptr;
}

/* static */ GeckoContentController*
CompositorBridgeParent::GetGeckoContentControllerForRoot(uint64_t aContentLayersId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState* state =
      GetStateForRoot(aContentLayersId, lock);
  return state ? state->mController.get() : nullptr;
}

PTextureParent*
CompositorBridgeParent::AllocPTextureParent(const SurfaceDescriptor& aSharedData,
                                            const LayersBackend& aLayersBackend,
                                            const TextureFlags& aFlags,
                                            const uint64_t& aId,
                                            const uint64_t& aSerial)
{
  return TextureHost::CreateIPDLActor(this, aSharedData, aLayersBackend, aFlags, aSerial);
}

bool
CompositorBridgeParent::DeallocPTextureParent(PTextureParent* actor)
{
  return TextureHost::DestroyIPDLActor(actor);
}

bool
CompositorBridgeParent::IsSameProcess() const
{
  return OtherPid() == base::GetCurrentProcId();
}

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
//#define PLUGINS_LOG(...) printf_stderr("CP [%s]: ", __FUNCTION__);
//                         printf_stderr(__VA_ARGS__);
//                         printf_stderr("\n");
#define PLUGINS_LOG(...)

bool
CompositorBridgeParent::UpdatePluginWindowState(uint64_t aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& lts = sIndirectLayerTrees[aId];
  if (!lts.mParent) {
    PLUGINS_LOG("[%" PRIu64 "] layer tree compositor parent pointer is null", aId);
    return false;
  }

  // Check if this layer tree has received any shadow layer updates
  if (!lts.mUpdatedPluginDataAvailable) {
    PLUGINS_LOG("[%" PRIu64 "] no plugin data", aId);
    return false;
  }

  // pluginMetricsChanged tracks whether we need to send plugin update
  // data to the main thread. If we do we'll have to block composition,
  // which we want to avoid if at all possible.
  bool pluginMetricsChanged = false;

  // Same layer tree checks
  if (mLastPluginUpdateLayerTreeId == aId) {
    // no plugin data and nothing has changed, bail.
    if (!mCachedPluginData.Length() && !lts.mPluginData.Length()) {
      PLUGINS_LOG("[%" PRIu64 "] no data, no changes", aId);
      return false;
    }

    if (mCachedPluginData.Length() == lts.mPluginData.Length()) {
      // check for plugin data changes
      for (uint32_t idx = 0; idx < lts.mPluginData.Length(); idx++) {
        if (!(mCachedPluginData[idx] == lts.mPluginData[idx])) {
          pluginMetricsChanged = true;
          break;
        }
      }
    } else {
      // array lengths don't match, need to update
      pluginMetricsChanged = true;
    }
  } else {
    // exchanging layer trees, we need to update
    pluginMetricsChanged = true;
  }

  // Check if plugin windows are currently hidden due to scrolling
  if (mDeferPluginWindows) {
    PLUGINS_LOG("[%" PRIu64 "] suppressing", aId);
    return false;
  }

  // If the plugin windows were hidden but now are not, we need to force
  // update the metrics to make sure they are visible again.
  if (mPluginWindowsHidden) {
    PLUGINS_LOG("[%" PRIu64 "] re-showing", aId);
    mPluginWindowsHidden = false;
    pluginMetricsChanged = true;
  }

  if (!lts.mPluginData.Length()) {
    // Don't hide plugins if the previous remote layer tree didn't contain any.
    if (!mCachedPluginData.Length()) {
      PLUGINS_LOG("[%" PRIu64 "] nothing to hide", aId);
      return false;
    }

    uintptr_t parentWidget = GetWidget()->GetWidgetKey();

    // We will pass through here in cases where the previous shadow layer
    // tree contained visible plugins and the new tree does not. All we need
    // to do here is hide the plugins for the old tree, so don't waste time
    // calculating clipping.
    mPluginsLayerOffset = nsIntPoint(0,0);
    mPluginsLayerVisibleRegion.SetEmpty();
    Unused << lts.mParent->SendHideAllPlugins(parentWidget);
    lts.mUpdatedPluginDataAvailable = false;
    PLUGINS_LOG("[%" PRIu64 "] hide all", aId);
  } else {
    // Retrieve the offset and visible region of the layer that hosts
    // the plugins, CompositorBridgeChild needs these in calculating proper
    // plugin clipping.
    LayerTransactionParent* layerTree = lts.mLayerTree;
    Layer* contentRoot = layerTree->GetRoot();
    if (contentRoot) {
      nsIntPoint offset;
      nsIntRegion visibleRegion;
      if (contentRoot->GetVisibleRegionRelativeToRootLayer(visibleRegion,
                                                            &offset)) {
        // Check to see if these values have changed, if so we need to
        // update plugin window position within the window.
        if (!pluginMetricsChanged &&
            mPluginsLayerVisibleRegion == visibleRegion &&
            mPluginsLayerOffset == offset) {
          PLUGINS_LOG("[%" PRIu64 "] no change", aId);
          return false;
        }
        mPluginsLayerOffset = offset;
        mPluginsLayerVisibleRegion = visibleRegion;
        Unused << lts.mParent->SendUpdatePluginConfigurations(
          LayoutDeviceIntPoint::FromUnknownPoint(offset),
          LayoutDeviceIntRegion::FromUnknownRegion(visibleRegion),
          lts.mPluginData);
        lts.mUpdatedPluginDataAvailable = false;
        PLUGINS_LOG("[%" PRIu64 "] updated", aId);
      } else {
        PLUGINS_LOG("[%" PRIu64 "] no visibility data", aId);
        return false;
      }
    } else {
      PLUGINS_LOG("[%" PRIu64 "] no content root", aId);
      return false;
    }
  }

  mLastPluginUpdateLayerTreeId = aId;
  mCachedPluginData = lts.mPluginData;
  return true;
}

void
CompositorBridgeParent::ScheduleShowAllPluginWindows()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::ShowAllPluginWindows));
}

void
CompositorBridgeParent::ShowAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  mDeferPluginWindows = false;
  ScheduleComposition();
}

void
CompositorBridgeParent::ScheduleHideAllPluginWindows()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod(this, &CompositorBridgeParent::HideAllPluginWindows));
}

void
CompositorBridgeParent::HideAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  // No plugins in the cache implies no plugins to manage
  // in this content.
  if (!mCachedPluginData.Length() || mDeferPluginWindows) {
    return;
  }

  uintptr_t parentWidget = GetWidget()->GetWidgetKey();

  mDeferPluginWindows = true;
  mPluginWindowsHidden = true;

#if defined(XP_WIN)
  // We will get an async reply that this has happened and then send hide.
  mWaitForPluginsUntil = TimeStamp::Now() + mVsyncRate;
  Unused << SendCaptureAllPlugins(parentWidget);
#else
  Unused << SendHideAllPlugins(parentWidget);
  ScheduleComposition();
#endif
}
#endif // #if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)

bool
CompositorBridgeParent::RecvAllPluginsCaptured()
{
#if defined(XP_WIN)
  mWaitForPluginsUntil = TimeStamp();
  mHaveBlockedForPlugins = false;
  ForceComposeToTarget(nullptr);
  Unused << SendHideAllPlugins(GetWidget()->GetWidgetKey());
  return true;
#else
  MOZ_ASSERT_UNREACHABLE(
    "CompositorBridgeParent::RecvAllPluginsCaptured calls unexpected.");
  return false;
#endif
}

} // namespace layers
} // namespace mozilla
