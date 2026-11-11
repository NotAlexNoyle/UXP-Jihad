/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorBridgeParent.h"
#include <stdio.h>                      // for fprintf, stdout
#include <unistd.h>                     // Jihad T-509: sleep() for JIHAD_GPU_TEST_STALL
#include <stdint.h>                     // for uint64_t
#include <map>                          // for _Rb_tree_iterator, etc
#include <mutex>                        // Jihad T-505 (per part): sJihadPartPaintLock
#include "CompositableHost.h"           // Jihad T-505 item 4: the tiled layers under the bands
#include "mozilla/layers/TiledContentHost.h"   // Jihad T-505 item 4: the tiles' buffers (JihadCpuBands)
#if defined(__ARM_NEON__)
# include <arm_neon.h>                          // Jihad T-505, the scaler: the bilinear sample in NEON
#endif
#include "mozilla/layers/ImageDataSerializer.h"   // Jihad T-505 item 4: a tile buffer's stride
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
#include <atomic>                       // Jihad T-505
#include "LayerTreeInvalidation.h"   // Jihad T-502: JihadDiffScrollShift

#include "mozilla/gfx/JihadGpuLog.h"   // Jihad (T-512): JIHAD_GPU_LOG
namespace mozilla { namespace layers { extern double gJihadPhase[12]; } }   // LayerManagerComposite.cpp (round 139)
static bool sJihadInCompose = false;   // Jihad T-502 diagnostic: inside JihadComposeAndRead's composite
static int sJihadOverview = 0;         // Jihad T-504: that composite is an overview window (1: fixed and stuck layers not
                                       // drawn) or the overview's parts (2: only fixed and stuck layers drawn)
static bool sJihadContentOnly = false; // Jihad T-505: the job is a frame without its parts (flag 128): every fixed and every
                                       // sticky layer counts as a part, none of the fixed rows' handling applies
namespace mozilla { namespace layers {
std::atomic<uint32_t> gJihadPartsGen(0);   // Jihad T-505: updates to layers under a fixed or sticky one
// Jihad T-505 (per part): every layer's last paint, as the stamp of ONE counter that only goes up (review F2: a count
// per part that started again after a clear could return to a value a held picture was signed with; a layer painted
// while it was no part keeps its stamp for when it becomes one). CompositableTransactionParent notes each update; an
// ImageBridge update may come on its own thread. Past 4096 layers the map is cleared: the stamps lost make every
// part's signature differ once (a pass), and the next paints take new stamps, so a lost stamp never matches
static std::mutex sJihadPaintStampLock;
static uint32_t sJihadPaintStamp = 0;
static std::map<Layer*, uint32_t> sJihadLayerPaint;
void JihadNotePartPaint(Layer* aLayer)
{
  std::lock_guard<std::mutex> g(sJihadPaintStampLock);
  if (sJihadLayerPaint.size() > 4096) sJihadLayerPaint.clear();
  sJihadLayerPaint[aLayer] = ++sJihadPaintStamp;
}
} }                                        // (CompositableTransactionParent.cpp)
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

// Jihad T-505 diagnostic (JIHAD_GPU_REQLOG=1): where the composite requests (the daemon's "tree changed") come from
void
JihadReqNote(const char* aSrc)
{
  static const bool sOn = getenv("JIHAD_GPU_REQLOG") != nullptr;
  if (sOn) JIHAD_GPU_LOG("[jihad-gpu] composite request: %s\n", aSrc);
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
    JihadReqNote("a shadow tree transaction");
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

// Jihad T-504/T-505: the overview's windows and a frame without its parts (flag 128, sJihadContentOnly) are the
// scrolled content alone: their fixed and stuck layers at shadow opacity 0 (set again from the layer at every
// composite); the overview's parts are the other way round, every leaf layer outside one at 0. A part is a fixed
// layer or a stuck sticky one (the probe); with flag 128 every sticky layer, stuck or not, so no layer turns from
// content into a part between two frames of a pan. After TransformShadowTree (the stuck set is this composite's)
void
CompositorBridgeParent::JihadHideForJob()
{
  Layer* root = mLayerManager ? mLayerManager->GetRoot() : nullptr;
  if (!root || !mCompositionManager) return;
  auto part = [&](Layer* l) {
    return l->GetIsFixedPosition() || (l->GetIsStickyPosition() && (sJihadContentOnly || mCompositionManager->JihadIsStuck(l)));
  };
  const bool partsAlone = sJihadOverview == 2;
  // T-605 diagnostic (JIHAD_GPU_DIFFLOG): each layer a frame without its parts hides, with its bounds on the frame and
  // its children: a part hidden here that the parts list (JihadFixedRectsNow) has no rect for leaves a hole no dress fills
  static const bool sHideLog = getenv("JIHAD_GPU_DIFFLOG") != nullptr;
  static uint32_t sHideN = 0;
  const bool hideLog = sHideLog && !partsAlone && sJihadContentOnly && (sHideN++ % 20) == 0;
  ForEachNode<ForwardIterator>(root, [&](Layer* l) {
    if (!partsAlone) {
      if (part(l)) {
        if (hideLog) {
          const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
          const gfx::IntRect ob = vb.IsEmpty() ? gfx::IntRect() :
            gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
          int kids = 0; for (Layer* c = l->GetFirstChild(); c; c = c->GetNextSibling()) kids++;
          JIHAD_GPU_LOG("[jihad-gpu] hide part %p type %d fixed %d sticky %d stuck %d: local %d,%d %dx%d, on the frame %d,%d %dx%d, %d children\n",
                        l, (int)l->GetType(), (int)l->GetIsFixedPosition(), (int)l->GetIsStickyPosition(),
                        (int)(mCompositionManager->JihadIsStuck(l)), vb.x, vb.y, vb.width, vb.height, ob.x, ob.y, ob.width, ob.height, kids);
        }
        // T-605: a part with more than one child (the contents sidebar holds 9 at the faulty frames of the load, 1
        // after): every child with its bounds on the frame, each such frame up to 60 lines, so the frame the fault
        // shows in has its list
        static uint32_t sKidsLog = 0;
        if (sHideLog && !partsAlone && sJihadContentOnly && sKidsLog < 60) {
          int kids2 = 0; for (Layer* c = l->GetFirstChild(); c; c = c->GetNextSibling()) kids2++;
          if (kids2 > 1) {
            for (Layer* c = l->GetFirstChild(); c && sKidsLog < 60; c = c->GetNextSibling(), sKidsLog++) {
              const gfx::IntRect cb = c->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
              const gfx::IntRect cf = cb.IsEmpty() ? gfx::IntRect() :
                gfx::RoundedOut(c->GetEffectiveTransform().TransformBounds(gfx::Rect(cb.x, cb.y, cb.width, cb.height)));
              int gk = 0; for (Layer* g = c->GetFirstChild(); g; g = g->GetNextSibling()) gk++;
              JIHAD_GPU_LOG("[jihad-gpu]   part %p child %p type %d fixed %d sticky %d opacity %.2f clip %d: on the frame %d,%d %dx%d (%u rects), %d children\n",
                            l, c, (int)c->GetType(), (int)c->GetIsFixedPosition(), (int)c->GetIsStickyPosition(), c->GetEffectiveOpacity(),
                            c->GetClipRect() ? 1 : 0, cf.x, cf.y, cf.width, cf.height, (unsigned)c->GetLocalVisibleRegion().GetNumRects(), gk);
            }
          }
        }
        // T-605: the part's leaves at 0, never the part itself. A container at opacity 0 with more than one child is
        // composed through an intermediate surface (DefaultComputeEffectiveTransforms), and on the frames that had one
        // (the contents sidebar's 9 children during desktop Wikipedia's load) the layers drawn after it, the article,
        // came out only inside the surface's extent (x below 208, the GL scissor's 787 rows from the bottom): a blank
        // article under the dressed sidebar. With one child the opacity folds into it and the frames were right. A
        // leaf at 0 needs no surface, and the hidden part costs no offscreen pass either. JIHAD_GPU_HIDE_PART=1: as before
        static const bool sHideWhole = getenv("JIHAD_GPU_HIDE_PART") != nullptr;
        if (sHideWhole) {
          l->AsLayerComposite()->SetShadowOpacity(0.0f);
        } else {
          ForEachNode<ForwardIterator>(l, [](Layer* d) {
            if (!d->GetFirstChild()) d->AsLayerComposite()->SetShadowOpacity(0.0f);
          });
        }
      }
      return;
    }
    if (l->GetFirstChild()) return;
    for (Layer* c = l; c; c = c->GetParent()) if (part(c)) return;
    l->AsLayerComposite()->SetShadowOpacity(0.0f);
  });
}

// T-505 (E2, per part): FNV-1a per outermost fixed or sticky layer over what makes its pixels: each layer of its subtree
// (identity and type, opacity, content flags, mask, visible region; inside the part its transform and clip too, so an
// inner scroll or a move inside the part counts; a colour layer's colour; an animation: every composite), the paints
// under it (sJihadPartPaint) and, cut by the frame's top or bottom edge, the slice shown. Each rect of aRects takes the
// signatures of the parts whose rects meet it and its own size (aRectSig; a rect no part's rect meets, a nested
// part's own, takes every part's). The whole is the rects' signatures, their count and the global paint count
// (gJihadPartsGen: a daemon with the one signature reads every part when any paint happened under one, as before).
// A content leaf drawn after a part (above it, in paint order) whose pixels meet it is a cover: a popup over a fixed
// bar, a page over a fixed background
// T-606: a leaf drawn above a part covers it only where its pixels are not transparent. Desktop Wikipedia's article
// layer sits above its two sticky sidebars by design (a positioned body after them in one stacking context) and never
// draws on them, but FrameLayerBuilder simplifies a painted layer's visible region to four rects (Accumulate's
// SimplifyOutward(4)), and the bound covers the sidebars: three such "covers" turned the CPU parts off for the document
// (T-605's rounds). For a tiled painted layer under an axis-aligned transform, the tiles under the overlap are read from
// their shared memory, as the CPU band path reads them, and the layer covers the part only where a tile pixel is drawn:
// alpha above 0, or with component alpha a pixel that is not black on black and white on white. A tile's drawn bounds
// are kept per texture and update count (a repaint bumps it), so a tile is read once per paint. Any other layer covers
// as before.
struct JihadTileInk { const uint8_t* buf = nullptr; uint32_t serial = 0; const uint8_t* white = nullptr; uint32_t wserial = 0; gfx::IntRect ink; };
static std::map<const void*, JihadTileInk> sJihadTileInk;   // the compositor thread's

static gfx::IntRect
JihadTileInkOf(BufferTextureHost* aBh, BufferTextureHost* aWhite)
{
  const RGBDescriptor& rgb = aBh->GetBufferDescriptor().get_RGBDescriptor();
  const int w = rgb.size().width, h = rgb.size().height;
  const uint8_t* b = aBh->GetBuffer();
  if (!b || (rgb.format() != gfx::SurfaceFormat::B8G8R8A8 && !aWhite)) return gfx::IntRect(0, 0, w, h);   // opaque
  const int bs = ImageDataSerializer::GetRGBStride(rgb);
  const uint8_t* wt = nullptr;
  int ws = 0;
  if (aWhite) {
    const RGBDescriptor& wr = aWhite->GetBufferDescriptor().get_RGBDescriptor();
    wt = aWhite->GetBuffer(); ws = ImageDataSerializer::GetRGBStride(wr);
    if (!wt || wr.size() != rgb.size()) return gfx::IntRect(0, 0, w, h);
  }
  int x0 = w, y0 = h, x1 = 0, y1 = 0;
  for (int y = 0; y < h; y++) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(b + (size_t)y * bs);
    const uint32_t* wrow = wt ? reinterpret_cast<const uint32_t*>(wt + (size_t)y * ws) : nullptr;
    int fx = -1, lx = -1;
    for (int x = 0; x < w; x++) {
      const bool ink = wrow ? ((row[x] & 0x00ffffffu) != 0 || (wrow[x] & 0x00ffffffu) != 0x00ffffffu) : (row[x] >> 24) != 0;
      if (ink) { if (fx < 0) fx = x; lx = x; }
    }
    if (fx >= 0) { x0 = std::min(x0, fx); x1 = std::max(x1, lx + 1); y0 = std::min(y0, y); y1 = y + 1; }
  }
  return x1 > x0 ? gfx::IntRect(x0, y0, x1 - x0, y1 - y0) : gfx::IntRect();
}

static bool
JihadLeafDrawsIn(Layer* aLayer, const nsIntRegion& aOnFrame)
{
  static const bool sOff = getenv("JIHAD_GPU_EXACT_COVERS") && atoi(getenv("JIHAD_GPU_EXACT_COVERS")) == 0;
  if (sOff || aLayer->GetType() != Layer::TYPE_PAINTED) return true;
  LayerComposite* lc = aLayer->AsLayerComposite();
  CompositableHost* ch = lc ? lc->GetCompositableHost() : nullptr;
  TiledContentHost* th = ch ? ch->AsTiledContentHost() : nullptr;
  gfx::Matrix m;
  if (!th || !aLayer->GetEffectiveTransform().Is2D(&m) || m._12 != 0.0f || m._21 != 0.0f || m._11 <= 0.0f || m._22 <= 0.0f) return true;
  gfx::Matrix inv = m;
  if (!inv.Invert()) return true;
  TiledLayerBufferComposite& tb = th->JihadTiles();
  nsIntRegion inLayer;   // the overlap in the layer's pixels
  for (auto it = aOnFrame.RectIter(); !it.Done(); it.Next()) {
    const gfx::IntRect& q = it.Get();
    inLayer.OrWith(gfx::RoundedOut(inv.TransformBounds(gfx::Rect(q.x, q.y, q.width, q.height))));
  }
  inLayer.AndWith(tb.GetValidRegion());
  if (inLayer.IsEmpty()) return false;   // no painted tile under the part
  const float res = tb.GetResolution() > 0.0f ? tb.GetResolution() : 1.0f;
  if (sJihadTileInk.size() > 1024) sJihadTileInk.clear();
  for (size_t i = 0; i < tb.GetTileCount(); i++) {
    TileHost& tile = tb.GetTile(i);
    if (tile.IsPlaceholderTile()) continue;
    const gfx::IntPoint off = tb.GetTileOffset(tb.GetPlacement().TilePosition(i));
    nsIntRegion tr(gfx::IntRect(off, tb.GetScaledTileSize()));
    tr.AndWith(inLayer);
    if (tr.IsEmpty()) continue;
    BufferTextureHost* bh = tile.mTextureHost ? tile.mTextureHost->AsBufferTextureHost() : nullptr;
    BufferTextureHost* wh = tile.mTextureHostOnWhite ? tile.mTextureHostOnWhite->AsBufferTextureHost() : nullptr;
    if (!bh || bh->GetBufferDescriptor().type() != BufferDescriptor::TRGBDescriptor) return true;
    if (tile.mTextureHostOnWhite && (!wh || wh->GetBufferDescriptor().type() != BufferDescriptor::TRGBDescriptor)) return true;
    JihadTileInk& e = sJihadTileInk[bh];
    const uint8_t* wbuf = wh ? wh->GetBuffer() : nullptr;
    const uint32_t wser = wh ? wh->JihadUpdateSerial() : 0;
    if (e.buf != bh->GetBuffer() || e.serial != bh->JihadUpdateSerial() || e.white != wbuf || e.wserial != wser) {
      e.buf = bh->GetBuffer(); e.serial = bh->JihadUpdateSerial(); e.white = wbuf; e.wserial = wser;
      e.ink = JihadTileInkOf(bh, wh);
    }
    if (e.ink.IsEmpty()) continue;
    const gfx::IntRect inkLayer = gfx::RoundedOut(gfx::Rect(off.x + e.ink.x / res, off.y + e.ink.y / res, e.ink.width / res, e.ink.height / res));
    tr.AndWith(inkLayer);
    if (!tr.IsEmpty()) return true;
  }
  return false;
}

uint32_t
CompositorBridgeParent::JihadPartsSig(const gfx::IntRect* aRects, int aN, bool* aCovered, int aWidth, int aHeight, uint32_t* aRectSig)
{
  const uint32_t kBasis = 2166136261u;
  uint32_t h = kBasis;   // the part being walked
  auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
  auto mixf = [&](float f) { uint32_t u; memcpy(&u, &f, sizeof u); mix(u); };
  auto mixr = [&](const gfx::IntRect& r) { mix(r.x); mix(r.y); mix(r.width); mix(r.height); };
  struct TopPart { uint32_t sig; gfx::IntRect ir; gfx::IntRect tb; };   // tb: its bounds on the frame, not inflated
  TopPart tops[32];
  int nTop = 0;
  gfx::IntRect topIr, topTb;
  std::map<Layer*, uint32_t> stamps;   // the layers' paint stamps, taken once under the lock
  {
    std::lock_guard<std::mutex> g(sJihadPaintStampLock);
    stamps = sJihadLayerPaint;
  }
  auto stamp = [&](Layer* l) { auto it = stamps.find(l); return it != stamps.end() ? it->second : 0u; };
  const gfx::IntRect frame(0, 0, aWidth, aHeight);
  *aCovered = false;
  Layer* root = mLayerManager ? mLayerManager->GetRoot() : nullptr;
  if (root) {
    static uint32_t sAnimTick = 0;
    static uint32_t sCoverLog = 0;
    int depth = 0;
    nsIntRegion partsSoFar;
    auto onFrame = [&](Layer* x, nsIntRegion& aOut) {
      gfx::Matrix m;
      if (!x->GetEffectiveTransform().Is2D(&m) || !m.PreservesAxisAlignedRectangles()) {
        const gfx::IntRect vb = x->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
        aOut.OrWith(gfx::RoundedOut(x->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height))));
        return;
      }
      const nsIntRegion visible = x->GetLocalVisibleRegion().ToUnknownRegion();
      for (auto it = visible.RectIter(); !it.Done(); it.Next()) {
        const gfx::IntRect& q = it.Get();
        aOut.OrWith(gfx::RoundedOut(x->GetEffectiveTransform().TransformBounds(gfx::Rect(q.x, q.y, q.width, q.height))));
      }
    };
    ForEachNode<ForwardIterator>(root,
      [&](Layer* l) {
        const bool part = l->GetIsFixedPosition() || l->GetIsStickyPosition();
        if (part || depth > 0) {
          if (part && depth == 0) {
            // a new part: its own hash; its rect on the frame as JihadFixedRectsNow makes it, and its bounds; (review F3)
            // its scale and the sub-pixel phase of its position (a quarter pixel): a picture made at another zoom
            // or phase is other pixels
            h = kBasis;
            topIr = topTb = gfx::IntRect();
            const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
            if (!vb.IsEmpty()) {
              topTb = gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
              topIr = topTb;
              topIr.Inflate(1);
              topIr = topIr.Intersect(frame);
            }
            gfx::Matrix m2;
            if (l->GetEffectiveTransform().Is2D(&m2)) {
              mixf(m2._11); mixf(m2._22); mixf(m2._12); mixf(m2._21);
              mix((uint32_t)lroundf((m2._31 - floorf(m2._31)) * 4.0f)); mix((uint32_t)lroundf((m2._32 - floorf(m2._32)) * 4.0f));
            } else {
              mix(0x3d3d3d3du);
            }
          }
          mix((uint32_t)(uintptr_t)l); mix((uint32_t)l->GetType()); mixf(l->GetOpacity());
          mix(stamp(l));   // its last paint (every layer of the subtree: review F2)
          mix(l->GetContentFlags());
          if (Layer* mk = l->GetMaskLayer()) { mix((uint32_t)(uintptr_t)mk); mixr(mk->GetVisibleRegion().GetBounds().ToUnknownRect()); }   // (review F5)
          mixr(l->GetVisibleRegion().GetBounds().ToUnknownRect());
          if (l->GetClipRect()) mixr(l->GetClipRect()->ToUnknownRect());   // (review F5) a top-level part's clip too
          if (depth > 0) {
            const gfx::Matrix4x4 m = l->GetBaseTransform();
            for (int k = 0; k < 16; k++) mixf(m.components[k]);
          }
          if (l->GetMixBlendMode() != gfx::CompositionOp::OP_OVER) *aCovered = true;   // (review F5) not drawable by "over" on the CPU
          if (part && depth == 0) {
            // (review F5) a part cut by the frame's top or bottom edge: its rect keeps its size while the slice shown
            // moves with it (a sticky element not yet stuck, taller than the frame or half off it): where it starts
            const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
            const gfx::IntRect tb = gfx::RoundedOut(l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
            if (tb.y < 0 || tb.YMost() > aHeight) mix((uint32_t)tb.y);
          }
          if (l->GetType() == Layer::TYPE_COLOR) {
            const gfx::Color c = static_cast<ColorLayer*>(l)->GetColor();
            mixf(c.r); mixf(c.g); mixf(c.b); mixf(c.a);
          }
          if (!l->GetAnimations().IsEmpty()) mix(++sAnimTick);
          if (part && depth == 0) onFrame(l, partsSoFar);
        } else if (!l->GetFirstChild() && !partsSoFar.IsEmpty() && l->GetEffectiveOpacity() > 0.0f) {
          const Layer::LayerType t = l->GetType();
          if (t == Layer::TYPE_PAINTED || t == Layer::TYPE_COLOR || t == Layer::TYPE_IMAGE || t == Layer::TYPE_CANVAS) {
            nsIntRegion mine;
            onFrame(l, mine);
            mine.AndWith(partsSoFar);
            if (!mine.IsEmpty() && JihadLeafDrawsIn(l, mine)) {   // T-606: where it draws, not where its region's bound lies
              if (!*aCovered && sCoverLog++ < 20) {
                const gfx::IntRect b = mine.GetBounds();
                JIHAD_GPU_LOG("[jihad-gpu] parts: layer %p (type %d) drawn above a part at %d,%d %dx%d\n", l, (int)t, b.x, b.y, b.width, b.height);
              }
              *aCovered = true;
            } else if (!mine.IsEmpty()) {
              static uint32_t sClearLog = 0;
              if (sClearLog++ < 10) {
                const gfx::IntRect b = mine.GetBounds();
                JIHAD_GPU_LOG("[jihad-gpu] parts: layer %p (type %d) above a part at %d,%d %dx%d draws nothing there: not a cover (T-606)\n",
                              l, (int)t, b.x, b.y, b.width, b.height);
              }
            }
          }
        }
        if (part) depth++;
      },
      [&](Layer* l) {
        if (!(l->GetIsFixedPosition() || l->GetIsStickyPosition())) return;
        if (--depth > 0) return;
        // the part walked: its signature, rect and bounds kept (past 32 parts, folded into the last)
        if (nTop < 32) { tops[nTop].sig = h; tops[nTop].ir = topIr; tops[nTop].tb = topTb; nTop++; }
        else { tops[31].sig = (tops[31].sig ^ h) * 16777619u; tops[31].ir = tops[31].ir.Union(topIr); tops[31].tb = tops[31].tb.Union(topTb); }
      });
  }
  // each rect's signature, and the whole
  uint32_t whole = kBasis;
  auto mixw = [&](uint32_t v) { whole = (whole ^ v) * 16777619u; };
  mixw((uint32_t)gJihadPartsGen); mixw((uint32_t)aN);
  for (int k = 0; k < aN; k++) {
    h = kBasis;
    mix((uint32_t)aRects[k].width); mix((uint32_t)aRects[k].height);
    bool any = false;
    for (int i = 0; i < nTop; i++) {
      if (tops[i].ir.IsEmpty() || !aRects[k].Intersects(tops[i].ir)) continue;
      mix(tops[i].sig); any = true;
      // (review F3) where the part sits inside the rect, and its size: a part wider than the frame moving sideways
      // under a cut rect, a part moving inside a merged rect, keep the rect's size and would keep its picture
      mix((uint32_t)(tops[i].tb.x - aRects[k].x)); mix((uint32_t)(tops[i].tb.y - aRects[k].y));
      mix((uint32_t)tops[i].tb.width); mix((uint32_t)tops[i].tb.height);
    }
    if (!any) {   // (review F6) a rect no part's rect meets: every part's, and its own index and place
      for (int i = 0; i < nTop; i++) mix(tops[i].sig);
      mix((uint32_t)k); mix((uint32_t)aRects[k].x); mix((uint32_t)aRects[k].y);
    }
    if (aRectSig) aRectSig[k] = h;
    mixw(h);
  }
  return whole;
}

void
CompositorBridgeParent::CompositeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  {
    // Jihad T-502 diagnostic: composites not started by the daemon's request
    static uint32_t sStray = 0, sOurs = 0;
    if (sJihadInCompose) ++sOurs; else { ++sStray; JIHAD_GPU_LOG("[jihad-gpu] stray composite %u (ours so far %u), target %p\n", sStray, sOurs, aTarget); }
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
  mJihadAnimating = requestNextFrame;
  if (sJihadPhases) gJihadPhase[7] += (TimeStamp::Now() - jihadT0).ToMilliseconds();
  // Jihad T-504: an overview window is the scrolled content alone: the daemon stitches windows composed at
  // other scrolls into one picture, where fixed and stuck layers would sit once per window. Their shadow
  // opacity (set again from the layer at every composite) is 0 for this one. The overview's parts are the
  // other way round: every leaf layer outside a fixed or stuck one at 0, so they come back with their alpha
  if ((sJihadOverview || sJihadContentOnly) && sJihadInCompose) JihadHideForJob();
  if (requestNextFrame) {
    JihadReqNote(sJihadContentOnly ? "the next frame asked (an animation), a job without its parts"
                                   : (sJihadOverview ? "the next frame asked (an animation), an overview job" : "the next frame asked (an animation)"));
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
      JIHAD_GPU_LOG("[jihad-gpu] slow composite %.0f ms: shadow props %.1f, async transforms %.1f, post-process %.1f, transforms+diff %.1f, begin frame %.1f, prepare %.1f, render layers %.1f (draws %.1f: %.0f calls, %.0f culled; uploads %.1f), end frame %.1f\n",
              all, d[6], d[7], d[0], d[1], d[2], d[3], d[4], d[8], d[9], d[10], d[11], d[5]);
    }
    static uint32_t sN = 0;
    if ((++sN % 20) == 0) {
      size_t layers = 0, painted = 0;
      if (Layer* root = mLayerManager->GetRoot()) {
        ForEachNode<ForwardIterator>(root, [&](Layer* l) { ++layers; if (l->GetType() == Layer::TYPE_PAINTED) ++painted; });
      }
      JIHAD_GPU_LOG("[jihad-gpu] phases over 20 composites (ms each): shadow props %.2f, async transforms %.2f, post-process %.2f, transforms+diff %.2f, begin frame %.2f, prepare %.2f, render layers %.2f (draws %.2f: %.1f calls, %.1f culled; uploads %.2f), end frame %.2f; %zu layers (%zu painted)\n",
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
      JIHAD_GPU_LOG("[jihad-gpu] compose-to-target: %.1f ms on the compositor thread (incl. copy-to-target)\n",
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
    JihadReqNote(sJihadContentOnly ? "composite until, a job without its parts" : "composite until");
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
  return sOn && !sJihadContentOnly;   // T-505: a frame without its parts has no fixed pixels to keep or read
}

// T-505 item 4 (2026-11-04): the band frames composed on the CPU from the tiles — the GPU path's default (the
// Deciding Vote of 2026-11-04, Codex, on the rounds' numbers; JIHAD_GPU_CPU_BANDS=0 turns it off): a band frame
// (a scroll-only frame, or one whose change fits the band pass) whose band rows are covered by tiled painted layers
// and colour layers only — each drawn "over" at full opacity, no mask, under a translation of whole frame pixels at
// a scale of 1 (the layers painted at the card's zoom's resolution: the daemon's JIHAD_GPU_RES_ZOOM) — is composed
// by the compositor thread copying the tiles' pixels (BufferTextureHost::GetBuffer: the shared memory the content
// side painted, read-locked while its tile is held) into the slot's band rows: no GL composite, no glReadPixels
// (90 us a row on the Adreno 220, T-567), no texture upload for those tiles. The frame's bookkeeping is a
// composite's (the shadow properties and the async transforms at this frame's scroll, DidComposite to the content
// side). Anything else under a band (a scaled or fractional transform, a mask, a blend mode, a group composed apart,
// a canvas, an image, a component-alpha tile, a low-precision buffer) sends the frame to the GPU band pass as
// before; the reasons are counted and logged every 20 band frames (the census of engine 0074, now the switch's own
// count). The measurement of 2026-11-03 (the census, by finger): the long page 420 of 420 band frames could be
// composed this way at scale 1; desktop Wikipedia 291 of 360, none at scale 1 with the tiles painted at 1 under a
// fit zoom of 0.69 (hence the resolution switch), the rest a masked translucent layer in the first drags.
static bool
JihadCpuBandsWanted()
{
  static const bool sOn = getenv("JIHAD_GPU_COMPOSITOR") != nullptr &&
                          !(getenv("JIHAD_GPU_CPU_BANDS") && atoi(getenv("JIHAD_GPU_CPU_BANDS")) == 0);
  return sOn;
}

// T-505, the scaler (2026-11-09; the owner's scale choice (a) after the Deciding Vote of 2026-11-04 had set it
// aside): a tiled layer painted at resolution 1 under a card zoom below 1 carries a uniform downscale in its
// effective transform (desktop Wikipedia's fit zoom of 0.69: none of its band frames was at scale 1 in 2026-11-03's
// census, so every one went to the GPU band pass and its glReadPixels). With JIHAD_GPU_CPU_SCALE=1 such a leaf is
// composed on the CPU too: each band pixel is the bilinear sample of the four layer pixels around its source point,
// the GPU pass's linear filter at the same scale, so the paints stay at resolution 1 (the resolution switch's
// dearer paints, 2026-11-04, are what this avoids). Off until measured on interleaved Wikipedia rounds (T-505).
static bool
JihadCpuScaleWanted()
{
  static const bool sOn = getenv("JIHAD_GPU_CPU_SCALE") && atoi(getenv("JIHAD_GPU_CPU_SCALE")) != 0;
  return sOn;
}

struct JihadCpuLeaf {
  Layer* layer;
  gfx::IntRect clip;   // the layer's scissor on the frame (ContainerPrepare's CalculateScissorRect chain)
  int tx, ty;          // its effective transform: a translation of whole frame pixels
  bool scaled;         // the scaler: a uniform downscale instead, the translation as it is
  float scale, ftx, fty;
};

struct JihadCpuCount {   // the band frames' census (a line every 20 band frames), and this frame's own counts
  uint32_t frames = 0, cpu = 0, could = 0, scaled = 0, fractional = 0, other = 0, masked = 0, blend = 0, group = 0,
           is3d = 0, transl = 0, lowres = 0, tile = 0, none = 0, scaledok = 0;   // scaledok: leaves drawn by the scaler
  int leaves = 0, tiled = 0, colour = 0, tiles = 0, rects = 0;
};
static JihadCpuCount sJihadCpu;

static void
JihadCpuLog()
{
  if ((sJihadCpu.frames % 20) != 0) return;
  JIHAD_GPU_LOG("[jihad-gpu] cpu bands: %u band frames, %u composed on the CPU (%u could be); not: scaled %u, fractional %u, not tiled or colour %u, masked %u, blend %u, group %u, 3d %u, translucent %u, low-res or resolution %u, tile format %u; no layer %u; %u leaves by the scaler\n",
                sJihadCpu.frames, sJihadCpu.cpu, sJihadCpu.could, sJihadCpu.scaled, sJihadCpu.fractional, sJihadCpu.other,
                sJihadCpu.masked, sJihadCpu.blend, sJihadCpu.group, sJihadCpu.is3d, sJihadCpu.transl, sJihadCpu.lowres,
                sJihadCpu.tile, sJihadCpu.none, sJihadCpu.scaledok);
}

// a rect on the frame meets one of the band rows
static bool
JihadCpuMeets(const gfx::IntRect& aOb, const int aBands[2][2], int aW, int aH)
{
  for (int b = 0; b < 2; b++) {
    const int lo = std::max(0, aBands[b][0]), hi = std::min(aH, aBands[b][1]);
    if (hi > lo && aOb.y < hi && aOb.YMost() > lo && aOb.x < aW && aOb.XMost() > 0) return true;
  }
  return false;
}

// the leaves under the band rows in paint order (the siblings' order: no 3D context), each with its scissor, when
// every one of them can be composed on the CPU; false, the reason counted, when one cannot
static bool
JihadCpuCollect(Layer* aLayer, const RenderTargetIntRect& aParentClip, bool aRoot, const int aBands[2][2],
                int aW, int aH, nsTArray<JihadCpuLeaf>& aLeaves, JihadCpuCount& aN)
{
  if (!aRoot && aLayer->IsBackfaceHidden()) return true;
  if (aLayer->GetEffectiveOpacity() <= 0.0f) return true;   // drawn invisible (a part hidden for a frame without its parts)
  // the root's scissor is the frame within its own clip (LayerManagerComposite::Render's), a child's its
  // CalculateScissorRect from its container's
  RenderTargetIntRect clip = aRoot ? aParentClip : aLayer->CalculateScissorRect(aParentClip);
  if (aRoot && aLayer->GetLocalClipRect()) {
    const ParentLayerIntRect& rc = *aLayer->GetLocalClipRect();
    clip = clip.Intersect(RenderTargetIntRect(rc.x, rc.y, rc.width, rc.height));
  }
  const gfx::IntRect clipRect(clip.x, clip.y, clip.width, clip.height);
  if (ContainerLayer* c = aLayer->AsContainerLayer()) {
    // a container composed apart — an intermediate surface (a group under an opacity or a mask), a blend mode, a
    // 3D context, an isolated group — is not a stack of leaves drawn "over", and its leaves' effective transforms
    // and scissors are the surface's, not the frame's (review 2026-11-04, F2): when its own bounds meet a band the
    // frame goes to the GPU; else its subtree is not under the bands
    if (c->UseIntermediateSurface() || c->GetMaskLayer() || c->GetMixBlendMode() != gfx::CompositionOp::OP_OVER ||
        c->Extend3DContext() || c->GetForceIsolatedGroup()) {
      const gfx::IntRect vb = c->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
      gfx::IntRect ob = gfx::RoundedOut(c->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
      ob = ob.Intersect(clipRect);
      if (JihadCpuMeets(ob, aBands, aW, aH)) { aN.group++; return false; }
      return true;
    }
    for (Layer* ch = c->GetFirstChild(); ch; ch = ch->GetNextSibling()) {
      if (!JihadCpuCollect(ch, clip, false, aBands, aW, aH, aLeaves, aN)) return false;
    }
    return true;
  }
  const Layer::LayerType t = aLayer->GetType();
  // an image or a canvas layer anywhere in the tree: a composite's traversal keeps their timed frames and
  // notifications going even off the bands (review F10); such a tree stays on the GPU
  if (t == Layer::TYPE_IMAGE || t == Layer::TYPE_CANVAS) { aN.other++; return false; }
  if (!aLayer->IsVisible() || clip.IsEmpty()) return true;
  const gfx::IntRect vb = aLayer->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
  gfx::Matrix m;
  const bool is2d = aLayer->GetEffectiveTransform().Is2D(&m);
  gfx::IntRect ob = gfx::RoundedOut(aLayer->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height)));
  ob = ob.Intersect(clipRect);
  if (!JihadCpuMeets(ob, aBands, aW, aH)) return true;
  if (!is2d) { aN.is3d++; return false; }
  // a scale of 1 within float noise (a scale of 1.0009 over 1024 px is a pixel at the edge, review F7) and a
  // translation of whole pixels (SnapTransformTranslation's); or, with the scaler, a uniform downscale between
  // 0.25 and 1 (the card's zoom below 1 over tiles at resolution 1), its translation taken as it is
  if (fabsf(m._12) > 2e-5f || fabsf(m._21) > 2e-5f) { aN.scaled++; return false; }
  const bool unit = fabsf(m._11 - 1.0f) <= 2e-5f && fabsf(m._22 - 1.0f) <= 2e-5f;
  bool scaled = false; float scale = 1.0f;
  if (!unit) {
    if (!JihadCpuScaleWanted() || fabsf(m._11 - m._22) > 1e-4f || m._11 < 0.25f || m._11 > 1.0f) { aN.scaled++; return false; }
    scaled = true; scale = m._11;
  }
  const int itx = (int)lroundf(m._31), ity = (int)lroundf(m._32);
  if (!scaled && (fabsf(m._31 - (float)itx) > 0.01f || fabsf(m._32 - (float)ity) > 0.01f)) { aN.fractional++; return false; }
  if (aLayer->GetMaskLayer() || aLayer->GetAncestorMaskLayerCount() > 0) { aN.masked++; return false; }
  if (aLayer->GetMixBlendMode() != gfx::CompositionOp::OP_OVER) { aN.blend++; return false; }
  if (aLayer->GetEffectiveOpacity() < 0.999f) { aN.transl++; return false; }
  if (t == Layer::TYPE_COLOR) {
    aN.colour++;
  } else if (t == Layer::TYPE_PAINTED) {
    CompositableHost* h = aLayer->AsLayerComposite() ? aLayer->AsLayerComposite()->GetCompositableHost() : nullptr;
    TiledContentHost* th = h ? h->AsTiledContentHost() : nullptr;
    if (!th) { aN.other++; return false; }
    TiledLayerBufferComposite& tb = th->JihadTiles();
    // the tiles at the layer's own pixels (the high-precision buffer's resolution exactly 1), no low-precision
    // buffer drawn under them, no fade-in
    if (th->JihadLowResTiles().GetTileCount() > 0 || gfxPrefs::LayerTileFadeInEnabled() || tb.GetResolution() != 1.0f) {
      aN.lowres++; return false;
    }
    // every tile under the band rows a shared-memory buffer of B8G8R8A8 or B8G8R8X8, no component alpha, its read
    // lock still held (the content side cannot be painting it: TextureHost.cpp's MaybeUpload keeps the lock under
    // this switch; a tile without one is not read)
    const gfx::IntSize ts = tb.GetTileSize();
    for (size_t i = 0; i < tb.GetTileCount(); i++) {
      TileHost& tile = tb.GetTile(i);
      if (tile.IsPlaceholderTile()) continue;
      const gfx::IntPoint off = tb.GetTileOffset(tb.GetPlacement().TilePosition(i));
      const int ty0 = scaled ? (int)floorf(off.y * scale + m._32) : off.y + ity;
      const int ty1 = scaled ? (int)ceilf((off.y + ts.height) * scale + m._32) : ty0 + ts.height;
      bool under = false;
      for (int b = 0; b < 2; b++) {
        const int lo = std::max(0, aBands[b][0]), hi = std::min(aH, aBands[b][1]);
        if (hi > lo && ty0 < hi && ty1 > lo) under = true;
      }
      if (!under) continue;
      BufferTextureHost* bh = tile.mTextureHost->AsBufferTextureHost();
      if (!bh || tile.mTextureHostOnWhite || bh->GetBufferDescriptor().type() != BufferDescriptor::TRGBDescriptor || !bh->GetBuffer() ||
          !tile.mTextureHost->GetReadLock()) {
        aN.tile++; return false;
      }
      const RGBDescriptor& rgb = bh->GetBufferDescriptor().get_RGBDescriptor();
      if ((rgb.format() != gfx::SurfaceFormat::B8G8R8A8 && rgb.format() != gfx::SurfaceFormat::B8G8R8X8) ||
          rgb.size().width < ts.width || rgb.size().height < ts.height) {
        aN.tile++; return false;
      }
    }
    aN.tiled++;
  } else {
    aN.other++; return false;
  }
  JihadCpuLeaf leaf;
  leaf.layer = aLayer;
  leaf.clip = gfx::IntRect(clip.x, clip.y, clip.width, clip.height);
  leaf.tx = itx; leaf.ty = ity;
  leaf.scaled = scaled; leaf.scale = scale; leaf.ftx = m._31; leaf.fty = m._32;
  if (scaled) aN.scaledok++;
  aLeaves.AppendElement(leaf);
  aN.leaves++;
  return true;
}

static gfx::IntRect JihadCoveredPixels(const gfx::Rect& aR);   // below
static inline void JihadCpuOver(uint8_t* d, const uint8_t* s, int n);   // below

// T-505, the scaler: a tiled leaf under a uniform downscale drawn into a band. For each rect of the layer's drawn
// region (valid tiles within the visible region, in layer pixels) the band pixels it covers at the scale are
// visited; a pixel's source point is ((x + 0.5 - tx) / s - 0.5, (y + 0.5 - ty) / s - 0.5) in layer pixels, its
// value the bilinear sample of the four layer pixels around that point (the GPU pass's linear filter at the same
// scale), the neighbours clamped to the rect (no sample from outside what is drawn). Weights in 1/256ths;
// premultiplied, "over" when a tile's format carries alpha.
// How the pixels are reached (the builds of 2026-11-09): a tile sits at GetTileOffset, the grid's ORIGIN plus its
// position times the tile size — a layer small enough for one tile has that tile where the layer's region is, not
// at a multiple of the tile size (the title block of Wikipedia: one 601x71 tile at 263,92; the first builds looked
// the tiles up from a grid at 0,0 and drew nothing for it, 2026-11-09's verify). Each source row a sample needs is
// copied once into a cached buffer (one memcpy per tile column) and the samples read from that: the per-pixel
// reads straight from the tiles' shared buffers cost 260-520 us a band row on the device (the second and third
// builds), a sequential copy does not. A pixel one of whose four sources is not a tile's bytes (a placeholder, no
// buffer) is left as it was.
struct JihadCpuTileRef { const uint8_t* base; int stride; bool alpha; bool ok; };
struct JihadCpuSrcRow { int sy; nsTArray<uint8_t> px; nsTArray<uint8_t> ok; bool alpha; };
static void
JihadCpuDrawScaled(uint8_t* aDest, int aStride, const JihadCpuLeaf& aLf, TiledLayerBufferComposite& aTb,
                   const nsIntRegion& aRegion, const gfx::IntRect& aBand, JihadCpuCount& aN)
{
  const gfx::IntSize ts = aTb.GetTileSize();
  const TilesPlacement& pl = aTb.GetPlacement();
  const gfx::IntPoint org = aTb.GetTileOffset(TileIntPoint(0, 0));   // the grid's origin in layer pixels
  const float s = aLf.scale, inv = 1.0f / s;
  const int tw = ts.width, th = ts.height;
  auto tileDiv = [](int v, int len) -> int { return (v >= 0) ? v / len : -((-v - 1) / len) - 1; };
  auto tileAt = [&](int tpx, int tpy) -> JihadCpuTileRef {
    JihadCpuTileRef r; r.base = nullptr; r.stride = 0; r.alpha = false; r.ok = false;
    const TileIntPoint pos(tpx, tpy);
    if (!pl.HasTile(pos)) return r;
    TileHost& tile = aTb.GetTile(pl.TileIndex(pos));
    if (tile.IsPlaceholderTile() || !tile.mTextureHost) return r;
    BufferTextureHost* bh = tile.mTextureHost->AsBufferTextureHost();
    if (!bh || bh->GetBufferDescriptor().type() != BufferDescriptor::TRGBDescriptor || !bh->GetBuffer()) return r;
    const RGBDescriptor& rgb = bh->GetBufferDescriptor().get_RGBDescriptor();
    r.base = bh->GetBuffer(); r.stride = ImageDataSerializer::GetRGBStride(rgb);
    r.alpha = rgb.format() == gfx::SurfaceFormat::B8G8R8A8; r.ok = true;
    return r;
  };
  static int sDiag = 0;   // the first leaves drawn, described (the scaler's rounds)
  const bool diag = sDiag < 6;
  if (diag) {
    sDiag++;
    const gfx::IntRect rb = aRegion.GetBounds();
    JIHAD_GPU_LOG("[jihad-gpu] scaler leaf: scale %.4f t %.2f,%.2f tiles %dx%d at %d,%d first %d,%d size %dx%d (%u tiles), region %d rects bounds %d,%d %dx%d, clip %d,%d %dx%d, band rows %d..%d\n",
                  s, aLf.ftx, aLf.fty, tw, th, org.x, org.y, pl.mFirst.x, pl.mFirst.y, pl.mSize.width, pl.mSize.height, aTb.GetTileCount(),
                  aRegion.GetNumRects(), rb.x, rb.y, rb.width, rb.height, aLf.clip.x, aLf.clip.y, aLf.clip.width, aLf.clip.height, aBand.y, aBand.YMost());
  }
  long diagPx = 0, diagSkip = 0, diagRects = 0;
  const TimeStamp diagT0 = TimeStamp::Now();
  nsTArray<int> ix0s, ix1s, tc0s, tc1s, wxs;   // per band column: the two source columns (byte offsets in a copied row), their tile columns, the weight
  JihadCpuSrcRow cache[2];
  nsTArray<uint8_t> rowBuf, vrow;   // the band row composed here; the two source rows blended down
  for (auto it = aRegion.RectIter(); !it.Done(); it.Next()) {
    const gfx::IntRect vr = it.Get();
    if (vr.IsEmpty()) continue;
    gfx::IntRect dr = JihadCoveredPixels(gfx::Rect(vr.x * s + aLf.ftx, vr.y * s + aLf.fty, vr.width * s, vr.height * s));
    dr = dr.Intersect(aLf.clip).Intersect(aBand);
    if (dr.IsEmpty()) continue;
    diagRects++;
    // The samples' reach: one layer pixel past the rect on each side where a tile holds it (the GPU's linear filter
    // reads the texel beyond a rect's edge inside a tile and duplicates only at a tile's edge; clamped at the rect,
    // the fourth build differed from the GPU by one row at every rect seam — 1 wrong row a frame, 2026-11-09's verify)
    auto hasTile = [&](int lx, int ly) -> bool {
      const TileIntPoint pos(tileDiv(lx - org.x, tw), tileDiv(ly - org.y, th));
      return pl.HasTile(pos) && !aTb.GetTile(pl.TileIndex(pos)).IsPlaceholderTile();
    };
    const int xLo = hasTile(vr.x - 1, vr.y) ? vr.x - 1 : vr.x, xHi = hasTile(vr.XMost(), vr.y) ? vr.XMost() + 1 : vr.XMost();
    const int yLo = hasTile(vr.x, vr.y - 1) ? vr.y - 1 : vr.y, yHi = hasTile(vr.x, vr.YMost()) ? vr.YMost() + 1 : vr.YMost();
    const int tcMin = tileDiv(xLo - org.x, tw), tcMax = tileDiv(xHi - 1 - org.x, tw), nTc = tcMax - tcMin + 1;
    ix0s.SetLength(dr.width); ix1s.SetLength(dr.width); tc0s.SetLength(dr.width); tc1s.SetLength(dr.width); wxs.SetLength(dr.width);
    for (int i = 0; i < dr.width; i++) {
      const float u = ((float)(dr.x + i) + 0.5f - aLf.ftx) * inv - 0.5f;
      int ix = (int)floorf(u);
      int wx = (int)lroundf((u - (float)ix) * 256.0f);
      if (wx >= 256) { ix++; wx = 0; }
      int ix1 = ix + 1;
      ix = std::max(xLo, std::min(xHi - 1, ix)); ix1 = std::max(xLo, std::min(xHi - 1, ix1));
      ix0s[i] = (ix - xLo) * 4; ix1s[i] = (ix1 - xLo) * 4;
      tc0s[i] = tileDiv(ix - org.x, tw) - tcMin; tc1s[i] = tileDiv(ix1 - org.x, tw) - tcMin;
      wxs[i] = wx;
    }
    cache[0].sy = INT32_MIN; cache[1].sy = INT32_MIN;
    // a source row of this rect, copied once: its pixels over vr's columns, per tile column whether the tile had bytes
    auto fetch = [&](int sy) -> JihadCpuSrcRow& {
      if (cache[0].sy == sy) return cache[0];
      if (cache[1].sy == sy) return cache[1];
      JihadCpuSrcRow& r = cache[sy & 1];
      r.sy = sy; r.px.SetLength((size_t)(xHi - xLo) * 4); r.ok.SetLength(nTc); r.alpha = false;
      const int tr = tileDiv(sy - org.y, th);
      const int tileY = org.y + tr * th;
      for (int c = 0; c < nTc; c++) {
        const int tc = tcMin + c;
        const JihadCpuTileRef t = tileAt(tc, tr);
        r.ok[c] = t.ok ? 1 : 0;
        if (!t.ok) continue;
        if (t.alpha) r.alpha = true;
        const int tx0 = org.x + tc * tw;
        const int x0 = std::max(xLo, tx0), x1 = std::min(xHi, tx0 + tw);
        if (x1 <= x0) continue;
        memcpy(r.px.Elements() + (size_t)(x0 - xLo) * 4, t.base + (size_t)(sy - tileY) * t.stride + (size_t)(x0 - tx0) * 4, (size_t)(x1 - x0) * 4);
      }
      return r;
    };
    for (int y = dr.y; y < dr.YMost(); y++) {
      const float v = ((float)y + 0.5f - aLf.fty) * inv - 0.5f;
      int iy = (int)floorf(v);
      int wy = (int)lroundf((v - (float)iy) * 256.0f);
      if (wy >= 256) { iy++; wy = 0; }
      int iy1 = iy + 1;
      iy = std::max(yLo, std::min(yHi - 1, iy)); iy1 = std::max(yLo, std::min(yHi - 1, iy1));
      const JihadCpuSrcRow& r0 = fetch(iy);
      const JihadCpuSrcRow& r1 = (iy1 == iy) ? r0 : fetch(iy1);
      const bool anyAlpha = r0.alpha || r1.alpha;
      const uint8_t* ok0 = r0.ok.Elements(); const uint8_t* ok1 = r1.ok.Elements();
      bool allOk = true;
      for (int c = 0; c < nTc; c++) { if (!ok0[c] || !ok1[c]) { allOk = false; break; } }
      // Two passes (the sixth build sampled each pixel in NEON with a transfer to ARM registers each, 160 ns a
      // pixel — no better than the scalar 200): first the two source rows blended DOWN into one row at this output
      // row's weight, 16 bytes an instruction group in NEON (plain bytes without it); then that row resampled
      // ACROSS, each output pixel the lerp of its two source pixels on packed 32-bit words (a pixel's two channel
      // pairs multiplied at once), "over" the standing pixel where the tiles carry alpha.
      const int spanPx = xHi - xLo;
      vrow.SetLength((size_t)spanPx * 4);
      {
        const uint8_t* s0 = r0.px.Elements(); const uint8_t* s1 = r1.px.Elements();
        uint8_t* v = vrow.Elements();
        const unsigned w1 = (unsigned)wy, w0 = 256 - w1;
        const size_t nBytes = (size_t)spanPx * 4;
        size_t k = 0;
        if (w1 == 0) { memcpy(v, s0, nBytes); k = nBytes; }
        else if (w0 == 0) { memcpy(v, s1, nBytes); k = nBytes; }
#if defined(__ARM_NEON__)
        else {
          // w0 and w1 are both 1..255 here and sum to 256: a*w0 + b*w1 <= 255*256 fits 16 bits, and the rounding
          // narrow by 8 is the division by the sum (the seventh build divided by 255 and overflowed: white wrapped to
          // black in whole bands, 2026-11-09's verify)
          const uint8x8_t vw0 = vdup_n_u8((uint8_t)w0);
          const uint8x8_t vw1 = vdup_n_u8((uint8_t)w1);
          for (; k + 16 <= nBytes; k += 16) {
            const uint8x16_t a = vld1q_u8(s0 + k), b = vld1q_u8(s1 + k);
            uint16x8_t lo = vmull_u8(vget_low_u8(a), vw0);  lo = vmlal_u8(lo, vget_low_u8(b), vw1);
            uint16x8_t hi = vmull_u8(vget_high_u8(a), vw0); hi = vmlal_u8(hi, vget_high_u8(b), vw1);
            vst1q_u8(v + k, vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8)));
          }
        }
#endif
        for (; k < nBytes; k++) v[k] = (uint8_t)((s0[k] * w0 + s1[k] * w1 + 128) >> 8);
      }
      const uint32_t* vr32 = reinterpret_cast<const uint32_t*>(vrow.Elements());
      uint8_t* d = aDest + (size_t)y * aStride + (size_t)dr.x * 4;
      rowBuf.SetLength((size_t)dr.width * 4);
      uint32_t* o32 = reinterpret_cast<uint32_t*>(rowBuf.Elements());
      if (anyAlpha || !allOk) memcpy(rowBuf.Elements(), d, (size_t)dr.width * 4);   // "over" needs the standing pixels; so does a pixel with no source
      const int* ix0 = ix0s.Elements(); const int* ix1 = ix1s.Elements(); const int* wxp = wxs.Elements();
      const int* tc0 = tc0s.Elements(); const int* tc1 = tc1s.Elements();
      for (int i = 0; i < dr.width; i++) {
        if (!allOk && (!ok0[tc0[i]] || !ok0[tc1[i]] || !ok1[tc0[i]] || !ok1[tc1[i]])) { diagSkip++; continue; }
        diagPx++;
        const uint32_t p = vr32[ix0[i] >> 2], q = vr32[ix1[i] >> 2];
        const uint32_t w1 = (uint32_t)wxp[i], w0 = 256 - w1;
        const uint32_t rb = (((p & 0x00FF00FFu) * w0 + (q & 0x00FF00FFu) * w1) >> 8) & 0x00FF00FFu;
        const uint32_t ag = (((p >> 8) & 0x00FF00FFu) * w0 + ((q >> 8) & 0x00FF00FFu) * w1) & 0xFF00FF00u;
        uint32_t out = rb | ag;
        if (anyAlpha) {
          const uint32_t a = out >> 24;
          if (a == 0) continue;
          if (a < 255) {
            const uint32_t dst = o32[i], k = 255 - a;
            uint32_t drb = (dst & 0x00FF00FFu) * k, dag = ((dst >> 8) & 0x00FF00FFu) * k;
            drb += 0x00800080u; drb = ((drb + ((drb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
            dag += 0x00800080u; dag = (dag + ((dag >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
            out += drb | dag;   // premultiplied: each channel stays within its byte
          }
        } else {
          out |= 0xFF000000u;
        }
        o32[i] = out;
      }
      memcpy(d, rowBuf.Elements(), (size_t)dr.width * 4);
    }
    aN.rects++;
  }
  if (diag) JIHAD_GPU_LOG("[jihad-gpu] scaler leaf done: %ld rects drawn, %ld pixels written, %ld skipped (no tile bytes), %.1f ms\n", diagRects, diagPx, diagSkip, (TimeStamp::Now() - diagT0).ToMilliseconds());
}

// premultiplied "over" of n pixels (B8G8R8A8), as the compositor's blend (ONE, ONE_MINUS_SRC_ALPHA)
static inline void
JihadCpuOver(uint8_t* d, const uint8_t* s, int n)
{
  for (int x = 0; x < n; x++, d += 4, s += 4) {
    const unsigned a = s[3];
    if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; continue; }
    if (!a) continue;
    const unsigned k = 255 - a;
    d[0] = (uint8_t)(s[0] + (d[0] * k + 127) / 255);
    d[1] = (uint8_t)(s[1] + (d[1] * k + 127) / 255);
    d[2] = (uint8_t)(s[2] + (d[2] * k + 127) / 255);
    d[3] = (uint8_t)(s[3] + (d[3] * k + 127) / 255);
  }
}

// the bands drawn into the slot: cleared opaque white as BeginFrame clears the window target (the blank frame the
// daemon knows, CompositorOGL.cpp), each leaf "over" in paint order (a tile's rows copied, blended when its format
// carries alpha; a colour layer's bounds filled), the alpha byte then 0xFF as the read forces it (the adapter
// blits the words raw)
static void
JihadCpuDraw(JihadFrameRequest* aReq, const nsTArray<JihadCpuLeaf>& aLeaves, const int aBands[2][2], int aW, int aH, JihadCpuCount& aN)
{
  uint8_t* dest = aReq->dest;
  const int stride = aReq->stride;
  nsTArray<uint8_t> rowBuf;   // a row composed here and written to the frame once (write-through memory)
  for (int b = 0; b < 2; b++) {
    const int lo = std::max(0, aBands[b][0]), hi = std::min(aH, aBands[b][1]);
    if (hi <= lo) continue;
    const gfx::IntRect band(0, lo, aW, hi - lo);
    memset(dest + (size_t)lo * stride, 0xFF, (size_t)(hi - lo) * stride);
    for (size_t k = 0; k < aLeaves.Length(); k++) {
      const JihadCpuLeaf& lf = aLeaves[k];
      Layer* l = lf.layer;
      if (l->GetType() == Layer::TYPE_COLOR) {
        ColorLayer* cl = l->AsColorLayer();
        gfx::IntRect r = cl->GetBounds();
        if (lf.scaled) {   // the scaler: the bounds at the scale, the pixels whose centres they cover
          r = JihadCoveredPixels(gfx::Rect(r.x * lf.scale + lf.ftx, r.y * lf.scale + lf.fty, r.width * lf.scale, r.height * lf.scale));
        } else {
          r.MoveBy(lf.tx, lf.ty);
        }
        r = r.Intersect(lf.clip).Intersect(band);
        if (r.IsEmpty()) continue;
        const gfx::Color c = cl->GetColor();
        const float a = std::max(0.0f, std::min(1.0f, c.a * l->GetEffectiveOpacity()));
        uint8_t px[4] = { (uint8_t)lroundf(std::max(0.0f, std::min(1.0f, c.b)) * a * 255.0f),
                          (uint8_t)lroundf(std::max(0.0f, std::min(1.0f, c.g)) * a * 255.0f),
                          (uint8_t)lroundf(std::max(0.0f, std::min(1.0f, c.r)) * a * 255.0f),
                          (uint8_t)lroundf(a * 255.0f) };
        // The fill is composed in a cached row and written to the frame once per row: the frame rows are
        // write-through memory (JihadFrameWindow.h), where a byte store or a read costs a bus round trip each —
        // Wikipedia's white background under a band took ~40 ms of the fourth scaler build's 43 ms "draw"
        // (2026-11-09), the scaler itself a few; a memcpy writes whole words in bursts.
        rowBuf.SetLength((size_t)r.width * 4);
        uint8_t* rb = rowBuf.Elements();
        if (px[3] == 255) {
          for (int x = 0; x < r.width; x++) memcpy(rb + (size_t)x * 4, px, 4);
          for (int y = r.y; y < r.YMost(); y++) memcpy(dest + (size_t)y * stride + (size_t)r.x * 4, rb, (size_t)r.width * 4);
        } else {
          for (int y = r.y; y < r.YMost(); y++) {
            uint8_t* d = dest + (size_t)y * stride + (size_t)r.x * 4;
            memcpy(rb, d, (size_t)r.width * 4);
            for (int x = 0; x < r.width; x++) JihadCpuOver(rb + (size_t)x * 4, px, 1);
            memcpy(d, rb, (size_t)r.width * 4);
          }
        }
        aN.rects++;
        continue;
      }
      TiledContentHost* th = l->AsLayerComposite()->GetCompositableHost()->AsTiledContentHost();
      TiledLayerBufferComposite& tb = th->JihadTiles();
      // the region drawn: the valid tiles within the visible region (its bounds when the paint resamples, as
      // PaintedLayerComposite::RenderLayer decides it from the layer at every composite), as
      // TiledContentHost::RenderLayerBuffer takes it
      nsIntRegion region = tb.GetValidRegion();
      if (l->MayResample()) region.AndWith(l->GetLocalVisibleRegion().GetBounds().ToUnknownRect());
      else region.AndWith(l->GetLocalVisibleRegion().ToUnknownRegion());
      if (lf.scaled) {   // the scaler: the band's rows in layer pixels (a row wider each side for the samples' neighbours)
        const int ly0 = (int)floorf((band.y - lf.fty) / lf.scale) - 1, ly1 = (int)ceilf((band.YMost() - lf.fty) / lf.scale) + 1;
        region.AndWith(gfx::IntRect(INT32_MIN / 2, ly0, INT32_MAX, ly1 - ly0));
        JihadCpuDrawScaled(dest, stride, lf, tb, region, band, aN);
        aN.tiles++;
        continue;
      }
      // only the tiles under this band are touched (the rest were not checked, review F9)
      {
        gfx::IntRect bandLayer = band;
        bandLayer.MoveBy(-lf.tx, -lf.ty);
        region.AndWith(bandLayer);
      }
      const gfx::IntSize ts = tb.GetTileSize();
      for (size_t i = 0; i < tb.GetTileCount(); i++) {
        TileHost& tile = tb.GetTile(i);
        if (tile.IsPlaceholderTile()) continue;
        const gfx::IntPoint off = tb.GetTileOffset(tb.GetPlacement().TilePosition(i));
        nsIntRegion tr(gfx::IntRect(off, tb.GetScaledTileSize()));
        tr.AndWith(region);
        if (tr.IsEmpty()) continue;
        BufferTextureHost* bh = tile.mTextureHost->AsBufferTextureHost();
        if (!bh || bh->GetBufferDescriptor().type() != BufferDescriptor::TRGBDescriptor) continue;
        const RGBDescriptor& rgb = bh->GetBufferDescriptor().get_RGBDescriptor();
        const int sstride = ImageDataSerializer::GetRGBStride(rgb);
        const uint8_t* src = bh->GetBuffer();
        const bool alpha = rgb.format() == gfx::SurfaceFormat::B8G8R8A8;
        bool used = false;
        for (auto it = tr.RectIter(); !it.Done(); it.Next()) {
          gfx::IntRect r = it.Get();
          r.MoveBy(lf.tx, lf.ty);
          r = r.Intersect(lf.clip).Intersect(band);
          if (r.IsEmpty()) continue;
          // the tile's pixel under the frame's (x, y) is (x - tx - off.x, y - ty - off.y)
          const int sx = r.x - lf.tx - off.x, sy = r.y - lf.ty - off.y;
          if (sx < 0 || sy < 0 || sx + r.width > ts.width || sy + r.height > ts.height) continue;
          if (alpha) rowBuf.SetLength((size_t)r.width * 4);
          for (int y = 0; y < r.height; y++) {
            const uint8_t* s = src + (size_t)(sy + y) * sstride + (size_t)sx * 4;
            uint8_t* d = dest + (size_t)(r.y + y) * stride + (size_t)r.x * 4;
            if (alpha) {   // "over" needs the standing pixels: read once, blend in the cached row, write once
              uint8_t* rb = rowBuf.Elements();
              memcpy(rb, d, (size_t)r.width * 4); JihadCpuOver(rb, s, r.width); memcpy(d, rb, (size_t)r.width * 4);
            } else {
              memcpy(d, s, (size_t)r.width * 4);
            }
          }
          aN.rects++;
          used = true;
        }
        if (used) aN.tiles++;
      }
    }
    for (int y = lo; y < hi; y++) {
      uint8_t* p = dest + (size_t)y * stride + 3;
      for (int x = 0; x < aW; x++, p += 4) *p = 0xFF;
    }
  }
}

// T-505 item 4: the band frame composed on the CPU (JihadCpuCollect, JihadCpuDraw), the tree prepared by the caller
// (the shadow properties and the async transforms at this frame's scroll, the visible regions and the effective
// transforms at the frame's world). True when the frame is done this way (*aOk its outcome); false sends it to the
// GPU band pass, nothing written
static bool
JihadCpuBands(JihadFrameRequest* aReq, Layer* aRoot, const int aBands[2][2], int aWidth, int aHeight,
              CompositorOGL* aOgl, bool* aOk, double* aMsShift, double* aMsDraw)
{
  sJihadCpu.frames++;
  sJihadCpu.leaves = sJihadCpu.tiled = sJihadCpu.colour = sJihadCpu.tiles = sJihadCpu.rects = 0;
  nsTArray<JihadCpuLeaf> leaves;
  const bool can = JihadCpuCollect(aRoot, RenderTargetIntRect(0, 0, aWidth, aHeight), true, aBands, aWidth, aHeight, leaves, sJihadCpu);
  if (!can) { JihadCpuLog(); return false; }
  sJihadCpu.could++;
  if (leaves.IsEmpty()) sJihadCpu.none++;
  // the rows kept, moved first (the GPU pass moves them while its bands compose)
  bool ok = true;
  const TimeStamp s0 = TimeStamp::Now();
  if (aReq->cancelled) ok = false; else aOgl->JihadShiftRowsNow(aReq);   // T-509: given up, dest untouched
  *aMsShift = (TimeStamp::Now() - s0).ToMilliseconds();
  const TimeStamp d0 = TimeStamp::Now();
  if (ok) JihadCpuDraw(aReq, leaves, aBands, aWidth, aHeight, sJihadCpu);
  if (ok && aReq->cancelled) ok = false;
  *aMsDraw = (TimeStamp::Now() - d0).ToMilliseconds();
  sJihadCpu.cpu++;
  JihadCpuLog();
  *aOk = ok;
  return true;
}

// T-544: the frame's fixed and stuck rects (the last JihadUpdateLastFixed's) out to the daemon
static void
JihadStuckOut(JihadFrameRequest* aReq, const gfx::IntRect* aRects, int aN, const uint32_t* aSig = nullptr)
{
  aReq->nStuck = 0;
  for (int k = 0; k < aN && aReq->nStuck < 4; k++) {
    if (aRects[k].IsEmpty()) continue;
    aReq->partsSig[aReq->nStuck] = aSig ? aSig[k] : 0;   // T-505 (per part): the rect's own signature
    int* o = aReq->stuck[aReq->nStuck++];
    o[0] = aRects[k].x; o[1] = aRects[k].y; o[2] = aRects[k].XMost(); o[3] = aRects[k].YMost();
  }
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
    // (review F3) the union may now overlap a third rect: joined again, so no two rects overlap (the daemon's
    // beneath records and restores take each rect's pixels once)
    for (bool merged = true; merged && aN > 1;) {
      merged = false;
      for (int p = 0; p < aN && !merged; p++) {
        for (int q = p + 1; q < aN && !merged; q++) {
          if (!aK[p].IsEmpty() || !aK[q].IsEmpty()) continue;
          gfx::IntRect a = aR[p];
          a.Inflate(1);
          if (a.Intersects(aR[q])) { aR[p] = aR[p].Union(aR[q]); remove(q); merged = true; }
        }
      }
    }
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
    return l->GetIsFixedPosition() || (l->GetIsStickyPosition() && (sJihadContentOnly || (mCompositionManager && mCompositionManager->JihadIsStuck(l))));
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
              JIHAD_GPU_LOG("[jihad-gpu]   above rect %d (%d,%d %dx%d): layer %p type %d flags 0x%x fixed %d sticky %d depth %d at %d,%d %dx%d (%u rects)\n",
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
                if (logThis) JIHAD_GPU_LOG("[jihad-gpu] keep %d,%d %dx%d dropped: layer %p (type %d) above it at %d,%d %dx%d\n",
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
            // T-505: a job without its parts keeps nothing; each part is its layer's whole rect, the same whether the
            // job hid the parts (a frame: their opacity 0 here) or showed them alone (the parts' picture)
            if (!sNoKeep && !sJihadContentOnly && fixedDepth == 0 && l->GetEffectiveOpacity() >= 0.999f && !l->GetMaskLayer() &&
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
              JIHAD_GPU_LOG("[jihad-gpu]   layer 2d %d axis %d, own mask %d, ancestor masks %u, vis bounds %d,%d %dx%d, transform %.3f %.3f %.3f %.3f %.1f %.1f\n",
                      (int)l2d, (int)(l2d && lm.PreservesAxisAlignedRectangles()), (int)(l->GetMaskLayer() != nullptr), (unsigned)l->GetAncestorMaskLayerCount(),
                      vis.GetBounds().x, vis.GetBounds().y, vis.GetBounds().width, vis.GetBounds().height, lm._11, lm._12, lm._21, lm._22, lm._31, lm._32);
              for (Layer* c = l->GetFirstChild(); c; c = c->GetNextSibling()) {
                gfx::Matrix cm;
                const bool c2d = c->GetEffectiveTransform().Is2D(&cm) && cm.PreservesAxisAlignedRectangles();
                const gfx::IntRect cb = c->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
                JIHAD_GPU_LOG("[jihad-gpu]   child %p type %d flags 0x%x opacity %.2f mask %d 2d %d visible %d,%d %dx%d\n", c, (int)c->GetType(),
                        (unsigned)c->GetContentFlags(), c->GetEffectiveOpacity(), (int)(c->GetMaskLayer() != nullptr), (int)c2d, cb.x, cb.y, cb.width, cb.height);
              }
              auto dumpRegion = [](const char* aName, const nsIntRegion& aRg) {
                JIHAD_GPU_LOG("[jihad-gpu]   %s (%u rects):", aName, (unsigned)aRg.GetNumRects());
                int q = 0;
                for (auto it = aRg.RectIter(); !it.Done() && q < 6; it.Next(), q++) {
                  const gfx::IntRect& rr = it.Get();
                  fprintf(stderr, " %d,%d %dx%d", rr.x, rr.y, rr.width, rr.height);
                }
                fprintf(stderr, "\n");
              };
              dumpRegion("covered", vis);
              dumpRegion("opaque", opaque);
              JIHAD_GPU_LOG("[jihad-gpu] fixed rect %d,%d %dx%d from layer %p (type %d, flags 0x%x, opacity %.2f, depth %d): keep %d,%d %dx%d (opaque bounds %d,%d %dx%d)\n",
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
    JIHAD_GPU_LOG("[jihad-gpu] last fixed now:");
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
        JIHAD_GPU_LOG("[jihad-gpu] keep %d,%d %dx%d not stable; the reference keeps:", aKeeps[i].x, aKeeps[i].y, aKeeps[i].width, aKeeps[i].height);
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

// T-511, the column read: the frame's bands are read as one column (x0..x1) when the daemon said
// which columns its bands are stale in, nothing else asks for whole rows (a shift and its entering
// rows, the rows kept from another frame, the fixed layers' columns), and the column that the
// change and the stale columns span is under three quarters of the width. A 480 px WebGL canvas
// on a 768 px card changes 482 rows a frame; every one of them was read whole.
static bool
JihadColumnRead(const JihadFrameRequest* aReq, int aInvX0, int aInvX1, int aInvLo, int aInvHi, int aWidth,
                int* aX0, int* aX1)
{
  static const bool sOff = getenv("JIHAD_GPU_NO_COLUMN_READ") != nullptr;
  if (sOff || !aReq->colsKnown || aReq->shift != 0 || aReq->src || aReq->nCols > 0 || aReq->nKeep > 0 || aWidth <= 0) {
    return false;
  }
  int x0 = aWidth, x1 = 0;
  if (aReq->colX1 > aReq->colX0) { x0 = aReq->colX0; x1 = aReq->colX1; }
  if (aInvHi > aInvLo) {
    if (aInvX1 <= aInvX0) return false;   // rows changed in columns not known: whole rows
    x0 = std::min(x0, aInvX0); x1 = std::max(x1, aInvX1);
  }
  x0 = std::max(0, x0 & ~7); x1 = std::min(aWidth, (x1 + 7) & ~7);
  if (x1 <= x0 || (x1 - x0) * 4 > aWidth * 3) {
    return false;
  }
  *aX0 = x0; *aX1 = x1;
  return true;
}

// T-605: the content's layer transactions applied so far (ShadowLayersUpdated), stamped on each frame job when it
// composes: the daemon's verify compares a frame only with a reference composed on the same tree
static std::atomic<uint32_t> sJihadTreeSeq{0};

bool
CompositorBridgeParent::JihadComposeAndRead(JihadFrameRequest* aReq, float aScale, float aOffsetX, float aOffsetY)
{
  if (aReq) aReq->treeSeq = sJihadTreeSeq.load();
  // T-509 device test (JIHAD_GPU_TEST_STALL=<n>): the nth job holds the compositor thread 6 s, as a
  // GPU wait that does not come back would; the daemon gives the frame up and goes on in software
  {
    static const int sStallAt = getenv("JIHAD_GPU_TEST_STALL") ? atoi(getenv("JIHAD_GPU_TEST_STALL")) : 0;
    static int sJobsSeen = 0;
    if (sStallAt > 0 && aReq && ++sJobsSeen == sStallAt) {
      JIHAD_GPU_LOG("[jihad-gpu] JIHAD_GPU_TEST_STALL: job %d holds the compositor thread 6 s\n", sJobsSeen);
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
    float probePrevY = 0.0f, probe = 0.0f;   // T-504 (review): the sticky probe the next real frame starts from
    JihadVerifyGuard(CompositorBridgeParent* aB, bool aOn) : b(aB), on(aOn) {
      if (!on) return;
      if (b->mCompositionManager) b->mCompositionManager->JihadProbeState(&probePrevY, &probe);
      world = b->mJihadLastWorld; sx = b->mJihadLastScrollX; sy = b->mJihadLastScrollY; sv = b->mJihadLastScrollValid;
      seq = b->mJihadSeqAtLast; seqKnown = b->mJihadSeqKnown; size = b->mJihadLastSize; worldKnown = b->mJihadWorldKnown;
      fixLo = b->mJihadLastFixedLo; fixHi = b->mJihadLastFixedHi; fixN = b->mJihadLastFixedN; prevN = b->mJihadPrevKeepN;
      for (int i = 0; i < 4; i++) { rects[i] = b->mJihadLastFixedRects[i]; keeps[i] = b->mJihadLastKeep[i]; prev[i] = b->mJihadPrevKeep[i]; }
      if (b->mLayerManager) b->mLayerManager->JihadHoldBaseline(true);
    }
    ~JihadVerifyGuard() {
      if (!on) return;
      if (b->mCompositionManager) b->mCompositionManager->JihadSetProbeState(probePrevY, probe);
      b->mJihadLastWorld = world; b->mJihadLastScrollX = sx; b->mJihadLastScrollY = sy; b->mJihadLastScrollValid = sv;
      b->mJihadSeqAtLast = seq; b->mJihadSeqKnown = seqKnown; b->mJihadLastSize = size; b->mJihadWorldKnown = worldKnown;
      b->mJihadLastFixedLo = fixLo; b->mJihadLastFixedHi = fixHi; b->mJihadLastFixedN = fixN; b->mJihadPrevKeepN = prevN;
      for (int i = 0; i < 4; i++) { b->mJihadLastFixedRects[i] = rects[i]; b->mJihadLastKeep[i] = keeps[i]; b->mJihadPrevKeep[i] = prev[i]; }
      if (b->mLayerManager) b->mLayerManager->JihadHoldBaseline(false);
    }
  } jihadVerifyGuard(this, aReq && aReq->verify);
  // T-505: a frame without its parts (flag 128), for this job's whole run; its reads are whole rows, so
  // a partial composite of a change is whole rows too (LayerManagerComposite::JihadWholeRows)
  struct JihadContentOnlyScope {
    LayerManagerComposite* m;
    JihadContentOnlyScope(LayerManagerComposite* aM, bool aOn) : m(aM) { sJihadContentOnly = aOn; if (m) m->JihadWholeRows(aOn); }
    ~JihadContentOnlyScope() { sJihadContentOnly = false; if (m) m->JihadWholeRows(false); }
  } jihadContentOnlyScope(mLayerManager, aReq && aReq->contentOnly);
  // (review F2) one sample time for the job's prep, its parts pass and its frame: an animated part is measured,
  // pictured and composed at the same instant (mTestTime is the mechanism the test API uses; restored after)
  struct JihadJobTimeScope {
    CompositorBridgeParent* b; bool was; TimeStamp wasTime;
    JihadJobTimeScope(CompositorBridgeParent* aB, bool aOn) : b(aB), was(aB->mIsTesting), wasTime(aB->mTestTime) {
      if (aOn && !was) { b->mIsTesting = true; b->mTestTime = TimeStamp::Now(); }
    }
    ~JihadJobTimeScope() { if (!was) { b->mIsTesting = false; b->mTestTime = wasTime; } }
  } jihadJobTimeScope(this, aReq && aReq->contentOnly);
  if (aReq) aReq->partsGen = gJihadPartsGen;
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
  aReq->readX0 = aReq->readX1 = 0; aReq->invX0 = aReq->invX1 = 0;   // T-511
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
  // T-505: a frame with no base to copy (its slot's own frame, its window moved or its rows shifted in place, or
  // none) has the diff leave out the card's scroll too, as one copied from the newest frame does: a layer
  // transaction during a pan was the whole frame changed for it (the content's translation), and a whole read
  // with the other two slots' frames made stale whole. Only the card's scroll: the shift the diff measured must
  // be the card's since the last composite (a scroll the page made itself is then the whole frame changed)
  // (review: not for a frame with the engine's parts while fixed or stuck rects exist: its keeps and columns are
  // judged against the last composite, not the older base)
  const bool ownComp = aReq->src == nullptr && aReq->scrollValid && mJihadLastScrollValid &&
                       aReq->scrollX == mJihadLastScrollX && (sJihadContentOnly || mJihadLastFixedN == 0);
  const float ownExpectRows = (aReq->scrollY - mJihadLastScrollY) * (aScale > 0.0f ? aScale : 1.0f);
  auto ownShiftOk = [&]() {
    float sdx = 0.0f, sdy = 0.0f;
    if (!JihadDiffScrollShift(&sdx, &sdy)) return true;   // nothing was left out
    return fabsf(-sdy * (aScale > 0.0f ? aScale : 1.0f) - ownExpectRows) < 0.5f;
  };
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
  // T-505 (E2): a frame without its parts: their list, signature and the card's hold set from this job's tree state,
  // and their picture read here when the daemon holds none of this signature (CompositorOGL.h, partsDest)
  bool jPartsPrepped = false;
  if (aReq->contentOnly && !aReq->overviewParts && !aReq->verify && mCompositionManager && mLayerManager->GetRoot()) {
    jPartsPrepped = true;
    gfx::IntRect parts[4], pk[4];
    mLayerManager->BeginTransaction();
    SetShadowProperties(mLayerManager->GetRoot());
    mCompositionManager->ComputeRotation();
    {
      TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
      mCompositionManager->TransformShadowTree(time, mVsyncRate);
    }
    mLayerManager->JihadPrepareOnly();
    const int partN = JihadFixedRectsNow(aWidth, aHeight, parts, pk);   // every fixed and sticky layer, whole rects
    bool covered = false;
    uint32_t rectSig[16] = {};
    aReq->partsGen = JihadPartsSig(parts, partN, &covered, aWidth, aHeight, rectSig);
    if (covered) aReq->partsFlags |= 1;
    {
      sJihadContentOnly = false;   // the card's hold set keeps T-544's meaning: fixed layers and stuck ones
      gfx::IntRect hr[4], hk[4];
      const int hn = JihadFixedRectsNow(aWidth, aHeight, hr, hk);
      sJihadContentOnly = true;
      aReq->nHold = 0;
      for (int k = 0; k < hn && aReq->nHold < 4; k++) {
        if (hr[k].IsEmpty()) continue;
        int* o = aReq->hold[aReq->nHold++];
        o[0] = hr[k].x; o[1] = hr[k].y; o[2] = hr[k].XMost(); o[3] = hr[k].YMost();
      }
    }
    mLayerManager->JihadEndTransactionNoComposite();
    JihadStuckOut(aReq, parts, partN, rectSig);
    // T-505 (per part): the rects to read — with the list of signatures the daemon holds, the ones not in it; with
    // the one signature, every rect when the whole differs
    int want = 0;
    if (!covered && aReq->partsDest && aReq->nStuck > 0) {
      if (aReq->nPartsHave >= 0) {
        for (int k = 0; k < aReq->nStuck; k++) {
          bool have = false;
          for (int j = 0; j < aReq->nPartsHave && j < 16 && !have; j++) have = aReq->partsHaveList[j] == aReq->partsSig[k];
          if (!have) want |= 1 << k;
        }
      } else if (aReq->partsGen != aReq->partsHave) {
        want = (1 << aReq->nStuck) - 1;
      }
    }
    if (want) {
      size_t need = 0;
      for (int k = 0; k < aReq->nStuck; k++)
        if (want & (1 << k)) need += (size_t)(aReq->stuck[k][2] - aReq->stuck[k][0]) * (aReq->stuck[k][3] - aReq->stuck[k][1]) * 4;
      // the rects cut into pieces no taller than the surface, on shelves across its width (12 regions at most)
      struct Piece { int k, x, y, w, h, atX, atY; } pieces[12];
      int np = 0, used = 0;
      auto pack = [&](int aCap) {
        np = 0; used = 0;
        int shelfY = 0, shelfH = 0, cursorX = 0;
        for (int k = 0; k < aReq->nStuck; k++) {
          if (!(want & (1 << k))) continue;   // a rect the daemon has
          const int x0 = aReq->stuck[k][0], w = aReq->stuck[k][2] - x0;
          for (int y = aReq->stuck[k][1]; y < aReq->stuck[k][3];) {
            const int hgt = std::min(aCap, aReq->stuck[k][3] - y);
            if (cursorX + w > aWidth) { shelfY += shelfH; shelfH = 0; cursorX = 0; }
            if (w > aWidth || shelfY + hgt > aCap || np == 12) return false;
            pieces[np++] = { k, x0, y, w, hgt, cursorX, shelfY };
            cursorX += w; shelfH = std::max(shelfH, hgt); y += hgt;
          }
        }
        used = shelfY + shelfH;
        return true;
      };
      const bool fits = need <= (size_t)aReq->partsCap &&
                        (pack(ogl->JihadBandRows()) || (ogl->JihadBandTallRows() > 0 && pack(ogl->JihadBandTallRows())));
      if (!fits) {
        aReq->partsFlags |= 2;
      } else {
        JihadVerifyGuard partsGuard(this, true);   // the baseline, the probe, the last scroll and view held
        struct PartsCancelScope { CompositorOGL* o; ~PartsCancelScope() { o->JihadSetCancel(nullptr); } } partsCancelScope{ogl};
        ogl->JihadSetCancel(&aReq->cancelled);   // (review F6) a job given up writes no more of its buffer
        const int passRows = ogl->JihadBeginBandPass(aWidth, used);
        if (!passRows) {
          aReq->partsFlags |= 2;
        } else {
          sJihadOverview = 2;   // the parts alone (JihadHideForJob)
          ogl->JihadSetClearTransparent(true);
          gfx::Matrix4x4 worlds[12];
          gfx::IntRect rects[12];
          for (int i = 0; i < np; i++) {
            worlds[i] = world;
            worlds[i].PostTranslate((float)(pieces[i].atX - pieces[i].x), (float)(pieces[i].atY - pieces[i].y), 0.0f);
            rects[i] = gfx::IntRect(pieces[i].atX, pieces[i].atY, pieces[i].w, pieces[i].h);
          }
          mLayerManager->JihadSetWorldTransform(worlds[0]);
          mLayerManager->UpdateRenderBounds(gfx::IntRect(0, 0, aWidth, passRows));
          mLayerManager->JihadIgnoreDiff(true);
          mLayerManager->JihadResetLastInvalid();
          mLayerManager->JihadSetBand(gfx::IntRect(), gfx::IntRect());
          mLayerManager->JihadSetRegions(worlds, rects, np);
          const TimeStamp pc0 = TimeStamp::Now();
          sJihadInCompose = true;
          ForceComposeToTarget(nullptr, nullptr);
          sJihadInCompose = false;
          mLayerManager->JihadSetRegions(nullptr, nullptr, 0);
          const TimeStamp pr0 = TimeStamp::Now();
          bool ok = true;
          size_t off[4] = {};   // each read rect's place in the buffer: the read ones packed in rect order
          for (int k = 0, o = 0; k < aReq->nStuck; k++) {
            off[k] = (size_t)o;
            if (want & (1 << k)) o += (aReq->stuck[k][2] - aReq->stuck[k][0]) * (aReq->stuck[k][3] - aReq->stuck[k][1]) * 4;
          }
          for (int i = 0; i < np && ok; i++) {
            if (aReq->cancelled) { ok = false; break; }   // (review F6)
            const int k = pieces[i].k, rw = aReq->stuck[k][2] - aReq->stuck[k][0];
            uint8_t* d = aReq->partsDest + off[k] + (size_t)(pieces[i].y - aReq->stuck[k][1]) * rw * 4;
            ok = ogl->JihadReadPacked(d, rw * 4, passRows, pieces[i].atX, pieces[i].w, pieces[i].atY, pieces[i].h);
          }
          ogl->JihadEndBandPass();
          ogl->JihadSetClearTransparent(false);
          sJihadOverview = 0;
          if (ok) { aReq->partsBytes = (int)need; aReq->partsRead = want; }
          static uint32_t sPassLog = 0;
          if (sPassLog++ < 20 || (sPassLog % 50) == 0) {
            int nRead = 0;
            for (int k = 0; k < aReq->nStuck; k++) if (want & (1 << k)) nRead++;
            JIHAD_GPU_LOG("[jihad-gpu] parts pass: %d of %d parts (mask %x), %d pieces on %d rows, %zu KB: compose %.1f ms, read %.1f ms%s\n",
                          nRead, aReq->nStuck, want, np, used, need / 1024, (pr0 - pc0).ToMilliseconds(), (TimeStamp::Now() - pr0).ToMilliseconds(),
                          ok ? "" : " (failed)");
          }
        }
        mLayerManager->JihadIgnoreDiff(false);
        mLayerManager->JihadSetWorldTransform(world);
        mLayerManager->UpdateRenderBounds(whole);
        mLayerManager->JihadSetBand(gfx::IntRect(), gfx::IntRect());
        ogl->JihadSelectTarget();
      }
    }
  }
  // Round 119: a frame whose tree changed takes the diff first, without a render. When what it
  // changed, the bands the daemon asked for, the entering band of a scroll the page made and the
  // fixed rows fit the band pass, the frame is composed there like a scroll-only one (the band
  // surface: the bands' GPU cost, not the frame's); otherwise the whole path composes it, the
  // diff's region invalidated by hand. JIHAD_GPU_NO_DIFF_FIRST=1 turns it off.
  bool diffFirst = false, diffTaken = false, dfHaveShift = false;
  int dfShift = 0, dfInvLo = 0, dfInvHi = 0, dfFixLo = 0, dfFixHi = 0, dfInvX0 = 0, dfInvX1 = 0;
  {
    static const bool sNoDiffFirst = getenv("JIHAD_GPU_NO_DIFF_FIRST") != nullptr;
    if (!aReq->verify && treeChanged && !viewChanged && !sNoDiffFirst && ogl->JihadBandMaxRows() > 0 && aReq->dest &&
        aReq->stride == aWidth * 4 && !getenv("JIHAD_GPU_READ_THREAD") && mCompositionManager &&
        mLayerManager->GetRoot()) {
      mLayerManager->JihadCompensateScroll((aReq->src != nullptr && aReq->engineShift) || ownComp);
      nsIntRegion dfRegion = JihadDiffNow();
      mLayerManager->JihadCompensateScroll(false);
      if (ownComp && !ownShiftOk()) dfRegion = nsIntRegion(gfx::IntRect(0, 0, aWidth, aHeight));   // T-505
      diffTaken = true;
      nsIntRegion dfOnFrame;
      dfOnFrame.And(dfRegion, whole);   // round 124: clipped to the frame before the bounds
      const gfx::IntRect cb = dfOnFrame.GetBounds();
      if (!cb.IsEmpty()) {
        dfInvLo = std::max(0, cb.y); dfInvHi = std::min(aHeight, cb.YMost());
        if (dfInvHi < dfInvLo) dfInvHi = dfInvLo;
        dfInvX0 = std::max(0, cb.x); dfInvX1 = std::min(aWidth, cb.XMost());   // T-511
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
          if (sJihadContentOnly || !(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
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
      // T-527: packed for the band surface first; a frame that does not fit it takes the tall one
      bool dfFits = total > 0 && dfSpan >= 0 && total + dfSpan <= ogl->JihadBandRows();
      const int dfTall = ogl->JihadBandTallRows();
      if (!dfFits && dfTall > 0 && total > 0) {
        int span2 = 0;
        if (JihadFixedCols()) {
          JihadChunk dfChunks2[12];
          int dfNChunks2 = 0;
          span2 = JihadPackColumns(dfCols, dfNCols, total, dfTall, aWidth, dfChunks2, &dfNChunks2);
        }
        if (span2 >= 0 && total + span2 <= dfTall) { dfSpan = span2; dfFits = true; }
      }
      if (dfFits) {
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
        JIHAD_GPU_LOG("[jihad-gpu] diff first: changed %d..%d, shift %d%s, fixed %d..%d -> %d rows, %s\n",
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
    int chunkRows = JihadPackColumns(aReq->cols, aReq->nCols, fullAsked, ogl->JihadBandRows(), aWidth, chunks, &nChunks);
    // T-527: packed for the band surface first; the tall one's rows only when that does not fit
    if (!(chunkRows >= 0 && fullAsked + chunkRows <= ogl->JihadBandRows()) && ogl->JihadBandTallRows() > 0) {
      JihadChunk chunks2[12];
      int nChunks2 = 0;
      const int cr2 = JihadPackColumns(aReq->cols, aReq->nCols, fullAsked, ogl->JihadBandTallRows(), aWidth, chunks2, &nChunks2);
      if (cr2 >= 0 && fullAsked + cr2 <= ogl->JihadBandTallRows()) {
        chunkRows = cr2; nChunks = nChunks2;
        for (int c = 0; c < nChunks2; c++) chunks[c] = chunks2[c];
      }
    }
    // T-513 (review): the regions of one composite are at most 12, bands included: chunks past that
    // would be read without being composed, so such a frame does not fit
    const bool fits = chunkRows >= 0 && fullAsked + chunkRows <= bandMax && nChunks + (b1 ? 1 : 0) + (b2 ? 1 : 0) <= 12;
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
      const TimeStamp tp0 = TimeStamp::Now();
      bool ok = top > 0 && top <= bandMax;
      double msBegin = 0, msCompose = 0, msRead = 0, msShift = 0, msEnd = 0;
      int passRows = 0;
      // T-517: the bands and the column chunks as regions of one frame (LayerManagerComposite::
      // JihadSetRegions); JIHAD_GPU_PASS_PER_REGION=1 composes each on its own, as before
      static const bool sPassPerRegion = getenv("JIHAD_GPU_PASS_PER_REGION") != nullptr;
      // T-505 item 4: the bands composed on the CPU from the tiles when every layer under them allows it
      // (the GPU path's default, JihadCpuBands above: a frame of full-width bands alone, no column chunks); the GPU
      // band pass otherwise, as before
      bool cpuDone = false;
      double msCpuPrep = 0, msCpuDraw = 0;
      if (ok && JihadCpuBandsWanted() && !aReq->verify && nChunks == 0 && aReq->nCols == 0 && !sPassPerRegion &&
          mCompositionManager && mLayerManager->GetRoot()) {
        const TimeStamp p0 = TimeStamp::Now();
        // the composite's steps before its render (CompositeToTarget's, as JihadDiffNow takes them): the shadow
        // properties from the layers, the async transforms at this frame's scroll, the parts hidden for a frame
        // without them, then the visible regions, the effective transforms at the frame's world and the tree
        // diff — taken and dropped as a band composite takes it (UpdateAndRender computes the differences
        // whatever it draws), which re-clones the diff's baseline at this frame's state: left at an older
        // frame's, the next diff-first frame measured the scroll of several frames as its shift and recorded
        // the frame rows off (the first verify round, 2026-11-04: 22 wrong frames in both lanes)
        Layer* root = mLayerManager->GetRoot();
        mLayerManager->BeginTransaction();
        SetShadowProperties(root);
        mCompositionManager->ComputeRotation();
        // the animations' clock as a composite sets it (the scheduler's, as ForceComposeToTarget notes it first:
        // sampled from an older composite's instant the next frame would rewind them, review F4)
        mCompositorScheduler->JihadNoteCompose();
        const TimeStamp time = mIsTesting ? mTestTime : mCompositorScheduler->GetLastComposeTime();
        const bool nextFrame = mCompositionManager->TransformShadowTree(time, mVsyncRate);
        if (sJihadContentOnly) JihadHideForJob();
        mLayerManager->JihadDiffOnly();
        mLayerManager->JihadEndTransactionNoComposite();
        bool cpuOk = false;
        cpuDone = JihadCpuBands(aReq, root, bands, aWidth, aHeight, ogl, &cpuOk, &msShift, &msCpuDraw);
        msCpuPrep = (TimeStamp::Now() - p0).ToMilliseconds() - msShift - msCpuDraw;
        if (cpuDone) {
          ok = cpuOk;
          // a composite's bookkeeping: the next frame asked when an animation moved or a composite-until time
          // stands (CompositeToTarget's tail); the content side told its transaction is composed (its paints wait
          // for that)
          if (nextFrame) { JihadReqNote("the next frame asked (an animation), a CPU band frame"); ScheduleComposition(); }
          else if (!mCompositor->GetCompositeUntilTime().IsNull()) { JihadReqNote("composite until, a CPU band frame"); ScheduleComposition(); }
          TimeStamp now = TimeStamp::Now();
          DidComposite(now, now);
        }
      }
      if (!cpuDone) {
      // the band pass: its own small surface and target when available (round 106), else the
      // stack at the top of the frame-sized target (round 105)
      passRows = top > 0 ? ogl->JihadBeginBandPass(aWidth, top) : 0;   // T-527: the band surface or the tall one
      if (!passRows) {
        // T-513 (review): stacked at the top of the frame-sized target, so the frame's own surface
        // current (a band frame before this one left the band pbuffer current, and on the Adreno 220
        // every target must match the current surface's size: memory compositor-scroll-traps)
        ogl->JihadUseFrameSurface();
        ogl->JihadEnsureTargetSize(aWidth, aHeight);
      }
      msBegin = (TimeStamp::Now() - tp0).ToMilliseconds();
      const gfx::IntRect passBounds = passRows ? gfx::IntRect(0, 0, aWidth, passRows) : whole;
      int at = 0;
      if (!sPassPerRegion && ok) {
        gfx::Matrix4x4 worlds[12];
        gfx::IntRect rects[12];
        int nr = 0;
        for (int b = 0; b < 2; b++) {
          if (!hs[b]) continue;
          const int lo = std::max(0, bands[b][0]);
          worlds[nr] = world;
          worlds[nr].PostTranslate(0.0f, (float)(at - lo), 0.0f);   // frame row lo -> stacked row at
          rects[nr++] = gfx::IntRect(0, at, aWidth, hs[b]);
          at += hs[b];
        }
        for (int c = 0; c < nChunks && nr < 12; c++) {
          const JihadChunk& ch = chunks[c];
          worlds[nr] = world;
          worlds[nr].PostTranslate((float)(ch.atX - ch.srcX), (float)(fullRows - ch.srcLo), 0.0f);
          rects[nr++] = gfx::IntRect(ch.atX, fullRows, ch.w, ch.h);
        }
        mLayerManager->JihadSetWorldTransform(nr ? worlds[0] : world);
        mLayerManager->UpdateRenderBounds(passBounds);    // the target and viewport are the surface's size
        mLayerManager->JihadIgnoreDiff(true);
        mLayerManager->JihadResetLastInvalid();
        mLayerManager->JihadSetBand(gfx::IntRect(), gfx::IntRect());
        mLayerManager->JihadSetRegions(worlds, rects, nr);
        if (!passRows) ogl->JihadSelectTarget();
        TimeStamp c0 = TimeStamp::Now();
        sJihadInCompose = true;
        ForceComposeToTarget(nullptr, nullptr);
        sJihadInCompose = false;
        mLayerManager->JihadSetRegions(nullptr, nullptr, 0);
        msCompose += (TimeStamp::Now() - c0).ToMilliseconds();
      }
      for (int b = 0; b < 2 && ok && sPassPerRegion; b++) {
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
      for (int c = 0; c < nChunks && ok && sPassPerRegion; c++) {
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
      // T-513 (review): the reads below belong to a request the daemon may give up meanwhile
      struct CancelScope { CompositorOGL* o; ~CancelScope() { o->JihadSetCancel(nullptr); } } cancelScope{ogl};
      ogl->JihadSetCancel(&aReq->cancelled);
      // T-511: one band and no column chunks: the band read as the column the change spans
      int cx0 = 0, cx1 = 0;
      // (T-505 review: not without the parts: the daemon puts back what lay beneath them by whole rows)
      const bool colRead = ok && diffFirst && !sJihadContentOnly && hs[0] > 0 && !hs[1] && nChunks == 0 &&
                           JihadColumnRead(aReq, dfInvX0, dfInvX1, dfInvLo, dfInvHi, aWidth, &cx0, &cx1);
      if (colRead) {
        TimeStamp r0 = TimeStamp::Now();
        ok = ogl->JihadReadColumns(aReq->dest, aReq->stride, passRows ? passRows : aHeight, cx0, cx1, 0, hs[0],
                                   std::max(0, bands[0][0]), cx0);
        aReq->readX0 = cx0; aReq->readX1 = cx1;
        msRead = (TimeStamp::Now() - r0).ToMilliseconds();
      } else if (ok) {
        TimeStamp r0 = TimeStamp::Now();
        uint8_t* scratch = fullRows ? ogl->JihadBandScratch(aWidth * 4 * fullRows) : nullptr;
        if (fullRows) ok = scratch && ogl->JihadReadFrame(scratch, aReq->stride, aWidth, passRows ? passRows : aHeight, 0, fullRows);
        if (ok && fullRows && aReq->cancelled) ok = false;   // T-513 (review): given up: the slot not touched
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
      msEnd = (TimeStamp::Now() - te0).ToMilliseconds();
      }   // the GPU band pass
      const double msEntry = (tp0 - tEntry).ToMilliseconds();
      mLayerManager->JihadIgnoreDiff(false);
      mLayerManager->JihadSetWorldTransform(world);
      mLayerManager->UpdateRenderBounds(whole);
      mLayerManager->JihadSetBand(gfx::IntRect(), gfx::IntRect());
      if (!cpuDone) ogl->JihadSelectTarget();
      aReq->invLo = diffFirst ? dfInvLo : 0; aReq->invHi = diffFirst ? dfInvHi : 0;
      aReq->invX0 = diffFirst ? dfInvX0 : 0; aReq->invX1 = diffFirst ? dfInvX1 : 0;   // T-511
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
          if (sJihadContentOnly || !(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
          const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
          if (vb.IsEmpty()) return;
          const gfx::Rect r = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
          const int a = std::max(0, (int)floor(r.y)), b = std::min(aHeight, (int)ceil(r.YMost()));
          if (logThis) {
            JIHAD_GPU_LOG("[jihad-gpu] fixed rows from layer %p (type %d, fixed %d, sticky %d, stuck %d, parent %p, flags 0x%x, opacity %.2f, rects %u): visible %d,%d %dx%d -> x %.0f..%.0f rows %d..%d\n",
                    l, (int)l->GetType(), (int)l->GetIsFixedPosition(), (int)l->GetIsStickyPosition(),
                    (int)(mCompositionManager && mCompositionManager->JihadIsStuck(l)), l->GetParent(),
                    (unsigned)l->GetContentFlags(), l->GetEffectiveOpacity(), (unsigned)l->GetLocalVisibleRegion().GetNumRects(),
                    vb.x, vb.y, vb.width, vb.height, r.x, r.XMost(), a, b);
          }
          if (b > a) { fLo = std::min(fLo, a); fHi = std::max(fHi, b); }
        });
        if (fHi > fLo) { mJihadLastFixedLo = fLo; mJihadLastFixedHi = fHi; } else { mJihadLastFixedLo = mJihadLastFixedHi = 0; }
        if (sJihadContentOnly) {
          gfx::IntRect pr[4], pk[4];   // T-505: where the parts are, for the daemon to draw them (E2: the prep's)
          if (!jPartsPrepped) JihadStuckOut(aReq, pr, JihadFixedRectsNow(aWidth, aHeight, pr, pk));
        } else {
          JihadUpdateLastFixed(aWidth, aHeight);   // rounds 138/141
          JihadStuckOut(aReq, mJihadLastFixedRects, mJihadLastFixedN);
        }
      } else if (diffFirst) {
        mJihadLastFixedLo = dfFixLo; mJihadLastFixedHi = dfFixHi;
      }
      aReq->fixedLo = mJihadLastFixedLo; aReq->fixedHi = mJihadLastFixedHi;
      aReq->composed = true;
      aReq->ok = ok;
      static uint32_t sBandFrames = 0;
      if (sBandFrames < 12 || (sBandFrames % 10) == 0) {
        if (cpuDone) {
          JIHAD_GPU_LOG("[jihad-gpu] compositor thread: cpu band frame %u: rows %d..%d and %d..%d, %d leaves (%d tiled, %d colour), %d tiles, %d rects, %d kept, entry %.1f ms, prep %.1f ms, shift %.1f ms, draw %.1f ms, all %.1f ms, ok=%d\n",
                  sBandFrames, aReq->lo, aReq->hi, aReq->lo2, aReq->hi2, sJihadCpu.leaves, sJihadCpu.tiled, sJihadCpu.colour, sJihadCpu.tiles, sJihadCpu.rects,
                  aReq->nKeep, msEntry, msCpuPrep, msShift, msCpuDraw, (TimeStamp::Now() - tEntry).ToMilliseconds(), (int)ok);
        } else {
          JIHAD_GPU_LOG("[jihad-gpu] compositor thread: band frame %u: rows %d..%d and %d..%d, x %d..%d, %d columns in %d chunks (%d rows), %d kept, entry %.1f ms, surface in %.1f ms, compose %.1f ms, shift %.1f ms, read %.1f ms, surface out %.1f ms, all %.1f ms, ok=%d\n",
                  sBandFrames, aReq->lo, aReq->hi, aReq->lo2, aReq->hi2, aReq->readX0, aReq->readX1 ? aReq->readX1 : aWidth, aReq->nCols, nChunks, chunkRows, aReq->nKeep, msEntry, msBegin, msCompose, msShift, msRead, msEnd, (TimeStamp::Now() - tEntry).ToMilliseconds(), (int)ok);
        }
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
  mLayerManager->JihadCompensateScroll(!diffTaken && ((aReq->src != nullptr && aReq->engineShift) || ownComp));   // round 115, T-505
  TimeStamp t0 = TimeStamp::Now();
  sJihadInCompose = true;
  sJihadOverview = aReq->overview ? 1 : (aReq->overviewParts ? 2 : 0);   // T-504
  ogl->JihadSetClearTransparent(sJihadOverview != 0);
  if (sJihadOverview && mCompositionManager) mCompositionManager->JihadSetProbeOverride(1024.0f);   // every window the same
  ForceComposeToTarget(nullptr, nullptr);
  if (mCompositionManager) mCompositionManager->JihadSetProbeOverride(0.0f);
  ogl->JihadSetClearTransparent(false);
  sJihadOverview = 0;
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
  gfx::IntRect changed = mLayerManager->JihadLastChanged();   // what the tree changed
  if (!diffTaken && ownComp && !ownShiftOk()) changed = gfx::IntRect(0, 0, aWidth, aHeight);   // T-505 (above)
  int invLo = 0, invHi = 0, invX0 = 0, invX1 = 0;
  if (!changed.IsEmpty()) {
    invLo = std::max(0, changed.y);
    invHi = std::min(aHeight, changed.YMost());
    if (invHi < invLo) invHi = invLo;
    invX0 = std::max(0, changed.x); invX1 = std::min(aWidth, changed.XMost());   // T-511
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
        if (sJihadContentOnly || !(l->GetIsFixedPosition() || (l->GetIsStickyPosition() && mCompositionManager && mCompositionManager->JihadIsStuck(l)))) return;
        const gfx::IntRect vb = l->GetLocalVisibleRegion().GetBounds().ToUnknownRect();
        if (vb.IsEmpty()) return;
        if (logThisW) {
          const gfx::Rect rr = l->GetEffectiveTransform().TransformBounds(gfx::Rect(vb.x, vb.y, vb.width, vb.height));
          JIHAD_GPU_LOG("[jihad-gpu] fixed rows (whole path) from layer %p (type %d, fixed %d, sticky %d, stuck %d): visible %d,%d %dx%d -> rows %.0f..%.0f\n",
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
            JIHAD_GPU_LOG("[jihad-gpu] fixed layer %p (parent %p): visible %d,%d %dx%d, effective translation %.1f,%.1f scale %.3f -> rows %.0f..%.0f\n",
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
  if (sJihadContentOnly) {
    gfx::IntRect pr[4], pk[4];   // T-505: where the parts are, for the daemon to draw them (E2: the prep's)
    if (!jPartsPrepped) JihadStuckOut(aReq, pr, JihadFixedRectsNow(aWidth, aHeight, pr, pk));
  }
  if (JihadFixedCols()) {
    gfx::IntRect lastR[4];
    const int lastN = mJihadLastFixedN;
    for (int k = 0; k < lastN; k++) lastR[k] = mJihadLastFixedRects[k];
    JihadUpdateLastFixed(aWidth, aHeight);   // prev = last, last = now
    JihadStuckOut(aReq, mJihadLastFixedRects, mJihadLastFixedN);
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
    // T-516: a frame that asks for its bands only (the first frame at a new zoom: the card's view) reads them
    // alone; the change is still reported (the daemon's records)
    int a[9][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2}, {aReq->bandsOnly ? 0 : invLo, aReq->bandsOnly ? 0 : invHi}, {eLo, eHi},
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
  aReq->invX0 = invX0; aReq->invX1 = invX1;
  {
    int cx0 = 0, cx1 = 0;   // T-511: the bands read as one column (JihadReadAsync)
    if (!aReq->bandsOnly && !sJihadContentOnly && JihadColumnRead(aReq, invX0, invX1, invLo, invHi, aWidth, &cx0, &cx1)) { aReq->readX0 = cx0; aReq->readX1 = cx1; }
  }
  // the log's values now: once the read hands the frame over, aReq belongs to the daemon
  const int logLo2 = aReq->lo2, logHi2 = aReq->hi2, logFixedLo = aReq->fixedLo, logFixedHi = aReq->fixedHi, logCols = aReq->nCols, logKeep = aReq->nKeep;
  const int logReadX0 = aReq->readX0, logReadX1 = aReq->readX1;
  const bool logScrollOnly = aReq->scrollOnly;
  const bool started = ogl->JihadReadAsync(aReq);
  static uint32_t sFrames = 0;
  static const bool sTimeline = getenv("JIHAD_GPU_TIMELINE") != nullptr;   // T-515 device rounds: every frame
  if (sFrames < 20 || (sFrames % 50) == 0 || sTimeline) {
    JIHAD_GPU_LOG("[jihad-gpu] compositor thread: compose %.1f ms, handoff %.1f ms (frame %u, rows %d..%d and %d..%d of %d, x %d..%d, %d columns, %d kept, changed %d..%d x %d..%d, fixed %d..%d%s%s, started=%d)\n",
            (t1 - t0).ToMilliseconds(), (TimeStamp::Now() - t1).ToMilliseconds(), sFrames,
            lo, hi, logLo2, logHi2, aHeight, logReadX0, logReadX1 ? logReadX1 : aWidth, logCols, logKeep, invLo, invHi, invX0, invX1,
            logFixedLo, logFixedHi, treeChanged ? ", tree changed" : "", logScrollOnly ? ", scroll only" : "", (int)started);
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
  if (sJihadContentOnly) JihadHideForJob();   // T-505: as the frame will be composed
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
  sJihadTreeSeq++;   // T-605
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
    JihadReqNote("layers updated");
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
