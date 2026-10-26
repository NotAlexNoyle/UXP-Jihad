/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_COMPOSITOROGL_H
#define MOZILLA_GFX_COMPOSITOROGL_H

#include "ContextStateTracker.h"
#include "gfx2DGlue.h"
#include "GLContextTypes.h"             // for GLContext, etc
#include "GLDefs.h"                     // for GLuint, LOCAL_GL_TEXTURE_2D, etc
#include "OGLShaderProgram.h"           // for ShaderProgramOGL, etc
#include "Units.h"                      // for ScreenPoint
#include "mozilla/Assertions.h"         // for MOZ_ASSERT, etc
#include "mozilla/Attributes.h"
#include "mozilla/Monitor.h"          // Jihad T-502
#include "mozilla/Atomics.h"         // for override, final
#include "mozilla/RefPtr.h"             // for already_AddRefed, RefPtr
#include "mozilla/gfx/2D.h"             // for DrawTarget
#include "mozilla/gfx/BaseSize.h"       // for BaseSize
#include "mozilla/gfx/MatrixFwd.h"      // for Matrix4x4
#include "mozilla/gfx/Point.h"          // for IntSize, Point
#include "mozilla/gfx/Rect.h"           // for Rect, IntRect
#include "mozilla/gfx/Triangle.h"       // for Triangle
#include "mozilla/gfx/Types.h"          // for Float, SurfaceFormat, etc
#include "mozilla/layers/Compositor.h"  // for SurfaceInitMode, Compositor, etc
#include "mozilla/layers/CompositorTypes.h"  // for MaskType::MaskType::NumMaskTypes, etc
#include "mozilla/layers/LayersTypes.h"
#include "nsCOMPtr.h"                   // for already_AddRefed
#include "nsDebug.h"                    // for NS_ASSERTION, NS_WARNING
#include "nsISupportsImpl.h"            // for MOZ_COUNT_CTOR, etc
#include "nsTArray.h"                   // for AutoTArray, nsTArray, etc
#include "nsThreadUtils.h"              // for nsRunnable
#include "nsXULAppAPI.h"                // for XRE_GetProcessType
#include "nscore.h"                     // for NS_IMETHOD

class nsIWidget;

namespace base { class Thread; }   // Jihad T-502
namespace mozilla {

namespace layers {

class CompositingRenderTarget;
class CompositingRenderTargetOGL;
class DataTextureSource;
class GLManagerCompositor;
class TextureSource;
struct Effect;
struct EffectChain;
class GLBlitTextureImageHelper;

/**
 * Interface for pools of temporary gl textures for the compositor.
 * The textures are fully owned by the pool, so the latter is responsible
 * calling fDeleteTextures accordingly.
 * Users of GetTexture receive a texture that is only valid for the duration
 * of the current frame.
 * This is primarily intended for direct texturing APIs that need to attach
 * shared objects (such as an EGLImage) to a gl texture.
 */
class CompositorTexturePoolOGL
{
protected:
  virtual ~CompositorTexturePoolOGL() {}

public:
  NS_INLINE_DECL_REFCOUNTING(CompositorTexturePoolOGL)

  virtual void Clear() = 0;

  virtual GLuint GetTexture(GLenum aTarget, GLenum aEnum) = 0;

  virtual void EndFrame() = 0;
};

/**
 * Agressively reuses textures. One gl texture per texture unit in total.
 * So far this hasn't shown the best results on b2g.
 */
class PerUnitTexturePoolOGL : public CompositorTexturePoolOGL
{
public:
  explicit PerUnitTexturePoolOGL(gl::GLContext* aGL)
  : mTextureTarget(0) // zero is never a valid texture target
  , mGL(aGL)
  {}

  virtual ~PerUnitTexturePoolOGL()
  {
    DestroyTextures();
  }

  virtual void Clear() override
  {
    DestroyTextures();
  }

  virtual GLuint GetTexture(GLenum aTarget, GLenum aUnit) override;

  virtual void EndFrame() override {}

protected:
  void DestroyTextures();

  GLenum mTextureTarget;
  nsTArray<GLuint> mTextures;
  RefPtr<gl::GLContext> mGL;
};

/**
 * Reuse gl textures from a pool of textures that haven't yet been
 * used during the current frame.
 * All the textures that are not used at the end of a frame are
 * deleted.
 * This strategy seems to work well with gralloc textures because destroying
 * unused textures which are bound to gralloc buffers let drivers know that it
 * can unlock the gralloc buffers.
 */
class PerFrameTexturePoolOGL : public CompositorTexturePoolOGL
{
public:
  explicit PerFrameTexturePoolOGL(gl::GLContext* aGL)
  : mTextureTarget(0) // zero is never a valid texture target
  , mGL(aGL)
  {}

  virtual ~PerFrameTexturePoolOGL()
  {
    DestroyTextures();
  }

  virtual void Clear() override
  {
    DestroyTextures();
  }

  virtual GLuint GetTexture(GLenum aTarget, GLenum aUnit) override;

  virtual void EndFrame() override;

protected:
  void DestroyTextures();

  GLenum mTextureTarget;
  RefPtr<gl::GLContext> mGL;
  nsTArray<GLuint> mCreatedTextures;
  nsTArray<GLuint> mUnusedTextures;
};

// If you want to make this class not final, first remove calls to virtual
// methods (Destroy) that are made in the destructor.
// Jihad T-502: one frame's readback, owned by the caller until done() has run.
struct JihadFrameRequest {
  uint8_t* dest = nullptr;
  int stride = 0, w = 0, h = 0;
  int lo = 0, hi = 0;                  // the rows to read, top-down ((0,0) = none asked for)
  int lo2 = 0, hi2 = 0;                // a second band to read ((0,0) = none)
  const uint8_t* src = nullptr;        // round 113: the kept rows come from this frame (the newest slot), not dest
  int srcY = 0;                        // round 115: src's position (card px), when the engine decides the shift
  bool engineShift = false;            //   the shift the diff saw (a scroll the page made) replaces the daemon's
  bool verify = false;                 // T-503: a verify composite (JIHAD_GPU_VERIFY): no state the next frame uses changes
  int frameY = 0; bool frameYValid = false;   // out: this frame's position (card px), when the engine decided it
  int shift = 0;                       // rows the destination is shifted by (memmove) before the read: the
                                       // card's scroll since the slot's frame, positive = scrolled down
  bool windowMoved = false;            // T-517 (flag 8): the daemon moved the frame's window by the shift
                                       //   instead: the rows are in place, only the kept rects are copied
  bool bandsOnly = false;              // T-516 (flag 16): read the bands asked for, not the rows the tree changed
  bool overview = false;               // T-504 (flag 32): a window of the daemon's overview: the scrolled content
                                       //   alone (fixed and stuck layers not drawn), cleared transparent so the
                                       //   rows no layer drew read alpha 0; read with its alpha kept
  bool overviewParts = false;          // T-504 (flag 64): the fixed and stuck layers alone, cleared transparent,
                                       //   read with their alpha (drawn over the daemon's overview)
  bool contentOnly = false;            // T-505 (flag 128): the frame without its parts: every fixed and every sticky
                                       //   layer at opacity 0 (stuck or not), no fixed rows, keeps or columns; the
                                       //   parts' rects still reported (the daemon draws them on the CPU). With flag
                                       //   64, the parts alone with the same rule
  uint32_t partsGen = 0;               // T-505, out: the count of updates to layers under a fixed or sticky one
  int fixedLo = 0, fixedHi = 0;        // out: the rows fixed and sticky layers cover (0,0 = none)
  int cols[4][4] = {};                 // round 138: column reads after the bands (x0, x1, y0, y1 on the
  int nCols = 0;                       //   frame): the fixed and stuck layers' rects, widened by the shift
  int keep[4][4] = {};                 // round 141: rects (x0, x1, y0, y1) the shift leaves where they are:
  int nKeep = 0;                       //   opaque stuck layers that held still, copied unshifted from the base
  int stuck[4][4] = {};                // T-544, out: the fixed and stuck layers' rects (x0, y0, x1, y1) on the
  int nStuck = 0;                      //   frame as composed: the card holds them still while it moves the frame
  bool scrollOnly = false;             // out: the composite's only change was the card's scroll
  int target = 0;                      // which window target holds the frame (the compositor fills it)
  int invLo = 0, invHi = 0;            // the rows the composite changed (the compositor fills them)
  // T-511, the column read: the columns the daemon's bands are stale in ((0,0) = none asked for,
  // (0,w) = whole rows), given when colsKnown. The bands are then read as the one column they and
  // the change span when it is narrow (readX0..readX1; (0,0) = whole rows), and the change's own
  // columns go back to the daemon (invX0..invX1, with invLo..invHi)
  bool colsKnown = false;
  int colX0 = 0, colX1 = 0;
  int readX0 = 0, readX1 = 0;
  int invX0 = 0, invX1 = 0;
  float scrollX = 0.0f, scrollY = 0.0f;   // the card's scroll (CSS px) for the compositor to apply
  bool scrollValid = false;
  void* fence = nullptr;               // the EGL fence after the compose, or null (glFinish was used)
  mozilla::Atomic<bool> composed{false};   // the compose is issued; the read is pending
  // T-509: the daemon gave the frame up (a bound passed, the GPU path is off): nothing is written
  // into dest from then on (a slot the software path may be painting)
  mozilla::Atomic<bool> cancelled{false};
  bool ok = false;
  void (*done)(JihadFrameRequest*) = nullptr;
  void* cookie = nullptr;
};

class CompositorOGL final : public Compositor
{
  typedef mozilla::gl::GLContext GLContext;

  friend class GLManagerCompositor;
  friend class CompositingRenderTargetOGL;

  std::map<ShaderConfigOGL, ShaderProgramOGL*> mPrograms;
public:
  explicit CompositorOGL(CompositorBridgeParent* aParent,
                         widget::CompositorWidget* aWidget,
                         int aSurfaceWidth = -1, int aSurfaceHeight = -1,
                         bool aUseExternalSurfaceSize = false);

protected:
  virtual ~CompositorOGL();

public:
  virtual CompositorOGL* AsCompositorOGL() override { return this; }

  // Jihad T-502: the window render target is a renderbuffer-backed FBO on the pbuffer context
  // (a glReadPixels from it is twice as fast as from the pbuffer, and thirty times faster than
  // from a texture-backed one on the Adreno 220); JihadReadFrame reads the frame's top-left
  // aWidth x aHeight into aDest as B8G8R8A8 (RGBA read, swizzled in place).
  GLuint JihadWindowFBO() const { return mJihadWindowFBO; }
  // T-504: the window frame cleared transparent instead of white (an overview window: rows no layer drew
  // read alpha 0), for the composite between the calls
  void JihadSetClearTransparent(bool aOn) { mJihadClearTransparent = aOn; }
  bool JihadIsWindowTarget() const;
  // rows [aRowLo, aRowHi) of the frame, top-down; (0, 0) = every row
  bool JihadReadFrame(uint8_t* aDest, int aStride, int aWidth, int aHeight, int aRowLo = 0, int aRowHi = 0);
  // round 138: columns aX0..aX1 of the target's rows aSrcRowLo.. (aRows of them, top-down, on a
  // target aSurfaceH rows tall) into the frame's rows aDestRowLo.. at the same columns
  bool JihadReadColumns(uint8_t* aDest, int aStride, int aSurfaceH, int aX0, int aX1, int aSrcRowLo, int aRows, int aDestRowLo,
                        int aDestX = -1);   // round 142: the frame's column when packed elsewhere on the target
  // The asynchronous frame: SelectTarget before the compose picks the free window target,
  // ReadAsync after it fences the compose and hands the target to the read thread, which reads
  // rows [lo, hi) into dest and calls done (on the read thread). When no read thread could be
  // made the read happens here, synchronously, before ReadAsync returns.
  void JihadSelectTarget();
  // T-502 round 101: the window targets (and the band target's width) sized to the frame the
  // daemon asks for, re-specified when it changes. Composing a 768x942 frame into the corner of
  // the 1100x1400 pbuffer-sized renderbuffers left frame rows 484..941 (the bottom 458 GL rows;
  // 1400 - 942 = 458) white in about one composite in five on the Adreno 220, every quad drawn,
  // glFinish no help (2026-10-18). JIHAD_GPU_PB_TARGETS=1 keeps the pbuffer size for A/B.
  void JihadEnsureTargetSize(int aWidth, int aHeight);
  // T-502 round 105: a scroll-only frame composes its bands stacked at the top of the frame-sized
  // window target (full render bounds, a partial clip per band) and reads them back in one call.
  // A separate band-sized FBO broke on the Adreno 220: the driver bins an FBO render by the EGL
  // surface's size, so any target or viewport smaller than the surface lost rows (round 104).
  // 0 rows when off (JIHAD_GPU_NO_BAND_TARGET=1).
  int JihadBandMaxRows() const { static const bool sOff = getenv("JIHAD_GPU_NO_BAND_TARGET") != nullptr; const int m = JihadBandRows() > JihadBandTallRows() ? JihadBandRows() : JihadBandTallRows(); return sOff ? 0 : (m > 320 ? m : 320); }
  // Round 106: the band pass on its own surface (JIHAD_GPU_BAND_SURFACE=0 keeps the stack in the
  // frame-sized target): a 256-row pbuffer and an FBO of the same size are made current, so the
  // composite costs the band, not the frame; JihadEndBandPass puts the frame's surface and
  // target back. Returns the band target's rows, 0 when the band pass is not available.
  // T-527: aRows the pass needs: the band surface when they fit it, else the tall one when they fit that
  int JihadBeginBandPass(int aWidth, int aRows);
  void JihadEndBandPass();
  void JihadUseFrameSurface();   // the frame's pbuffer current again (before a whole composite)
  // T-513 (review): the reads that follow belong to a request the daemon may give up while the GPU
  // is read (a stall shows up inside glReadPixels); null when they are done
  void JihadSetCancel(const mozilla::Atomic<bool>* aCancel) { mJihadCancel = aCancel; }
  GLuint mJihadBandRB = 0, mJihadBandFBO = 0;
  int mJihadBandW = 0;
  GLuint mJihadBandTallRB = 0, mJihadBandTallFBO = 0;   // T-527: the tall band surface's target
  int mJihadBandTallW = 0;
  int mJihadBandFailW[2] = { 0, 0 };   // T-527: the width a band surface (0) or the tall one (1) failed at...
  TimeStamp mJihadBandFailT[2];        // ...and when: tried again 30 s later (a failure may be another app's GPU memory)
  // the band surface's rows: JIHAD_GPU_BAND_ROWS (64..942, default 256; round 137)
  static int JihadBandRows() {
    static const int sRows = [] {
      const char* e = getenv("JIHAD_GPU_BAND_ROWS");
      const int r = e ? atoi(e) : 256;
      return r < 64 ? 64 : (r > 942 ? 942 : r);
    }();
    return sRows;
  }
  // T-527: a second, taller band surface for a frame whose change does not fit the band surface (a
  // 480 px WebGL canvas changes 354-482 rows a frame and took the whole frame-sized target, 768x1224
  // cleared, drawn and resolved every frame): JIHAD_GPU_BAND_TALL (default 512, 0 off). Frames are
  // packed for the band surface first, so a frame that fits it never pays the tall one.
  static int JihadBandTallRows() {
    static const int sRows = [] {
      const char* e = getenv("JIHAD_GPU_BAND_TALL");
      const int r = e ? atoi(e) : 512;
      return r <= JihadBandRows() ? 0 : (r > 942 ? 942 : r);
    }();
    return sRows;
  }
  void JihadShiftRowsNow(JihadFrameRequest* aReq);
  uint8_t* JihadBandScratch(size_t aBytes);   // the stacked bands' read, at least aBytes
  bool JihadReadAsync(JihadFrameRequest* aReq);
  void JihadReadOnReadThread(JihadFrameRequest* aReq);   // the read thread
  void JihadReadThreadInit();                            // the read thread, once

  virtual already_AddRefed<DataTextureSource>
  CreateDataTextureSource(TextureFlags aFlags = TextureFlags::NO_FLAGS) override;

  virtual bool Initialize(nsCString* const out_failureReason) override;

  virtual void Destroy() override;

  virtual TextureFactoryIdentifier GetTextureFactoryIdentifier() override
  {
    TextureFactoryIdentifier result =
      TextureFactoryIdentifier(LayersBackend::LAYERS_OPENGL,
                               XRE_GetProcessType(),
                               GetMaxTextureSize(),
                               mFBOTextureTarget == LOCAL_GL_TEXTURE_2D,
                               SupportsPartialTextureUpdate());
    return result;
  }

  virtual already_AddRefed<CompositingRenderTarget>
  CreateRenderTarget(const gfx::IntRect &aRect, SurfaceInitMode aInit) override;

  virtual already_AddRefed<CompositingRenderTarget>
  CreateRenderTargetFromSource(const gfx::IntRect &aRect,
                               const CompositingRenderTarget *aSource,
                               const gfx::IntPoint &aSourcePoint) override;

  virtual void SetRenderTarget(CompositingRenderTarget *aSurface) override;
  virtual CompositingRenderTarget* GetCurrentRenderTarget() const override;

  virtual void DrawQuad(const gfx::Rect& aRect,
                        const gfx::IntRect& aClipRect,
                        const EffectChain &aEffectChain,
                        gfx::Float aOpacity,
                        const gfx::Matrix4x4& aTransform,
                        const gfx::Rect& aVisibleRect) override;

  virtual void DrawTriangle(const gfx::TexturedTriangle& aTriangle,
                            const gfx::IntRect& aClipRect,
                            const EffectChain& aEffectChain,
                            gfx::Float aOpacity,
                            const gfx::Matrix4x4& aTransform,
                            const gfx::Rect& aVisibleRect) override;

  virtual void EndFrame() override;
  virtual void EndFrameForExternalComposition(const gfx::Matrix& aTransform) override;

  virtual bool SupportsPartialTextureUpdate() override;

  virtual bool CanUseCanvasLayerForSize(const gfx::IntSize &aSize) override
  {
    if (!mGLContext)
      return false;
    int32_t maxSize = GetMaxTextureSize();
    return aSize <= gfx::IntSize(maxSize, maxSize);
  }

  virtual int32_t GetMaxTextureSize() const override;

  /**
   * Set the size of the EGL surface we're rendering to, if we're rendering to
   * an EGL surface.
   */
  virtual void SetDestinationSurfaceSize(const gfx::IntSize& aSize) override;

  virtual void SetScreenRenderOffset(const ScreenPoint& aOffset) override {
    mRenderOffset = aOffset;
  }

  virtual void MakeCurrent(MakeCurrentFlags aFlags = 0) override;

#ifdef MOZ_DUMP_PAINTING
  virtual const char* Name() const override { return "OGL"; }
#endif // MOZ_DUMP_PAINTING

  virtual LayersBackend GetBackendType() const override {
    return LayersBackend::LAYERS_OPENGL;
  }

  virtual void Pause() override;
  virtual bool Resume() override;

  virtual bool HasImageHostOverlays() override
  {
    return false;
  }

  virtual void AddImageHostOverlay(ImageHostOverlay* aOverlay) override
  {
  }

  virtual void RemoveImageHostOverlay(ImageHostOverlay* aOverlay) override
  {
  }

  GLContext* gl() const { return mGLContext; }
  /**
   * Clear the program state. This must be called
   * before operating on the GLContext directly. */
  void ResetProgram();

  gfx::SurfaceFormat GetFBOFormat() const {
    return gfx::SurfaceFormat::R8G8B8A8;
  }

  GLBlitTextureImageHelper* BlitTextureImageHelper();

  /**
   * The compositor provides with temporary textures for use with direct
   * textruing like gralloc texture.
   * Doing so lets us use gralloc the way it has been designed to be used
   * (see https://wiki.mozilla.org/Platform/GFX/Gralloc)
   */
  GLuint GetTemporaryTexture(GLenum aTarget, GLenum aUnit);

  const gfx::Matrix4x4& GetProjMatrix() const {
    return mProjMatrix;
  }

  void SetProjMatrix(const gfx::Matrix4x4& aProjMatrix) {
    mProjMatrix = aProjMatrix;
  }

  const gfx::IntSize GetDestinationSurfaceSize() const {
    return gfx::IntSize (mSurfaceSize.width, mSurfaceSize.height);
  }

  const ScreenPoint& GetScreenRenderOffset() const {
    return mRenderOffset;
  }

private:
  template<typename Geometry>
  void DrawGeometry(const Geometry& aGeometry,
                    const gfx::IntRect& aClipRect,
                    const EffectChain &aEffectChain,
                    gfx::Float aOpacity,
                    const gfx::Matrix4x4& aTransform,
                    const gfx::Rect& aVisibleRect);

  void PrepareViewport(CompositingRenderTargetOGL *aRenderTarget);

  /** Widget associated with this compositor */
  LayoutDeviceIntSize mWidgetSize;
  GLuint mJihadWindowFBO = 0;      // Jihad T-502: the target the frame draws into (one of the two below, or the texture's)
  bool mJihadClearTransparent = false;   // T-504: JihadSetClearTransparent
  bool mJihadKeepAlpha = false;          // T-504: JihadReadFrame keeps the alpha byte (an overview's read)
  GLuint mJihadWindowTex = 0;      // the texture window target the swizzle pass samples (JIHAD_GPU_PASS)
  gfx::IntSize mJihadPbSize;
  // The default path: two renderbuffer window targets, used in turn, read back by a thread of
  // their own through a context that shares them, so the compositor thread is never held by a
  // glReadPixels (20-45 ms on the Adreno 220) and a frame's read overlaps the next compose.
  GLuint mJihadTargetRB[2] = {0, 0}, mJihadTargetFBO[2] = {0, 0};
  // T-502 diagnostic (JIHAD_GPU_DRAWSTATS): per composite, the draws on the window target
  int mJihadDraws = 0, mJihadClipBottomMax = 0, mJihadQuadBottomMax = 0, mJihadClipBottomMin = 1 << 30;
  int mJihadSurfaceDraws = 0;
  struct JihadDrawRec { float x, y, w, h; int cy, cb; int type; float r, g, b, a, opacity; };
  JihadDrawRec mJihadDrawRecs[48];
  GLint mJihadRawSc[4], mJihadCacheSc[4], mJihadRawVp[4], mJihadCacheVp[4];   // at the first window draw
  bool mJihadProbed = false;
  gfx::IntSize mJihadTargetSize;                     // the window targets' storage size (round 101)
  UniquePtr<uint8_t[]> mJihadBandScratch;              // one read of the stacked bands, split from here
  size_t mJihadBandScratchSize = 0;                    // its size (T-513: per compositor)
  const mozilla::Atomic<bool>* mJihadCancel = nullptr; // T-513: the frame being read was given up (JihadSetCancel)
  int mJihadCur = 0;                                 // the target the next frame draws into
  mozilla::Atomic<int32_t> mJihadTargetBusy[2];      // handed to the read thread, not yet read
  mozilla::Monitor mJihadReadMon{"jihad-read"};
  base::Thread* mJihadReadThread = nullptr;
  bool mJihadReadInitDone = false, mJihadReadInitOk = false, mJihadReadInitAnswered = false;
  RefPtr<GLContext> mJihadReadCtx;                   // made on the read thread, current there only
  GLuint mJihadReadTargetFBO[2] = {0, 0};            // the read context's FBOs on the shared renderbuffers
  bool JihadEnsureReadThread();
  void JihadStopReadThread();
  // The swizzle pass: a frame-sized renderbuffer FBO the window texture is drawn into through a
  // shader that swaps red and blue, forces alpha and flips the rows, so glReadPixels lands the
  // adapter's BGRA top-down frame straight in the shared buffer (no CPU pass).
  GLuint mJihadReadFBO = 0, mJihadReadRB = 0;
  gfx::IntSize mJihadReadSize;
  GLuint mJihadSwizzleProg = 0;
  GLint mJihadSwizzlePos = -1, mJihadSwizzleTex = -1, mJihadSwizzleScale = -1;
  bool mJihadSwizzleFailed = false;
  bool JihadEnsureSwizzlePass(int aW, int aH);
  RefPtr<GLContext> mGLContext;
  UniquePtr<GLBlitTextureImageHelper> mBlitTextureImageHelper;
  gfx::Matrix4x4 mProjMatrix;

  /** The size of the surface we are rendering to */
  gfx::IntSize mSurfaceSize;

  ScreenPoint mRenderOffset;

  already_AddRefed<mozilla::gl::GLContext> CreateContext();

  /** Texture target to use for FBOs */
  GLenum mFBOTextureTarget;

  /** Currently bound render target */
  RefPtr<CompositingRenderTargetOGL> mCurrentRenderTarget;
#ifdef DEBUG
  CompositingRenderTargetOGL* mWindowRenderTarget;
#endif

  /**
   * VBO that has some basics in it for a textured quad, including vertex
   * coords and texcoords.
   */
  GLuint mQuadVBO;


  /**
  * VBO that stores dynamic triangle geometry.
  */
  GLuint mTriangleVBO;

  bool mHasBGRA;

  /**
   * When rendering to some EGL surfaces (e.g. on Android), we rely on being told
   * about size changes (via SetSurfaceSize) rather than pulling this information
   * from the widget.
   */
  bool mUseExternalSurfaceSize;

  /**
   * Have we had DrawQuad calls since the last frame was rendered?
   */
  bool mFrameInProgress;

  /*
   * Clear aRect on current render target.
   */
  virtual void ClearRect(const gfx::Rect& aRect) override;

  /* Start a new frame. If aClipRectIn is null and aClipRectOut is non-null,
   * sets *aClipRectOut to the screen dimensions.
   */
  virtual void BeginFrame(const nsIntRegion& aInvalidRegion,
                          const gfx::IntRect *aClipRectIn,
                          const gfx::IntRect& aRenderBounds,
                          const nsIntRegion& aOpaqueRegion,
                          gfx::IntRect *aClipRectOut = nullptr,
                          gfx::IntRect *aRenderBoundsOut = nullptr) override;

  ShaderConfigOGL GetShaderConfigFor(Effect *aEffect,
                                     MaskType aMask = MaskType::MaskNone,
                                     gfx::CompositionOp aOp = gfx::CompositionOp::OP_OVER,
                                     bool aColorMatrix = false,
                                     bool aDEAAEnabled = false) const;

  ShaderProgramOGL* GetShaderProgramFor(const ShaderConfigOGL &aConfig);

  void ApplyPrimitiveConfig(ShaderConfigOGL& aConfig,
                            const gfx::Rect&)
  {
    aConfig.SetDynamicGeometry(false);
  }

  void ApplyPrimitiveConfig(ShaderConfigOGL& aConfig,
                            const gfx::TexturedTriangle&)
  {
    aConfig.SetDynamicGeometry(true);
  }

  /**
   * Create a FBO backed by a texture.
   * Note that the texture target type will be
   * of the type returned by FBOTextureTarget; different
   * shaders are required to sample from the different
   * texture types.
   */
  void CreateFBOWithTexture(const gfx::IntRect& aRect, bool aCopyFromSource,
                            GLuint aSourceFrameBuffer,
                            GLuint *aFBO, GLuint *aTexture,
                            gfx::IntSize* aAllocSize = nullptr);

  GLuint CreateTexture(const gfx::IntRect& aRect, bool aCopyFromSource,
                       GLuint aSourceFrameBuffer,
                       gfx::IntSize* aAllocSize = nullptr);

  gfx::Point3D GetLineCoefficients(const gfx::Point& aPoint1,
                                   const gfx::Point& aPoint2);

  void ActivateProgram(ShaderProgramOGL *aProg);

  void CleanupResources();

  void BindAndDrawQuads(ShaderProgramOGL *aProg,
                        int aQuads,
                        const gfx::Rect* aLayerRect,
                        const gfx::Rect* aTextureRect);

  void BindAndDrawQuad(ShaderProgramOGL *aProg,
                       const gfx::Rect& aLayerRect,
                       const gfx::Rect& aTextureRect =
                         gfx::Rect(0.0f, 0.0f, 1.0f, 1.0f))
  {
    gfx::Rect layerRects[4];
    gfx::Rect textureRects[4];
    layerRects[0] = aLayerRect;
    textureRects[0] = aTextureRect;
    BindAndDrawQuads(aProg, 1, layerRects, textureRects);
  }

  void BindAndDrawGeometry(ShaderProgramOGL* aProgram,
                           const gfx::Rect& aRect,
                           const gfx::Rect& aTextureRect =
                             gfx::Rect(0.0f, 0.0f, 1.0f, 1.0f));

  void BindAndDrawGeometry(ShaderProgramOGL* aProgram,
                           const gfx::TexturedTriangle& aTriangle,
                           const gfx::Rect& aTextureRect =
                             gfx::Rect(0.0f, 0.0f, 1.0f, 1.0f));

  void BindAndDrawGeometryWithTextureRect(ShaderProgramOGL *aProg,
                                          const gfx::Rect& aRect,
                                          const gfx::Rect& aTexCoordRect,
                                          TextureSource *aTexture);

  void BindAndDrawGeometryWithTextureRect(ShaderProgramOGL *aProg,
                                         const gfx::TexturedTriangle& aTriangle,
                                         const gfx::Rect& aTexCoordRect,
                                         TextureSource *aTexture);

  void InitializeVAO(const GLuint aAttribIndex, const GLint aComponents,
                     const GLsizei aStride, const size_t aOffset);

  gfx::Rect GetTextureCoordinates(gfx::Rect textureRect,
                                  TextureSource* aTexture);

  /**
   * Bind the texture behind the current render target as the backdrop for a
   * mix-blend shader.
   */
  void BindBackdrop(ShaderProgramOGL* aProgram, GLuint aBackdrop, GLenum aTexUnit);

  /**
   * Copies the content of our backbuffer to the set transaction target.
   * Does not restore the target FBO, so only call from EndFrame.
   */
  void CopyToTarget(gfx::DrawTarget* aTarget, const nsIntPoint& aTopLeft, const gfx::Matrix& aWorldMatrix);

  /**
   * Implements the flipping of the y-axis to convert from layers/compositor
   * coordinates to OpenGL coordinates.
   *
   * Indeed, the only coordinate system that OpenGL knows has the y-axis
   * pointing upwards, but the layers/compositor coordinate system has the
   * y-axis pointing downwards, for good reason as Web pages are typically
   * scrolled downwards. So, some flipping has to take place; FlippedY does it.
   */
  GLint FlipY(GLint y) const { return mViewportSize.height - y; }

  RefPtr<CompositorTexturePoolOGL> mTexturePool;

  ContextStateTrackerOGL mContextStateTracker;

  bool mDestroyed;

  /**
   * Size of the OpenGL context's primary framebuffer in pixels. Used by
   * FlipY for the y-flipping calculation and by the DEAA shader.
   */
  gfx::IntSize mViewportSize;

  ShaderProgramOGL *mCurrentProgram;
};

} // namespace layers
} // namespace mozilla

#endif /* MOZILLA_GFX_COMPOSITOROGL_H */
