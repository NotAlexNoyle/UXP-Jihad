/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsPrintfCString.h"   // Jihad T-502 diagnostic
#include "CompositorOGL.h"
#include "mozilla/TimeStamp.h"   // Jihad T-502 timing
#include <stddef.h>                     // for size_t
#include <stdint.h>                     // for uint32_t, uint8_t
#include <stdlib.h>                     // for free, malloc
#include "GLContextProvider.h"          // for GLContextProvider
#include "GLContext.h"                  // for GLContext
#include "GLContextEGL.h"               // Jihad T-502: the pbuffer context
#include "base/task.h"                  // Jihad T-502: the read thread
#include "base/thread.h"
#include <time.h>                       // Jihad T-502: CLOCK_THREAD_CPUTIME_ID
#include <algorithm>
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>                  // Jihad T-502: the readback swizzle
#endif
#include "GLUploadHelpers.h"
#include "Layers.h"                     // for WriteSnapshotToDumpFile
#include "LayerScope.h"                 // for LayerScope
#include "gfxEnv.h"                     // for gfxEnv
#include "gfxPlatform.h"                // for gfxPlatform
#include "gfxPrefs.h"                   // for gfxPrefs
#include "gfxRect.h"                    // for gfxRect
#include "gfxUtils.h"                   // for gfxUtils, etc
#include "mozilla/ArrayUtils.h"         // for ArrayLength
#include "mozilla/Preferences.h"        // for Preferences
#include "mozilla/gfx/BasePoint.h"      // for BasePoint
#include "mozilla/gfx/Matrix.h"         // for Matrix4x4, Matrix
#include "mozilla/gfx/Triangle.h"       // for Triangle
#include "mozilla/gfx/gfxVars.h"        // for gfxVars
#include "mozilla/layers/LayerManagerComposite.h"  // for LayerComposite, etc
#include "mozilla/layers/CompositingRenderTargetOGL.h"
#include "mozilla/layers/Effects.h"     // for EffectChain, TexturedEffect, etc
#include "mozilla/layers/TextureHost.h"  // for TextureSource, etc
#include "mozilla/layers/TextureHostOGL.h"  // for TextureSourceOGL, etc
#include "mozilla/mozalloc.h"           // for operator delete, etc
#include "nsAppRunner.h"
#include "nsAString.h"
#include "nsIConsoleService.h"          // for nsIConsoleService, etc
#include "nsIWidget.h"                  // for nsIWidget
#include "nsLiteralString.h"            // for NS_LITERAL_STRING
#include "nsMathUtils.h"                // for NS_roundf
#include "nsRect.h"                     // for mozilla::gfx::IntRect
#include "nsServiceManagerUtils.h"      // for do_GetService
#include "nsString.h"                   // for nsString, nsAutoCString, etc
#include "ScopedGLHelpers.h"
#include "GLReadTexImageHelper.h"
#include "GLBlitTextureImageHelper.h"
#include "HeapCopyOfStackArray.h"

#include "GeckoProfiler.h"

#include "mozilla/gfx/JihadGpuLog.h"   // Jihad (T-512): JIHAD_GPU_LOG

namespace mozilla {

using namespace std;
using namespace gfx;

namespace layers {

extern double gJihadPhase[12];   // LayerManagerComposite.cpp (T-502 round 139, JIHAD_GPU_PHASES=1)

using namespace mozilla::gl;

static const GLuint kCoordinateAttributeIndex = 0;
static const GLuint kTexCoordinateAttributeIndex = 1;

static void
BindMaskForProgram(ShaderProgramOGL* aProgram, TextureSourceOGL* aSourceMask,
                   GLenum aTexUnit, const gfx::Matrix4x4& aTransform)
{
  MOZ_ASSERT(LOCAL_GL_TEXTURE0 <= aTexUnit && aTexUnit <= LOCAL_GL_TEXTURE31);
  aSourceMask->BindTexture(aTexUnit, gfx::SamplingFilter::LINEAR);
  aProgram->SetMaskTextureUnit(aTexUnit - LOCAL_GL_TEXTURE0);
  aProgram->SetMaskLayerTransform(aTransform);
}

void
CompositorOGL::BindBackdrop(ShaderProgramOGL* aProgram, GLuint aBackdrop, GLenum aTexUnit)
{
  MOZ_ASSERT(aBackdrop);

  mGLContext->fActiveTexture(aTexUnit);
  mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, aBackdrop);
  mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_LINEAR);
  mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_LINEAR);
  aProgram->SetBackdropTextureUnit(aTexUnit - LOCAL_GL_TEXTURE0);
}

CompositorOGL::CompositorOGL(CompositorBridgeParent* aParent,
                             widget::CompositorWidget* aWidget,
                             int aSurfaceWidth, int aSurfaceHeight,
                             bool aUseExternalSurfaceSize)
  : Compositor(aWidget, aParent)
  , mWidgetSize(-1, -1)
  , mSurfaceSize(aSurfaceWidth, aSurfaceHeight)
  , mHasBGRA(0)
  , mUseExternalSurfaceSize(aUseExternalSurfaceSize)
  , mFrameInProgress(false)
  , mDestroyed(false)
  , mViewportSize(0, 0)
  , mCurrentProgram(nullptr)
{
  MOZ_COUNT_CTOR(CompositorOGL);
}

CompositorOGL::~CompositorOGL()
{
  MOZ_COUNT_DTOR(CompositorOGL);
  Destroy();
}

already_AddRefed<mozilla::gl::GLContext>
CompositorOGL::CreateContext()
{
  RefPtr<GLContext> context;

  // T-509 device test (JIHAD_GPU_TEST_NO_EGL=1): no GL context for the compositor, as when EGL
  // fails; the daemon must find the GPU path unusable and render in software
  if (getenv("JIHAD_GPU_TEST_NO_EGL")) {
    fprintf(stderr, "[jihad-gpu] JIHAD_GPU_TEST_NO_EGL: the compositor gets no GL context\n");
    return nullptr;
  }

  // Used by mock widget to create an offscreen context
  nsIWidget* widget = mWidget->RealWidget();
  void* widgetOpenGLContext = widget ? widget->GetNativeData(NS_NATIVE_OPENGL_CONTEXT) : nullptr;
  if (widgetOpenGLContext) {
    GLContext* alreadyRefed = reinterpret_cast<GLContext*>(widgetOpenGLContext);
    return already_AddRefed<GLContext>(alreadyRefed);
  }

#ifdef XP_WIN
  if (gfxEnv::LayersPreferEGL()) {
    printf_stderr("Trying GL layers...\n");
    context = gl::GLContextProviderEGL::CreateForCompositorWidget(mWidget, false);
  }
#endif

  // Jihad T-502: on the Adreno 220 a glReadPixels from a texture-backed FBO (what the offscreen
  // GLScreenBuffer is) costs 370-640 ms a frame; from the pbuffer surface 12-43 ms
  // (tools/device/gpu-bench.c, 2026-10-17). So with JIHAD_GPU_COMPOSITOR the compositor draws
  // into a plain pbuffer context with NO screen buffer: framebuffer 0 is the pbuffer itself. A
  // pbuffer cannot be resized, so it is made large enough for the layout viewport T-499 may ask
  // for and each frame uses its bottom-left part.
  if (!context && getenv("JIHAD_GPU_COMPOSITOR")) {
    nsCString failureId;
    gfx::IntSize pb(std::max(mSurfaceSize.width, 1100), std::max(mSurfaceSize.height, 1400));
    context = gl::GLContextEGL::CreateEGLPBufferOffscreenContext(
        CreateContextFlags::REQUIRE_COMPAT_PROFILE, pb, SurfaceCaps::ForRGBA(), &failureId);
    mJihadPbSize = pb;
    fprintf(stderr, "[jihad-gpu] pbuffer context %dx%d: %s %s\n", pb.width, pb.height,
            context ? "created" : "FAILED", failureId.get());
  }

  // Allow to create offscreen GL context for main Layer Manager
  if (!context && gfxEnv::LayersPreferOffscreen()) {
    SurfaceCaps caps = SurfaceCaps::ForRGB();
    caps.preserve = false;
    caps.bpp16 = gfxVars::OffscreenFormat() == SurfaceFormat::R5G6B5_UINT16;

    nsCString discardFailureId;
    context = GLContextProvider::CreateOffscreen(mSurfaceSize,
                                                 caps, CreateContextFlags::REQUIRE_COMPAT_PROFILE,
                                                 &discardFailureId);
  }

  if (!context) {
    context = gl::GLContextProvider::CreateForCompositorWidget(mWidget,
                gfxVars::RequiresAcceleratedGLContextForCompositorOGL());
  }

  if (!context) {
    NS_WARNING("Failed to create CompositorOGL context");
  }

  return context.forget();
}

void
CompositorOGL::Destroy()
{
  JihadStopReadThread();   // Jihad T-502: before the GL objects it reads go
  Compositor::Destroy();

  if (mTexturePool) {
    mTexturePool->Clear();
    mTexturePool = nullptr;
  }

  if (!mDestroyed) {
    mDestroyed = true;
    CleanupResources();
  }
}

void
CompositorOGL::CleanupResources()
{
  if (!mGLContext)
    return;

  RefPtr<GLContext> ctx = mGLContext->GetSharedContext();
  if (!ctx) {
    ctx = mGLContext;
  }

  if (!ctx->MakeCurrent()) {
    // Leak resources!
    gl::JihadDropPooledTextures(mGLContext, false);   // Jihad T-504: names of a context going away
    mQuadVBO = 0;
    mTriangleVBO = 0;
    mGLContext = nullptr;
    mPrograms.clear();
    return;
  }
  gl::JihadDropPooledTextures(mGLContext, true);   // Jihad T-504

  for (std::map<ShaderConfigOGL, ShaderProgramOGL *>::iterator iter = mPrograms.begin();
       iter != mPrograms.end();
       iter++) {
    delete iter->second;
  }
  mPrograms.clear();

  ctx->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);

  if (mQuadVBO) {
    ctx->fDeleteBuffers(1, &mQuadVBO);
    mQuadVBO = 0;
  }

  if (mTriangleVBO) {
    ctx->fDeleteBuffers(1, &mTriangleVBO);
    mTriangleVBO = 0;
  }

  mGLContext->MakeCurrent();

  mBlitTextureImageHelper = nullptr;

  mContextStateTracker.DestroyOGL(mGLContext);

  // On the main thread the Widget will be destroyed soon and calling MakeCurrent
  // after that could cause a crash (at least with GLX, see bug 1059793), unless
  // context is marked as destroyed.
  // There may be some textures still alive that will try to call MakeCurrent on
  // the context so let's make sure it is marked destroyed now.
  mGLContext->MarkDestroyed();

  mGLContext = nullptr;
}

bool
CompositorOGL::Initialize(nsCString* const out_failureReason)
{
  // Do not allow double initialization
  MOZ_ASSERT(mGLContext == nullptr, "Don't reinitialize CompositorOGL");

  mGLContext = CreateContext();

  if (!mGLContext){
    *out_failureReason = "FEATURE_FAILURE_OPENGL_CREATE_CONTEXT";
    return false;
  }

  // Jihad T-502: a renderbuffer-backed FBO as the window target of the pbuffer context.
  if (mJihadPbSize.width > 0 && mGLContext->IsOffscreen() && !mGLContext->Screen()) {
    // The window target(s). By default two RENDERBUFFERS used in turn, read back by the read
    // thread (JihadReadAsync); JIHAD_GPU_PASS=1 keeps the single texture target the swizzle
    // pass samples (measured 2026-10-17, run 37: the pass buys nothing, glReadPixels itself is
    // the 20-40 ms, the same from a frame-sized renderbuffer).
    static const bool sPass = getenv("JIHAD_GPU_PASS") != nullptr;
    mGLContext->MakeCurrent();
    // JIHAD_GPU_ONE_TARGET=1: a single renderbuffer target (round 103 A/B: one of the two went bad)
    static const bool sOneTarget = getenv("JIHAD_GPU_ONE_TARGET") != nullptr;
    const int n = (sPass || sOneTarget) ? 1 : 2;
    bool ok = true;
    for (int t = 0; t < n && ok; ++t) {
      GLuint rb = 0, tex = 0, fbo = 0;
      if (sPass) {
        mGLContext->fGenTextures(1, &tex);
        mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, tex);
        mGLContext->fTexImage2D(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_RGBA, mJihadPbSize.width, mJihadPbSize.height,
                                0, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, nullptr);
        mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_NEAREST);
        mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_NEAREST);
        mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_S, LOCAL_GL_CLAMP_TO_EDGE);
        mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_T, LOCAL_GL_CLAMP_TO_EDGE);
        mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, 0);
      } else {
        mGLContext->fGenRenderbuffers(1, &rb);
        mGLContext->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, rb);
        mGLContext->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */,
                                         mJihadPbSize.width, mJihadPbSize.height);
      }
      mGLContext->fGenFramebuffers(1, &fbo);
      mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, fbo);
      if (sPass) {
        mGLContext->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_TEXTURE_2D, tex, 0);
      } else {
        mGLContext->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_RENDERBUFFER, rb);
      }
      GLenum status = mGLContext->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
      if (status != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[jihad-gpu] window target %d: incomplete (0x%x), drawing into the pbuffer\n", t, status);
        mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);
        mGLContext->fDeleteFramebuffers(1, &fbo);
        if (rb) mGLContext->fDeleteRenderbuffers(1, &rb);
        if (tex) mGLContext->fDeleteTextures(1, &tex);
        ok = false;
        break;
      }
      // the whole target white once: its first contents are whatever the driver had
      mGLContext->fDisable(LOCAL_GL_SCISSOR_TEST);
      mGLContext->fViewport(0, 0, mJihadPbSize.width, mJihadPbSize.height);
      mGLContext->fClearColor(1.0f, 1.0f, 1.0f, 1.0f);
      mGLContext->fClear(LOCAL_GL_COLOR_BUFFER_BIT);
      mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);
      if (sPass) {
        mJihadWindowTex = tex;
      } else {
        mJihadTargetRB[t] = rb; mJihadTargetFBO[t] = fbo;
      }
      if (t == 0) mJihadWindowFBO = fbo;
    }
    if (!ok) {
      for (int t = 0; t < 2; ++t) {
        if (mJihadTargetFBO[t]) mGLContext->fDeleteFramebuffers(1, &mJihadTargetFBO[t]);
        if (mJihadTargetRB[t]) mGLContext->fDeleteRenderbuffers(1, &mJihadTargetRB[t]);
        mJihadTargetFBO[t] = mJihadTargetRB[t] = 0;
      }
      mJihadWindowFBO = mJihadWindowTex = 0;
    } else {
      mJihadTargetSize = mJihadPbSize;
      JIHAD_GPU_LOG("[jihad-gpu] %s window target%s %dx%d: FBO %u%s\n", sPass ? "texture" : "renderbuffer", n > 1 ? "s" : "",
              mJihadPbSize.width, mJihadPbSize.height, mJihadWindowFBO, n > 1 ? " and one more, read by a thread of their own" : "");
    }
  }
  MakeCurrent();

  mHasBGRA =
    mGLContext->IsExtensionSupported(gl::GLContext::EXT_texture_format_BGRA8888) ||
    mGLContext->IsExtensionSupported(gl::GLContext::EXT_bgra);

  mGLContext->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA,
                                 LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA);
  mGLContext->fEnable(LOCAL_GL_BLEND);

  // initialise a common shader to check that we can actually compile a shader
  RefPtr<EffectSolidColor> effect = new EffectSolidColor(Color(0, 0, 0, 0));
  ShaderConfigOGL config = GetShaderConfigFor(effect);
  if (!GetShaderProgramFor(config)) {
    *out_failureReason = "FEATURE_FAILURE_OPENGL_COMPILE_SHADER";
    return false;
  }

  if (mGLContext->WorkAroundDriverBugs()) {
    /**
    * We'll test the ability here to bind NPOT textures to a framebuffer, if
    * this fails we'll try ARB_texture_rectangle.
    */

    GLenum textureTargets[] = {
      LOCAL_GL_TEXTURE_2D,
      LOCAL_GL_NONE
    };

    if (!mGLContext->IsGLES()) {
      // No TEXTURE_RECTANGLE_ARB available on ES2
      textureTargets[1] = LOCAL_GL_TEXTURE_RECTANGLE_ARB;
    }

    mFBOTextureTarget = LOCAL_GL_NONE;

    GLuint testFBO = 0;
    mGLContext->fGenFramebuffers(1, &testFBO);
    GLuint testTexture = 0;

    for (uint32_t i = 0; i < ArrayLength(textureTargets); i++) {
      GLenum target = textureTargets[i];
      if (!target)
          continue;

      mGLContext->fGenTextures(1, &testTexture);
      mGLContext->fBindTexture(target, testTexture);
      mGLContext->fTexParameteri(target,
                                LOCAL_GL_TEXTURE_MIN_FILTER,
                                LOCAL_GL_NEAREST);
      mGLContext->fTexParameteri(target,
                                LOCAL_GL_TEXTURE_MAG_FILTER,
                                LOCAL_GL_NEAREST);
      mGLContext->fTexImage2D(target,
                              0,
                              LOCAL_GL_RGBA,
                              5, 3, /* sufficiently NPOT */
                              0,
                              LOCAL_GL_RGBA,
                              LOCAL_GL_UNSIGNED_BYTE,
                              nullptr);

      // unbind this texture, in preparation for binding it to the FBO
      mGLContext->fBindTexture(target, 0);

      mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, testFBO);
      mGLContext->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER,
                                        LOCAL_GL_COLOR_ATTACHMENT0,
                                        target,
                                        testTexture,
                                        0);

      if (mGLContext->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER) ==
          LOCAL_GL_FRAMEBUFFER_COMPLETE)
      {
        mFBOTextureTarget = target;
        mGLContext->fDeleteTextures(1, &testTexture);
        break;
      }

      mGLContext->fDeleteTextures(1, &testTexture);
    }

    if (testFBO) {
      mGLContext->fDeleteFramebuffers(1, &testFBO);
    }

    if (mFBOTextureTarget == LOCAL_GL_NONE) {
      /* Unable to find a texture target that works with FBOs and NPOT textures */
      *out_failureReason = "FEATURE_FAILURE_OPENGL_NO_TEXTURE_TARGET";
      return false;
    }
  } else {
    // not trying to work around driver bugs, so TEXTURE_2D should just work
    mFBOTextureTarget = LOCAL_GL_TEXTURE_2D;
  }

  // back to default framebuffer, to avoid confusion
  mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);

  if (mFBOTextureTarget == LOCAL_GL_TEXTURE_RECTANGLE_ARB) {
    /* If we're using TEXTURE_RECTANGLE, then we must have the ARB
     * extension -- the EXT variant does not provide support for
     * texture rectangle access inside GLSL (sampler2DRect,
     * texture2DRect).
     */
    if (!mGLContext->IsExtensionSupported(gl::GLContext::ARB_texture_rectangle)){
      *out_failureReason = "FEATURE_FAILURE_OPENGL_ARB_EXT";
      return false;
    }
  }

  // Create a VBO for triangle vertices.
  mGLContext->fGenBuffers(1, &mTriangleVBO);

  /* Create a simple quad VBO */
  mGLContext->fGenBuffers(1, &mQuadVBO);

  // 4 quads, with the number of the quad (vertexID) encoded in w.
  GLfloat vertices[] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    1.0f, 1.0f, 0.0f, 0.0f,

    0.0f, 0.0f, 0.0f, 1.0f,
    1.0f, 0.0f, 0.0f, 1.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 0.0f, 0.0f, 1.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 1.0f, 0.0f, 1.0f,

    0.0f, 0.0f, 0.0f, 2.0f,
    1.0f, 0.0f, 0.0f, 2.0f,
    0.0f, 1.0f, 0.0f, 2.0f,
    1.0f, 0.0f, 0.0f, 2.0f,
    0.0f, 1.0f, 0.0f, 2.0f,
    1.0f, 1.0f, 0.0f, 2.0f,

    0.0f, 0.0f, 0.0f, 3.0f,
    1.0f, 0.0f, 0.0f, 3.0f,
    0.0f, 1.0f, 0.0f, 3.0f,
    1.0f, 0.0f, 0.0f, 3.0f,
    0.0f, 1.0f, 0.0f, 3.0f,
    1.0f, 1.0f, 0.0f, 3.0f,
  };
  HeapCopyOfStackArray<GLfloat> verticesOnHeap(vertices);

  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, mQuadVBO);
  mGLContext->fBufferData(LOCAL_GL_ARRAY_BUFFER,
                          verticesOnHeap.ByteLength(),
                          verticesOnHeap.Data(),
                          LOCAL_GL_STATIC_DRAW);
  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);

  nsCOMPtr<nsIConsoleService>
    console(do_GetService(NS_CONSOLESERVICE_CONTRACTID));

  if (console) {
    nsString msg;
    msg +=
      NS_LITERAL_STRING("OpenGL compositor Initialized Succesfully.\nVersion: ");
    msg += NS_ConvertUTF8toUTF16(
      nsDependentCString((const char*)mGLContext->fGetString(LOCAL_GL_VERSION)));
    msg += NS_LITERAL_STRING("\nVendor: ");
    msg += NS_ConvertUTF8toUTF16(
      nsDependentCString((const char*)mGLContext->fGetString(LOCAL_GL_VENDOR)));
    msg += NS_LITERAL_STRING("\nRenderer: ");
    msg += NS_ConvertUTF8toUTF16(
      nsDependentCString((const char*)mGLContext->fGetString(LOCAL_GL_RENDERER)));
    msg += NS_LITERAL_STRING("\nFBO Texture Target: ");
    if (mFBOTextureTarget == LOCAL_GL_TEXTURE_2D)
      msg += NS_LITERAL_STRING("TEXTURE_2D");
    else
      msg += NS_LITERAL_STRING("TEXTURE_RECTANGLE");
    console->LogStringMessage(msg.get());
  }

  return true;
}

/*
 * Returns a size that is equal to, or larger than and closest to,
 * aSize where both width and height are powers of two.
 * If the OpenGL setup is capable of using non-POT textures,
 * then it will just return aSize.
 */
static IntSize
CalculatePOTSize(const IntSize& aSize, GLContext* gl)
{
  if (CanUploadNonPowerOfTwo(gl))
    return aSize;

  return IntSize(RoundUpPow2(aSize.width), RoundUpPow2(aSize.height));
}

gfx::Rect
CompositorOGL::GetTextureCoordinates(gfx::Rect textureRect, TextureSource* aTexture)
{
  // If the OpenGL setup does not support non-power-of-two textures then the
  // texture's width and height will have been increased to the next
  // power-of-two (unless already a power of two). In that case we must scale
  // the texture coordinates to account for that.
  if (!CanUploadNonPowerOfTwo(mGLContext)) {
    const IntSize& textureSize = aTexture->GetSize();
    const IntSize potSize = CalculatePOTSize(textureSize, mGLContext);
    if (potSize != textureSize) {
      const float xScale = (float)textureSize.width / (float)potSize.width;
      const float yScale = (float)textureSize.height / (float)potSize.height;
      textureRect.Scale(xScale, yScale);
    }
  }

  return textureRect;
}

void
CompositorOGL::PrepareViewport(CompositingRenderTargetOGL* aRenderTarget)
{
  MOZ_ASSERT(aRenderTarget);
  // Logical surface size.
  const gfx::IntSize& size = aRenderTarget->mInitParams.mSize;
  // Physical surface size.
  const gfx::IntSize& phySize = aRenderTarget->mInitParams.mPhySize;

  // Set the viewport correctly.
  mGLContext->fViewport(0, 0, phySize.width, phySize.height);

  mViewportSize = size;

  if (!aRenderTarget->HasComplexProjection()) {
    // We flip the view matrix around so that everything is right-side up; we're
    // drawing directly into the window's back buffer, so this keeps things
    // looking correct.
    // XXX: We keep track of whether the window size changed, so we could skip
    // this update if it hadn't changed since the last call.

    // Matrix to transform (0, 0, aWidth, aHeight) to viewport space (-1.0, 1.0,
    // 2, 2) and flip the contents.
    Matrix viewMatrix;
    // Jihad T-502: with the renderbuffer FBO standing for the window, keep the WINDOW convention
    // (the flipped view) even though the context is offscreen. The intermediate-surface draw
    // (EffectTypes::RENDER_TARGET) flips its texture unconditionally to cancel this view flip;
    // without the view flip every frame drawn through an intermediate surface (a scroll frame
    // with a displayport, a clipped container under the world transform) came out upside down.
    // JihadReadFrame reverses the rows on the way out.
    if (mGLContext->IsOffscreen() && !gIsGtest && !mJihadWindowFBO) {
      // In case of rendering via GL Offscreen context, disable Y-Flipping
      viewMatrix.PreTranslate(-1.0, -1.0);
      viewMatrix.PreScale(2.0f / float(size.width), 2.0f / float(size.height));
    } else {
      viewMatrix.PreTranslate(-1.0, 1.0);
      viewMatrix.PreScale(2.0f / float(size.width), 2.0f / float(size.height));
      viewMatrix.PreScale(1.0f, -1.0f);
    }

    MOZ_ASSERT(mCurrentRenderTarget, "No destination");
    // If we're drawing directly to the window then we want to offset
    // drawing by the render offset.
    if (!mTarget && JihadIsWindowTarget()) {
      viewMatrix.PreTranslate(mRenderOffset.x, mRenderOffset.y);
    }

    Matrix4x4 matrix3d = Matrix4x4::From2D(viewMatrix);
    matrix3d._33 = 0.0f;
    mProjMatrix = matrix3d;
    mGLContext->fDepthRange(0.0f, 1.0f);
  } else {
    // XXX take into account mRenderOffset
    bool depthEnable;
    float zNear, zFar;
    aRenderTarget->GetProjection(mProjMatrix, depthEnable, zNear, zFar);
    mGLContext->fDepthRange(zNear, zFar);
  }
}

already_AddRefed<CompositingRenderTarget>
CompositorOGL::CreateRenderTarget(const IntRect &aRect, SurfaceInitMode aInit)
{
  MOZ_ASSERT(aRect.width != 0 && aRect.height != 0, "Trying to create a render target of invalid size");

  if (aRect.width * aRect.height == 0) {
    return nullptr;
  }

  if (!gl()) {
    // CompositingRenderTargetOGL does not work without a gl context.
    return nullptr;
  }

  GLuint tex = 0;
  GLuint fbo = 0;
  IntRect rect = aRect;
  IntSize FBOSize;
  CreateFBOWithTexture(rect, false, 0, &fbo, &tex, &FBOSize);
  RefPtr<CompositingRenderTargetOGL> surface
    = new CompositingRenderTargetOGL(this, aRect.TopLeft(), tex, fbo);
  surface->Initialize(aRect.Size(), FBOSize, mFBOTextureTarget, aInit);
  return surface.forget();
}

already_AddRefed<CompositingRenderTarget>
CompositorOGL::CreateRenderTargetFromSource(const IntRect &aRect,
                                            const CompositingRenderTarget *aSource,
                                            const IntPoint &aSourcePoint)
{
  MOZ_ASSERT(aRect.width != 0 && aRect.height != 0, "Trying to create a render target of invalid size");

  if (aRect.width * aRect.height == 0) {
    return nullptr;
  }

  if (!gl()) {
    return nullptr;
  }

  GLuint tex = 0;
  GLuint fbo = 0;
  const CompositingRenderTargetOGL* sourceSurface
    = static_cast<const CompositingRenderTargetOGL*>(aSource);
  IntRect sourceRect(aSourcePoint, aRect.Size());
  if (aSource) {
    CreateFBOWithTexture(sourceRect, true, sourceSurface->GetFBO(),
                         &fbo, &tex);
  } else {
    CreateFBOWithTexture(sourceRect, true, 0,
                         &fbo, &tex);
  }

  RefPtr<CompositingRenderTargetOGL> surface
    = new CompositingRenderTargetOGL(this, aRect.TopLeft(), tex, fbo);
  surface->Initialize(aRect.Size(),
                      sourceRect.Size(),
                      mFBOTextureTarget,
                      INIT_MODE_NONE);
  return surface.forget();
}

void
CompositorOGL::SetRenderTarget(CompositingRenderTarget *aSurface)
{
  MOZ_ASSERT(aSurface);
  CompositingRenderTargetOGL* surface
    = static_cast<CompositingRenderTargetOGL*>(aSurface);
  if (mCurrentRenderTarget != surface) {
    mCurrentRenderTarget = surface;
    if (mCurrentRenderTarget) {
      mContextStateTracker.PopOGLSection(gl(), "Frame");
    }
    mContextStateTracker.PushOGLSection(gl(), "Frame");
    surface->BindRenderTarget();
  }

  PrepareViewport(mCurrentRenderTarget);
}

CompositingRenderTarget*
CompositorOGL::GetCurrentRenderTarget() const
{
  return mCurrentRenderTarget;
}

static GLenum
GetFrameBufferInternalFormat(GLContext* gl,
                             GLuint aFrameBuffer,
                             mozilla::widget::CompositorWidget* aWidget)
{
  if (aFrameBuffer == 0) { // default framebuffer
    return aWidget->GetGLFrameBufferFormat();
  }
  return LOCAL_GL_RGBA;
}

void
CompositorOGL::ClearRect(const gfx::Rect& aRect)
{
  // Map aRect to OGL coordinates, origin:bottom-left
  GLint y = mViewportSize.height - (aRect.y + aRect.height);

  ScopedGLState scopedScissorTestState(mGLContext, LOCAL_GL_SCISSOR_TEST, true);
  ScopedScissorRect autoScissorRect(mGLContext, aRect.x, y, aRect.width, aRect.height);
  mGLContext->fClearColor(0.0, 0.0, 0.0, 0.0);
  mGLContext->fClear(LOCAL_GL_COLOR_BUFFER_BIT | LOCAL_GL_DEPTH_BUFFER_BIT);
}

void
CompositorOGL::BeginFrame(const nsIntRegion& aInvalidRegion,
                          const IntRect *aClipRectIn,
                          const IntRect& aRenderBounds,
                          const nsIntRegion& aOpaqueRegion,
                          IntRect *aClipRectOut,
                          IntRect *aRenderBoundsOut)
{
  PROFILER_LABEL("CompositorOGL", "BeginFrame",
    js::ProfileEntry::Category::GRAPHICS);

  MOZ_ASSERT(!mFrameInProgress, "frame still in progress (should have called EndFrame");

  gfx::IntRect rect;
  if (mUseExternalSurfaceSize) {
    rect = gfx::IntRect(0, 0, mSurfaceSize.width, mSurfaceSize.height);
  } else {
    rect = gfx::IntRect(aRenderBounds.x, aRenderBounds.y, aRenderBounds.width, aRenderBounds.height);
  }

  if (aRenderBoundsOut) {
    *aRenderBoundsOut = rect;
  }

  GLint width = rect.width;
  GLint height = rect.height;

  // We can't draw anything to something with no area
  // so just return
  if (width == 0 || height == 0)
    return;

  // We're about to actually draw a frame.
  mFrameInProgress = true;

  // If the widget size changed, we have to force a MakeCurrent
  // to make sure that GL sees the updated widget size.
  if (mWidgetSize.width != width ||
      mWidgetSize.height != height)
  {
    MakeCurrent(ForceMakeCurrent);
    mWidgetSize.width = width;
    mWidgetSize.height = height;
    // Jihad T-502: on an offscreen (pbuffer/FBO) context the screen buffer keeps the size it
    // was created with; a taller frame (the portrait card) was clipped. Resize it with the
    // frame.
    if (mGLContext->IsOffscreen() && mGLContext->Screen() &&
        mGLContext->OffscreenSize() != gfx::IntSize(width, height)) {
      mGLContext->ResizeScreenBuffer(gfx::IntSize(width, height));
    }
  } else {
    MakeCurrent();
  }

  mPixelsPerFrame = width * height;
  mPixelsFilled = 0;

  // Default blend function implements "OVER"
  mGLContext->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA,
                                 LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA);
  mGLContext->fEnable(LOCAL_GL_BLEND);

  mJihadDraws = mJihadSurfaceDraws = mJihadClipBottomMax = mJihadQuadBottomMax = 0;
  mJihadProbed = false;
  for (int i = 0; i < 4; i++) { mJihadRawSc[i] = mJihadCacheSc[i] = mJihadRawVp[i] = mJihadCacheVp[i] = -1; }
  mJihadClipBottomMin = 1 << 30;
  RefPtr<CompositingRenderTargetOGL> rt =
    CompositingRenderTargetOGL::RenderTargetForWindow(this,
                                                      IntSize(width, height),
                                                      mJihadWindowFBO);
  SetRenderTarget(rt);
  {
    // Jihad T-502 diagnostic: the geometry of every composite (every 40th, and every one whose
    // height is not the frame's)
    static uint32_t sN = 0;
    static int sLastH = -1;
    const bool odd = (height != sLastH && sLastH >= 0);
    if ((sN++ % 40) == 0 || odd) {
      JIHAD_GPU_LOG("[jihad-gpu] BeginFrame %u: render bounds %d,%d %dx%d, target %dx%d, clip %s, widget %dx%d, window fbo %u\n",
              sN, aRenderBounds.x, aRenderBounds.y, aRenderBounds.width, aRenderBounds.height, width, height,
              aClipRectIn ? nsPrintfCString("%d,%d %dx%d", aClipRectIn->x, aClipRectIn->y, aClipRectIn->width, aClipRectIn->height).get() : "none",
              mWidgetSize.width, mWidgetSize.height, mJihadWindowFBO);
    }
    sLastH = height;
  }

#ifdef DEBUG
  mWindowRenderTarget = mCurrentRenderTarget;
#endif

  if (aClipRectOut && !aClipRectIn) {
    aClipRectOut->SetRect(0, 0, width, height);
  }

  if (mJihadWindowFBO) {
    // Jihad T-502: the window frame starts white and opaque like the software path's buffer,
    // so a frame with no layers is the blank frame the daemon knows (and not published black).
    // A partial composite (a clip rect given) clears only inside it.
    // JIHAD_GPU_CLEAR_MAGENTA=1 (round 104 diagnostic): magenta, so rows no draw reached show as such
    static const bool sMagenta = getenv("JIHAD_GPU_CLEAR_MAGENTA") != nullptr;
    if (sMagenta) mGLContext->fClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    else mGLContext->fClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (aClipRectIn) {
      mGLContext->fEnable(LOCAL_GL_SCISSOR_TEST);
      mGLContext->fScissor(aClipRectIn->x, FlipY(aClipRectIn->y + aClipRectIn->height),
                           aClipRectIn->width, aClipRectIn->height);
    } else {
      mGLContext->fDisable(LOCAL_GL_SCISSOR_TEST);
    }
  } else {
  mGLContext->fClearColor(mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a);
  }
  mGLContext->fClear(LOCAL_GL_COLOR_BUFFER_BIT | LOCAL_GL_DEPTH_BUFFER_BIT);
}

void
CompositorOGL::CreateFBOWithTexture(const gfx::IntRect& aRect,
                                    bool aCopyFromSource,
                                    GLuint aSourceFrameBuffer,
                                    GLuint *aFBO, GLuint *aTexture,
                                    gfx::IntSize* aAllocSize)
{
  *aTexture = CreateTexture(aRect, aCopyFromSource, aSourceFrameBuffer,
                            aAllocSize);
  mGLContext->fGenFramebuffers(1, aFBO);
}

GLuint
CompositorOGL::CreateTexture(const IntRect& aRect, bool aCopyFromSource,
                             GLuint aSourceFrameBuffer, IntSize* aAllocSize)
{
  // we're about to create a framebuffer backed by textures to use as an intermediate
  // surface. What to do if its size (as given by aRect) would exceed the
  // maximum texture size supported by the GL? The present code chooses the compromise
  // of just clamping the framebuffer's size to the max supported size.
  // This gives us a lower resolution rendering of the intermediate surface (children layers).
  // See bug 827170 for a discussion.
  IntRect clampedRect = aRect;
  int32_t maxTexSize = GetMaxTextureSize();
  clampedRect.width = std::min(clampedRect.width, maxTexSize);
  clampedRect.height = std::min(clampedRect.height, maxTexSize);

  GLuint tex;

  mGLContext->fActiveTexture(LOCAL_GL_TEXTURE0);
  mGLContext->fGenTextures(1, &tex);
  mGLContext->fBindTexture(mFBOTextureTarget, tex);

  if (aCopyFromSource) {
    GLuint curFBO = mCurrentRenderTarget->GetFBO();
    if (curFBO != aSourceFrameBuffer) {
      mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, aSourceFrameBuffer);
    }

    // We're going to create an RGBA temporary fbo.  But to
    // CopyTexImage() from the current framebuffer, the framebuffer's
    // format has to be compatible with the new texture's.  So we
    // check the format of the framebuffer here and take a slow path
    // if it's incompatible.
    GLenum format =
      GetFrameBufferInternalFormat(gl(), aSourceFrameBuffer, mWidget);

    bool isFormatCompatibleWithRGBA
        = gl()->IsGLES() ? (format == LOCAL_GL_RGBA)
                          : true;

    if (isFormatCompatibleWithRGBA) {
      mGLContext->fCopyTexImage2D(mFBOTextureTarget,
                                  0,
                                  LOCAL_GL_RGBA,
                                  clampedRect.x, FlipY(clampedRect.y + clampedRect.height),
                                  clampedRect.width, clampedRect.height,
                                  0);
    } else {
      // Curses, incompatible formats.  Take a slow path.

      // RGBA
      size_t bufferSize = clampedRect.width * clampedRect.height * 4;
      auto buf = MakeUnique<uint8_t[]>(bufferSize);

      mGLContext->fReadPixels(clampedRect.x, clampedRect.y,
                              clampedRect.width, clampedRect.height,
                              LOCAL_GL_RGBA,
                              LOCAL_GL_UNSIGNED_BYTE,
                              buf.get());
      mGLContext->fTexImage2D(mFBOTextureTarget,
                              0,
                              LOCAL_GL_RGBA,
                              clampedRect.width, clampedRect.height,
                              0,
                              LOCAL_GL_RGBA,
                              LOCAL_GL_UNSIGNED_BYTE,
                              buf.get());
    }

    GLenum error = mGLContext->fGetError();
    if (error != LOCAL_GL_NO_ERROR) {
      nsAutoCString msg;
      msg.AppendPrintf("Texture initialization failed! -- error 0x%x, Source %d, Source format %d,  RGBA Compat %d",
                       error, aSourceFrameBuffer, format, isFormatCompatibleWithRGBA);
      NS_ERROR(msg.get());
    }
  } else {
    mGLContext->fTexImage2D(mFBOTextureTarget,
                            0,
                            LOCAL_GL_RGBA,
                            clampedRect.width, clampedRect.height,
                            0,
                            LOCAL_GL_RGBA,
                            LOCAL_GL_UNSIGNED_BYTE,
                            nullptr);
  }
  mGLContext->fTexParameteri(mFBOTextureTarget, LOCAL_GL_TEXTURE_MIN_FILTER,
                             LOCAL_GL_LINEAR);
  mGLContext->fTexParameteri(mFBOTextureTarget, LOCAL_GL_TEXTURE_MAG_FILTER,
                             LOCAL_GL_LINEAR);
  mGLContext->fTexParameteri(mFBOTextureTarget, LOCAL_GL_TEXTURE_WRAP_S,
                             LOCAL_GL_CLAMP_TO_EDGE);
  mGLContext->fTexParameteri(mFBOTextureTarget, LOCAL_GL_TEXTURE_WRAP_T,
                             LOCAL_GL_CLAMP_TO_EDGE);
  mGLContext->fBindTexture(mFBOTextureTarget, 0);

  if (aAllocSize) {
    aAllocSize->width = clampedRect.width;
    aAllocSize->height = clampedRect.height;
  }

  return tex;
}

ShaderConfigOGL
CompositorOGL::GetShaderConfigFor(Effect *aEffect,
                                  MaskType aMask,
                                  gfx::CompositionOp aOp,
                                  bool aColorMatrix,
                                  bool aDEAAEnabled) const
{
  ShaderConfigOGL config;

  switch(aEffect->mType) {
  case EffectTypes::SOLID_COLOR:
    config.SetRenderColor(true);
    break;
  case EffectTypes::YCBCR:
    config.SetYCbCr(true);
    break;
  case EffectTypes::NV12:
    config.SetNV12(true);
    config.SetTextureTarget(LOCAL_GL_TEXTURE_RECTANGLE_ARB);
    break;
  case EffectTypes::COMPONENT_ALPHA:
  {
    config.SetComponentAlpha(true);
    EffectComponentAlpha* effectComponentAlpha =
      static_cast<EffectComponentAlpha*>(aEffect);
    gfx::SurfaceFormat format = effectComponentAlpha->mOnWhite->GetFormat();
    config.SetRBSwap(format == gfx::SurfaceFormat::B8G8R8A8 ||
                     format == gfx::SurfaceFormat::B8G8R8X8);
    TextureSourceOGL* source = effectComponentAlpha->mOnWhite->AsSourceOGL();
    config.SetTextureTarget(source->GetTextureTarget());
    break;
  }
  case EffectTypes::RENDER_TARGET:
    config.SetTextureTarget(mFBOTextureTarget);
    break;
  default:
  {
    MOZ_ASSERT(aEffect->mType == EffectTypes::RGB);
    TexturedEffect* texturedEffect =
        static_cast<TexturedEffect*>(aEffect);
    TextureSourceOGL* source = texturedEffect->mTexture->AsSourceOGL();
    MOZ_ASSERT_IF(source->GetTextureTarget() == LOCAL_GL_TEXTURE_EXTERNAL,
                  source->GetFormat() == gfx::SurfaceFormat::R8G8B8A8 ||
                  source->GetFormat() == gfx::SurfaceFormat::R8G8B8X8);
    MOZ_ASSERT_IF(source->GetTextureTarget() == LOCAL_GL_TEXTURE_RECTANGLE_ARB,
                  source->GetFormat() == gfx::SurfaceFormat::R8G8B8A8 ||
                  source->GetFormat() == gfx::SurfaceFormat::R8G8B8X8 ||
                  source->GetFormat() == gfx::SurfaceFormat::R5G6B5_UINT16 ||
                  source->GetFormat() == gfx::SurfaceFormat::YUV422 );
    config = ShaderConfigFromTargetAndFormat(source->GetTextureTarget(),
                                             source->GetFormat());
    if (!texturedEffect->mPremultiplied) {
      config.SetNoPremultipliedAlpha();
    }
    break;
  }
  }
  config.SetColorMatrix(aColorMatrix);
  config.SetMask(aMask == MaskType::Mask);
  config.SetDEAA(aDEAAEnabled);
  config.SetCompositionOp(aOp);
  return config;
}

ShaderProgramOGL*
CompositorOGL::GetShaderProgramFor(const ShaderConfigOGL &aConfig)
{
  std::map<ShaderConfigOGL, ShaderProgramOGL *>::iterator iter = mPrograms.find(aConfig);
  if (iter != mPrograms.end())
    return iter->second;

  ProgramProfileOGL profile = ProgramProfileOGL::GetProfileFor(aConfig);
  ShaderProgramOGL *shader = new ShaderProgramOGL(gl(), profile);
  if (!shader->Initialize()) {
    delete shader;
    return nullptr;
  }

  mPrograms[aConfig] = shader;
  return shader;
}

void
CompositorOGL::ActivateProgram(ShaderProgramOGL* aProg)
{
  if (mCurrentProgram != aProg) {
    gl()->fUseProgram(aProg->GetProgram());
    mCurrentProgram = aProg;
  }
}

void
CompositorOGL::ResetProgram()
{
  mCurrentProgram = nullptr;
}

static bool SetBlendMode(GLContext* aGL, gfx::CompositionOp aBlendMode, bool aIsPremultiplied = true)
{
  if (BlendOpIsMixBlendMode(aBlendMode)) {
    // Mix-blend modes require an extra step (or more) that cannot be expressed
    // in the fixed-function blending capabilities of opengl. We handle them
    // separately in shaders, and the shaders assume we will use our default
    // blend function for compositing (premultiplied OP_OVER).
    return false;
  }
  if (aBlendMode == gfx::CompositionOp::OP_OVER && aIsPremultiplied) {
    return false;
  }

  GLenum srcBlend;
  GLenum dstBlend;
  GLenum srcAlphaBlend = LOCAL_GL_ONE;
  GLenum dstAlphaBlend = LOCAL_GL_ONE_MINUS_SRC_ALPHA;

  switch (aBlendMode) {
    case gfx::CompositionOp::OP_OVER:
      MOZ_ASSERT(!aIsPremultiplied);
      srcBlend = LOCAL_GL_SRC_ALPHA;
      dstBlend = LOCAL_GL_ONE_MINUS_SRC_ALPHA;
      break;
    case gfx::CompositionOp::OP_SOURCE:
      srcBlend = aIsPremultiplied ? LOCAL_GL_ONE : LOCAL_GL_SRC_ALPHA;
      dstBlend = LOCAL_GL_ZERO;
      srcAlphaBlend = LOCAL_GL_ONE;
      dstAlphaBlend = LOCAL_GL_ZERO;
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported blend mode!");
      return false;
  }

  aGL->fBlendFuncSeparate(srcBlend, dstBlend,
                          srcAlphaBlend, dstAlphaBlend);
  return true;
}

gfx::Point3D
CompositorOGL::GetLineCoefficients(const gfx::Point& aPoint1,
                                   const gfx::Point& aPoint2)
{
  // Return standard coefficients for a line between aPoint1 and aPoint2
  // for standard line equation:
  //
  // Ax + By + C = 0
  //
  // A = (p1.y – p2.y)
  // B = (p2.x – p1.x)
  // C = (p1.x * p2.y) – (p2.x * p1.y)

  gfx::Point3D coeffecients;
  coeffecients.x = aPoint1.y - aPoint2.y;
  coeffecients.y = aPoint2.x - aPoint1.x;
  coeffecients.z = aPoint1.x * aPoint2.y - aPoint2.x * aPoint1.y;

  coeffecients *= 1.0f / sqrtf(coeffecients.x * coeffecients.x +
                               coeffecients.y * coeffecients.y);

  // Offset outwards by 0.5 pixel as the edge is considered to be 1 pixel
  // wide and included within the interior of the polygon
  coeffecients.z += 0.5f;

  return coeffecients;
}

void
CompositorOGL::DrawQuad(const Rect& aRect,
                        const IntRect& aClipRect,
                        const EffectChain &aEffectChain,
                        Float aOpacity,
                        const gfx::Matrix4x4& aTransform,
                        const gfx::Rect& aVisibleRect)
{
  PROFILER_LABEL("CompositorOGL", "DrawQuad",
    js::ProfileEntry::Category::GRAPHICS);

  DrawGeometry(aRect, aClipRect, aEffectChain,
               aOpacity, aTransform, aVisibleRect);
}

void
CompositorOGL::DrawTriangle(const gfx::TexturedTriangle& aTriangle,
                            const gfx::IntRect& aClipRect,
                            const EffectChain& aEffectChain,
                            gfx::Float aOpacity,
                            const gfx::Matrix4x4& aTransform,
                            const gfx::Rect& aVisibleRect)
{
  PROFILER_LABEL("CompositorOGL", "DrawTriangle",
    js::ProfileEntry::Category::GRAPHICS);

  DrawGeometry(aTriangle, aClipRect, aEffectChain,
               aOpacity, aTransform, aVisibleRect);
}

template<typename Geometry>
void
CompositorOGL::DrawGeometry(const Geometry& aGeometry,
                            const IntRect& aClipRect,
                            const EffectChain &aEffectChain,
                            Float aOpacity,
                            const gfx::Matrix4x4& aTransform,
                            const gfx::Rect& aVisibleRect)
{
  MOZ_ASSERT(mFrameInProgress, "frame not started");
  MOZ_ASSERT(mCurrentRenderTarget, "No destination");

  static const bool sJihadPhases = getenv("JIHAD_GPU_PHASES") != nullptr;
  struct JihadDrawTimer {
    bool on; TimeStamp t; const EffectChain& chain; const gfx::Matrix4x4& xf; const gfx::Rect& rect;
    ~JihadDrawTimer() {
      if (!on) return;
      const double ms = (TimeStamp::Now() - t).ToMilliseconds();
      gJihadPhase[8] += ms; gJihadPhase[9] += 1;
      // T-504: a draw that blocks (the GL call waits in the driver), with what it drew
      static uint32_t sSlow = 0;
      if (ms >= 5.0 && sSlow++ < 80) {
        const Effect* e = chain.mPrimaryEffect;
        int tw = 0, th = 0;
        if (e && (e->mType == EffectTypes::RGB)) {
          if (TextureSource* src = static_cast<const TexturedEffect*>(e)->mTexture) { tw = src->GetSize().width; th = src->GetSize().height; }
        }
        const gfx::Rect dst = xf.TransformBounds(rect);
        JIHAD_GPU_LOG("[jihad-gpu] slow draw %.1f ms: effect %d, texture %dx%d, %.0fx%.0f px drawn at %.0f,%.0f (scale %.3f)\n",
                ms, e ? (int)e->mType : -1, tw, th, dst.width, dst.height, dst.x, dst.y, xf._11);
      }
    }
  } jihadDrawTimer{sJihadPhases, sJihadPhases ? TimeStamp::Now() : TimeStamp(), aEffectChain, aTransform, aVisibleRect};

  MakeCurrent();

  IntPoint offset = mCurrentRenderTarget->GetOrigin();
  IntSize size = mCurrentRenderTarget->GetSize();

  Rect renderBound(0, 0, size.width, size.height);
  renderBound.IntersectRect(renderBound, Rect(aClipRect));
  renderBound.MoveBy(offset);

  Rect destRect = aTransform.TransformAndClipBounds(aGeometry, renderBound);
  // Jihad T-502 round 140: a quad the clip leaves nothing of is not drawn. The culling below
  // never caught it: the empty rect, inflated by a pixel, is a 2x2 rect at the origin, which
  // always meets the target, so a band pass issued every tile of the page (60 draws, ~8 ms of
  // CPU on a desktop page) for the scissor to throw away. JIHAD_GPU_NO_CULL=1 restores that.
  static const bool sJihadNoCull = getenv("JIHAD_GPU_NO_CULL") != nullptr;
  if (destRect.IsEmpty() && !sJihadNoCull) {
    if (sJihadPhases) gJihadPhase[10] += 1;
    return;
  }
  {
    static const bool sDrawStats = getenv("JIHAD_GPU_DRAWSTATS") != nullptr;
    if (sDrawStats) {
      if (JihadIsWindowTarget()) {
        if (mJihadDraws < 48) {
          JihadDrawRec& d = mJihadDrawRecs[mJihadDraws];
          d.x = destRect.x; d.y = destRect.y; d.w = destRect.width; d.h = destRect.height;
          d.cy = aClipRect.y; d.cb = aClipRect.YMost();
          d.type = aEffectChain.mPrimaryEffect ? (int)aEffectChain.mPrimaryEffect->mType : -1;
          d.r = d.g = d.b = d.a = -1.0f; d.opacity = aOpacity;
          if (aEffectChain.mPrimaryEffect && aEffectChain.mPrimaryEffect->mType == EffectTypes::SOLID_COLOR) {
            const gfx::Color& c = static_cast<EffectSolidColor*>(aEffectChain.mPrimaryEffect.get())->mColor;
            d.r = c.r; d.g = c.g; d.b = c.b; d.a = c.a;
          }
        }
        ++mJihadDraws;
        mJihadClipBottomMax = std::max(mJihadClipBottomMax, aClipRect.YMost());
        mJihadClipBottomMin = std::min(mJihadClipBottomMin, aClipRect.YMost());
        mJihadQuadBottomMax = std::max(mJihadQuadBottomMax, (int)destRect.YMost());
      } else {
        ++mJihadSurfaceDraws;
      }
    }
  }

  // XXX: This doesn't handle 3D transforms. It also doesn't handled rotated
  //      quads. Fix me.
  mPixelsFilled += destRect.width * destRect.height;

  // Do a simple culling if this rect is out of target buffer.
  // Inflate a small size to avoid some numerical imprecision issue.
  destRect.Inflate(1, 1);
  destRect.MoveBy(-offset);
  renderBound = Rect(0, 0, size.width, size.height);
  if (!renderBound.Intersects(destRect)) {
    if (sJihadPhases) gJihadPhase[10] += 1;
    return;
  }

  LayerScope::DrawBegin();

  IntRect clipRect = aClipRect;
  // aClipRect is in destination coordinate space (after all
  // transforms and offsets have been applied) so if our
  // drawing is going to be shifted by mRenderOffset then we need
  // to shift the clip rect by the same amount.
  if (!mTarget && JihadIsWindowTarget()) {
    clipRect.MoveBy(mRenderOffset.x, mRenderOffset.y);
  }

  ScopedGLState scopedScissorTestState(mGLContext, LOCAL_GL_SCISSOR_TEST, true);
  ScopedScissorRect autoScissorRect(mGLContext, clipRect.x, FlipY(clipRect.y + clipRect.height),
                                    clipRect.width, clipRect.height);
  {
    static const bool sDrawStats = getenv("JIHAD_GPU_DRAWSTATS") != nullptr;
    if (sDrawStats && !mJihadProbed && JihadIsWindowTarget() && clipRect.height > 0) {
      mJihadProbed = true;
      mGLContext->JihadRawAndCached(mJihadRawSc, mJihadCacheSc, mJihadRawVp, mJihadCacheVp);
    }
  }

  MaskType maskType;
  EffectMask* effectMask;
  TextureSourceOGL* sourceMask = nullptr;
  gfx::Matrix4x4 maskQuadTransform;
  if (aEffectChain.mSecondaryEffects[EffectTypes::MASK]) {
    effectMask = static_cast<EffectMask*>(aEffectChain.mSecondaryEffects[EffectTypes::MASK].get());
    sourceMask = effectMask->mMaskTexture->AsSourceOGL();

    // NS_ASSERTION(textureMask->IsAlpha(),
    //              "OpenGL mask layers must be backed by alpha surfaces");

    // We're assuming that the gl backend won't cheat and use NPOT
    // textures when glContext says it can't (which seems to happen
    // on a mac when you force POT textures)
    IntSize maskSize = CalculatePOTSize(effectMask->mSize, mGLContext);

    const gfx::Matrix4x4& maskTransform = effectMask->mMaskTransform;
    NS_ASSERTION(maskTransform.Is2D(), "How did we end up with a 3D transform here?!");
    Rect bounds = Rect(Point(), Size(maskSize));
    bounds = maskTransform.As2D().TransformBounds(bounds);

    maskQuadTransform._11 = 1.0f/bounds.width;
    maskQuadTransform._22 = 1.0f/bounds.height;
    maskQuadTransform._41 = float(-bounds.x)/bounds.width;
    maskQuadTransform._42 = float(-bounds.y)/bounds.height;

    maskType = MaskType::Mask;
  } else {
    maskType = MaskType::MaskNone;
  }

  // Determine the color if this is a color shader and fold the opacity into
  // the color since color shaders don't have an opacity uniform.
  Color color;
  if (aEffectChain.mPrimaryEffect->mType == EffectTypes::SOLID_COLOR) {
    EffectSolidColor* effectSolidColor =
      static_cast<EffectSolidColor*>(aEffectChain.mPrimaryEffect.get());
    color = effectSolidColor->mColor;

    Float opacity = aOpacity * color.a;
    color.r *= opacity;
    color.g *= opacity;
    color.b *= opacity;
    color.a = opacity;

    // We can fold opacity into the color, so no need to consider it further.
    aOpacity = 1.f;
  }

  bool createdMixBlendBackdropTexture = false;
  GLuint mixBlendBackdrop = 0;
  gfx::CompositionOp blendMode = gfx::CompositionOp::OP_OVER;

  if (aEffectChain.mSecondaryEffects[EffectTypes::BLEND_MODE]) {
    EffectBlendMode *blendEffect =
      static_cast<EffectBlendMode*>(aEffectChain.mSecondaryEffects[EffectTypes::BLEND_MODE].get());
    blendMode = blendEffect->mBlendMode;
  }

  // Only apply DEAA to quads that have been transformed such that aliasing
  // could be visible
  bool bEnableAA = gfxPrefs::LayersDEAAEnabled() &&
                   !aTransform.Is2DIntegerTranslation();
  // Jihad T-504: on the GPU path an axis-aligned scale (the card's zoom, tiles painted at another
  // resolution) gets no edge antialiasing: its edges stay axis-aligned, and DEAA's own shader
  // variants were compiled on their first use, ~150 ms each on the Adreno 220, in the first frame
  // of a zoom (two of them: the first frame at a new zoom took 300-385 ms). Rotated and skewed
  // layers keep it.
  static const bool sJihadGpu = getenv("JIHAD_GPU_COMPOSITOR") && !getenv("JIHAD_GPU_DEAA");
  if (bEnableAA && sJihadGpu) {
    gfx::Matrix m2;
    if (aTransform.Is2D(&m2) && m2.PreservesAxisAlignedRectangles()) {
      bEnableAA = false;
    }
  }

  bool colorMatrix = aEffectChain.mSecondaryEffects[EffectTypes::COLOR_MATRIX];
  ShaderConfigOGL config = GetShaderConfigFor(aEffectChain.mPrimaryEffect,
                                              maskType, blendMode, colorMatrix,
                                              bEnableAA);

  config.SetOpacity(aOpacity != 1.f);
  ApplyPrimitiveConfig(config, aGeometry);

  ShaderProgramOGL *program = GetShaderProgramFor(config);
  ActivateProgram(program);
  program->SetProjectionMatrix(mProjMatrix);
  program->SetLayerTransform(aTransform);
  LayerScope::SetLayerTransform(aTransform);

  if (colorMatrix) {
      EffectColorMatrix* effectColorMatrix =
        static_cast<EffectColorMatrix*>(aEffectChain.mSecondaryEffects[EffectTypes::COLOR_MATRIX].get());
      program->SetColorMatrix(effectColorMatrix->mColorMatrix);
  }

  if (BlendOpIsMixBlendMode(blendMode)) {
    gfx::Matrix4x4 backdropTransform;

    if (gl()->IsExtensionSupported(GLContext::NV_texture_barrier)) {
      // The NV_texture_barrier extension lets us read directly from the
      // backbuffer. Let's do that.
      // We need to tell OpenGL about this, so that it can make sure everything
      // on the GPU is happening in the right order.
      gl()->fTextureBarrier();
      mixBlendBackdrop = mCurrentRenderTarget->GetTextureHandle();
    } else {
      gfx::IntRect rect = ComputeBackdropCopyRect(aGeometry, aClipRect,
                                                  aTransform, &backdropTransform);
      mixBlendBackdrop = CreateTexture(rect, true, mCurrentRenderTarget->GetFBO());
      createdMixBlendBackdropTexture = true;
    }
    program->SetBackdropTransform(backdropTransform);
  }

  program->SetRenderOffset(offset.x, offset.y);
  LayerScope::SetRenderOffset(offset.x, offset.y);

  if (aOpacity != 1.f)
    program->SetLayerOpacity(aOpacity);

  if (config.mFeatures & ENABLE_TEXTURE_RECT) {
    TextureSourceOGL* source = nullptr;
    if (aEffectChain.mPrimaryEffect->mType == EffectTypes::COMPONENT_ALPHA) {
      EffectComponentAlpha* effectComponentAlpha =
        static_cast<EffectComponentAlpha*>(aEffectChain.mPrimaryEffect.get());
      source = effectComponentAlpha->mOnWhite->AsSourceOGL();
    } else {
      TexturedEffect* texturedEffect =
        static_cast<TexturedEffect*>(aEffectChain.mPrimaryEffect.get());
      source = texturedEffect->mTexture->AsSourceOGL();
    }
    if (source) {
        // This is used by IOSurface that use 0,0...w,h coordinate rather then 0,0..1,1.
        program->SetTexCoordMultiplier(source->GetSize().width, source->GetSize().height);
    }
  }

  // XXX kip - These calculations could be performed once per layer rather than
  //           for every tile.  This might belong in Compositor.cpp once DEAA
  //           is implemented for DirectX.
  if (bEnableAA) {
    // Calculate the transformed vertices of aVisibleRect in screen space
    // pixels, mirroring the calculations in the vertex shader
    Matrix4x4 flatTransform = aTransform;
    flatTransform.PostTranslate(-offset.x, -offset.y, 0.0f);
    flatTransform *= mProjMatrix;

    Rect viewportClip = Rect(-1.0f, -1.0f, 2.0f, 2.0f);
    size_t edgeCount = 0;
    Point3D coefficients[4];

    Point points[Matrix4x4::kTransformAndClipRectMaxVerts];
    size_t pointCount = flatTransform.TransformAndClipRect(aVisibleRect, viewportClip, points);
    for (size_t i = 0; i < pointCount; i++) {
      points[i] = Point((points[i].x * 0.5f + 0.5f) * mViewportSize.width,
                        (points[i].y * 0.5f + 0.5f) * mViewportSize.height);
    }
    if (pointCount > 2) {
      // Use shoelace formula on a triangle in the clipped quad to determine if
      // winding order is reversed.  Iterate through the triangles until one is
      // found with a non-zero area.
      float winding = 0.0f;
      size_t wp = 0;
      while (winding == 0.0f && wp < pointCount) {
        int wp1 = (wp + 1) % pointCount;
        int wp2 = (wp + 2) % pointCount;
        winding = (points[wp1].x - points[wp].x) * (points[wp1].y + points[wp].y) +
                  (points[wp2].x - points[wp1].x) * (points[wp2].y + points[wp1].y) +
                  (points[wp].x - points[wp2].x) * (points[wp].y + points[wp2].y);
        wp++;
      }
      bool frontFacing = winding >= 0.0f;

      // Calculate the line coefficients used by the DEAA shader to determine the
      // sub-pixel coverage of the edge pixels
      for (size_t i=0; i<pointCount; i++) {
        const Point& p1 = points[i];
        const Point& p2 = points[(i + 1) % pointCount];
        // Create a DEAA edge for any non-straight lines, to a maximum of 4
        if (p1.x != p2.x && p1.y != p2.y && edgeCount < 4) {
          if (frontFacing) {
            coefficients[edgeCount++] = GetLineCoefficients(p2, p1);
          } else {
            coefficients[edgeCount++] = GetLineCoefficients(p1, p2);
          }
        }
      }
    }

    // The coefficients that are not needed must not cull any fragments.
    // We fill these unused coefficients with a clipping plane that has no
    // effect.
    for (size_t i = edgeCount; i < 4; i++) {
      coefficients[i] = Point3D(0.0f, 1.0f, mViewportSize.height);
    }

    // Set uniforms required by DEAA shader
    Matrix4x4 transformInverted = aTransform;
    transformInverted.Invert();
    program->SetLayerTransformInverse(transformInverted);
    program->SetDEAAEdges(coefficients);
    program->SetVisibleCenter(aVisibleRect.Center());
    program->SetViewportSize(mViewportSize);
  }

  bool didSetBlendMode = false;

  switch (aEffectChain.mPrimaryEffect->mType) {
    case EffectTypes::SOLID_COLOR: {
      program->SetRenderColor(color);

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE0, maskQuadTransform);
      }
      if (mixBlendBackdrop) {
        BindBackdrop(program, mixBlendBackdrop, LOCAL_GL_TEXTURE1);
      }

      didSetBlendMode = SetBlendMode(gl(), blendMode);

      BindAndDrawGeometry(program, aGeometry);
    }
    break;

  case EffectTypes::RGB: {
      TexturedEffect* texturedEffect =
          static_cast<TexturedEffect*>(aEffectChain.mPrimaryEffect.get());
      TextureSource *source = texturedEffect->mTexture;

      didSetBlendMode = SetBlendMode(gl(), blendMode, texturedEffect->mPremultiplied);

      gfx::SamplingFilter samplingFilter = texturedEffect->mSamplingFilter;

      source->AsSourceOGL()->BindTexture(LOCAL_GL_TEXTURE0, samplingFilter);

      program->SetTextureUnit(0);

      Matrix4x4 textureTransform = source->AsSourceOGL()->GetTextureTransform();
      program->SetTextureTransform(textureTransform);

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE1, maskQuadTransform);
      }
      if (mixBlendBackdrop) {
        BindBackdrop(program, mixBlendBackdrop, LOCAL_GL_TEXTURE2);
      }

      BindAndDrawGeometryWithTextureRect(program, aGeometry,
                                         texturedEffect->mTextureCoords, source);
    }
    break;
  case EffectTypes::YCBCR: {
      EffectYCbCr* effectYCbCr =
        static_cast<EffectYCbCr*>(aEffectChain.mPrimaryEffect.get());
      TextureSource* sourceYCbCr = effectYCbCr->mTexture;
      const int Y = 0, Cb = 1, Cr = 2;
      TextureSourceOGL* sourceY =  sourceYCbCr->GetSubSource(Y)->AsSourceOGL();
      TextureSourceOGL* sourceCb = sourceYCbCr->GetSubSource(Cb)->AsSourceOGL();
      TextureSourceOGL* sourceCr = sourceYCbCr->GetSubSource(Cr)->AsSourceOGL();

      if (!sourceY || !sourceCb || !sourceCr) {
        NS_WARNING("Invalid layer texture.");
        return;
      }

      sourceY->BindTexture(LOCAL_GL_TEXTURE0, effectYCbCr->mSamplingFilter);
      sourceCb->BindTexture(LOCAL_GL_TEXTURE1, effectYCbCr->mSamplingFilter);
      sourceCr->BindTexture(LOCAL_GL_TEXTURE2, effectYCbCr->mSamplingFilter);

      program->SetYCbCrTextureUnits(Y, Cb, Cr);
      program->SetTextureTransform(Matrix4x4());
      program->SetYUVColorSpace(effectYCbCr->mYUVColorSpace, effectYCbCr->mColorRange);

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE3, maskQuadTransform);
      }
      if (mixBlendBackdrop) {
        BindBackdrop(program, mixBlendBackdrop, LOCAL_GL_TEXTURE4);
      }
      didSetBlendMode = SetBlendMode(gl(), blendMode);
      BindAndDrawGeometryWithTextureRect(program,
                                         aGeometry,
                                         effectYCbCr->mTextureCoords,
                                         sourceYCbCr->GetSubSource(Y));
    }
    break;
  case EffectTypes::NV12: {
      EffectNV12* effectNV12 =
        static_cast<EffectNV12*>(aEffectChain.mPrimaryEffect.get());
      TextureSource* sourceNV12 = effectNV12->mTexture;
      const int Y = 0, CbCr = 1;
      TextureSourceOGL* sourceY =  sourceNV12->GetSubSource(Y)->AsSourceOGL();
      TextureSourceOGL* sourceCbCr = sourceNV12->GetSubSource(CbCr)->AsSourceOGL();

      if (!sourceY || !sourceCbCr) {
        NS_WARNING("Invalid layer texture.");
        return;
      }

      sourceY->BindTexture(LOCAL_GL_TEXTURE0, effectNV12->mSamplingFilter);
      sourceCbCr->BindTexture(LOCAL_GL_TEXTURE1, effectNV12->mSamplingFilter);

      if (config.mFeatures & ENABLE_TEXTURE_RECT) {
        // This is used by IOSurface that use 0,0...w,h coordinate rather then 0,0..1,1.
        program->SetCbCrTexCoordMultiplier(sourceCbCr->GetSize().width, sourceCbCr->GetSize().height);
      }

      program->SetNV12TextureUnits(Y, CbCr);
      program->SetTextureTransform(Matrix4x4());

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE2, maskQuadTransform);
      }
      if (mixBlendBackdrop) {
        BindBackdrop(program, mixBlendBackdrop, LOCAL_GL_TEXTURE3);
      }
      didSetBlendMode = SetBlendMode(gl(), blendMode);
      BindAndDrawGeometryWithTextureRect(program,
                                         aGeometry,
                                         effectNV12->mTextureCoords,
                                         sourceNV12->GetSubSource(Y));
    }
    break;
  case EffectTypes::RENDER_TARGET: {
      EffectRenderTarget* effectRenderTarget =
        static_cast<EffectRenderTarget*>(aEffectChain.mPrimaryEffect.get());
      RefPtr<CompositingRenderTargetOGL> surface
        = static_cast<CompositingRenderTargetOGL*>(effectRenderTarget->mRenderTarget.get());

      surface->BindTexture(LOCAL_GL_TEXTURE0, mFBOTextureTarget);

      // Drawing is always flipped, but when copying between surfaces we want to avoid
      // this, so apply a flip here to cancel the other one out.
      Matrix transform;
      transform.PreTranslate(0.0, 1.0);
      transform.PreScale(1.0f, -1.0f);
      program->SetTextureTransform(Matrix4x4::From2D(transform));
      program->SetTextureUnit(0);

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE1, maskQuadTransform);
      }
      if (mixBlendBackdrop) {
        BindBackdrop(program, mixBlendBackdrop, LOCAL_GL_TEXTURE2);
      }

      if (config.mFeatures & ENABLE_TEXTURE_RECT) {
        // 2DRect case, get the multiplier right for a sampler2DRect
        program->SetTexCoordMultiplier(surface->GetSize().width,
                                       surface->GetSize().height);
      }

      // Drawing is always flipped, but when copying between surfaces we want to avoid
      // this. Pass true for the flip parameter to introduce a second flip
      // that cancels the other one out.
      didSetBlendMode = SetBlendMode(gl(), blendMode);
      BindAndDrawGeometry(program, aGeometry);
    }
    break;
  case EffectTypes::COMPONENT_ALPHA: {
      MOZ_ASSERT(gfxPrefs::ComponentAlphaEnabled());
      MOZ_ASSERT(blendMode == gfx::CompositionOp::OP_OVER, "Can't support blend modes with component alpha!");
      EffectComponentAlpha* effectComponentAlpha =
        static_cast<EffectComponentAlpha*>(aEffectChain.mPrimaryEffect.get());
      TextureSourceOGL* sourceOnWhite = effectComponentAlpha->mOnWhite->AsSourceOGL();
      TextureSourceOGL* sourceOnBlack = effectComponentAlpha->mOnBlack->AsSourceOGL();

      if (!sourceOnBlack->IsValid() ||
          !sourceOnWhite->IsValid()) {
        NS_WARNING("Invalid layer texture for component alpha");
        return;
      }

      sourceOnBlack->BindTexture(LOCAL_GL_TEXTURE0, effectComponentAlpha->mSamplingFilter);
      sourceOnWhite->BindTexture(LOCAL_GL_TEXTURE1, effectComponentAlpha->mSamplingFilter);

      program->SetBlackTextureUnit(0);
      program->SetWhiteTextureUnit(1);
      program->SetTextureTransform(Matrix4x4());

      if (maskType != MaskType::MaskNone) {
        BindMaskForProgram(program, sourceMask, LOCAL_GL_TEXTURE2, maskQuadTransform);
      }
      // Pass 1.
      gl()->fBlendFuncSeparate(LOCAL_GL_ZERO, LOCAL_GL_ONE_MINUS_SRC_COLOR,
                               LOCAL_GL_ONE, LOCAL_GL_ONE);
      program->SetTexturePass2(false);
      BindAndDrawGeometryWithTextureRect(program,
                                         aGeometry,
                                         effectComponentAlpha->mTextureCoords,
                                         effectComponentAlpha->mOnBlack);

      // Pass 2.
      gl()->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE,
                               LOCAL_GL_ONE, LOCAL_GL_ONE);
      program->SetTexturePass2(true);
      BindAndDrawGeometryWithTextureRect(program,
                                         aGeometry,
                                         effectComponentAlpha->mTextureCoords,
                                         effectComponentAlpha->mOnBlack);

      mGLContext->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA,
                                     LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA);
    }
    break;
  default:
    MOZ_ASSERT(false, "Unhandled effect type");
    break;
  }

  if (didSetBlendMode) {
    gl()->fBlendFuncSeparate(LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA,
                             LOCAL_GL_ONE, LOCAL_GL_ONE_MINUS_SRC_ALPHA);
  }
  if (createdMixBlendBackdropTexture) {
    gl()->fDeleteTextures(1, &mixBlendBackdrop);
  }

  // in case rendering has used some other GL context
  MakeCurrent();

  LayerScope::DrawEnd(mGLContext, aEffectChain,
                      aGeometry.width, aGeometry.height);
}

void
CompositorOGL::BindAndDrawGeometry(ShaderProgramOGL* aProgram,
                                   const gfx::Rect& aRect,
                                   const gfx::Rect& aTextureRect)
{
  BindAndDrawQuad(aProgram, aRect, aTextureRect);
}

void
CompositorOGL::BindAndDrawGeometry(ShaderProgramOGL* aProgram,
                                   const gfx::TexturedTriangle& aTriangle,
                                   const gfx::Rect& aTextureRect)
{
  NS_ASSERTION(aProgram->HasInitialized(), "Shader program not correctly initialized");

  const gfx::TexturedTriangle& t = aTriangle;
  const gfx::Triangle& tex = t.textureCoords;

  GLfloat vertices[] = {
    t.p1.x, t.p1.y, 0.0f, 1.0f, tex.p1.x, tex.p1.y,
    t.p2.x, t.p2.y, 0.0f, 1.0f, tex.p2.x, tex.p2.y,
    t.p3.x, t.p3.y, 0.0f, 1.0f, tex.p3.x, tex.p3.y
  };

  HeapCopyOfStackArray<GLfloat> verticesOnHeap(vertices);
  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, mTriangleVBO);
  mGLContext->fBufferData(LOCAL_GL_ARRAY_BUFFER,
                          verticesOnHeap.ByteLength(),
                          verticesOnHeap.Data(),
                          LOCAL_GL_STREAM_DRAW);

  const GLsizei stride = 6 * sizeof(GLfloat);
  InitializeVAO(kCoordinateAttributeIndex, 4, stride, 0);
  InitializeVAO(kTexCoordinateAttributeIndex, 2, stride, 4 * sizeof(GLfloat));

  mGLContext->fDrawArrays(LOCAL_GL_TRIANGLES, 0, 3);

  mGLContext->fDisableVertexAttribArray(kCoordinateAttributeIndex);
  mGLContext->fDisableVertexAttribArray(kTexCoordinateAttributeIndex);
  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);
}

// |aRect| is the rectangle we want to draw to. We will draw it with
// up to 4 draw commands if necessary to avoid wrapping.
// |aTexCoordRect| is the rectangle from the texture that we want to
// draw using the given program.
// |aTexture| is the texture we are drawing. Its actual size can be
// larger than the rectangle given by |texCoordRect|.
void
CompositorOGL::BindAndDrawGeometryWithTextureRect(ShaderProgramOGL *aProg,
                                                  const Rect& aRect,
                                                  const Rect& aTexCoordRect,
                                                  TextureSource *aTexture)
{
  Rect scaledTexCoordRect = GetTextureCoordinates(aTexCoordRect, aTexture);
  Rect layerRects[4];
  Rect textureRects[4];
  size_t rects = DecomposeIntoNoRepeatRects(aRect,
                                            scaledTexCoordRect,
                                            &layerRects,
                                            &textureRects);

  BindAndDrawQuads(aProg, rects, layerRects, textureRects);
}

void
CompositorOGL::BindAndDrawGeometryWithTextureRect(ShaderProgramOGL *aProg,
                                                  const gfx::TexturedTriangle& aTriangle,
                                                  const gfx::Rect& aTexCoordRect,
                                                  TextureSource *aTexture)
{
  BindAndDrawGeometry(aProg, aTriangle,
                      GetTextureCoordinates(aTexCoordRect, aTexture));
}

void
CompositorOGL::BindAndDrawQuads(ShaderProgramOGL *aProg,
                                int aQuads,
                                const Rect* aLayerRects,
                                const Rect* aTextureRects)
{
  NS_ASSERTION(aProg->HasInitialized(), "Shader program not correctly initialized");

  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, mQuadVBO);
  InitializeVAO(kCoordinateAttributeIndex, 4, 0, 0);

  aProg->SetLayerRects(aLayerRects);
  if (aProg->GetTextureCount() > 0) {
    aProg->SetTextureRects(aTextureRects);
  }

  // We are using GL_TRIANGLES here because the Mac Intel drivers fail to properly
  // process uniform arrays with GL_TRIANGLE_STRIP. Go figure.
  mGLContext->fDrawArrays(LOCAL_GL_TRIANGLES, 0, 6 * aQuads);
  mGLContext->fDisableVertexAttribArray(kCoordinateAttributeIndex);
  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);
  LayerScope::SetDrawRects(aQuads, aLayerRects, aTextureRects);
}

void
CompositorOGL::InitializeVAO(const GLuint aAttrib, const GLint aComponents,
                             const GLsizei aStride, const size_t aOffset)
{
  mGLContext->fVertexAttribPointer(aAttrib, aComponents, LOCAL_GL_FLOAT,
                                   LOCAL_GL_FALSE, aStride,
                                   reinterpret_cast<GLvoid*>(aOffset));
  mGLContext->fEnableVertexAttribArray(aAttrib);
}

void
CompositorOGL::EndFrame()
{
  PROFILER_LABEL("CompositorOGL", "EndFrame",
    js::ProfileEntry::Category::GRAPHICS);

  MOZ_ASSERT(mCurrentRenderTarget == mWindowRenderTarget, "Rendering target not properly restored");

#ifdef MOZ_DUMP_PAINTING
  if (gfxEnv::DumpCompositorTextures()) {
    LayoutDeviceIntSize size;
    if (mUseExternalSurfaceSize) {
      size = LayoutDeviceIntSize(mSurfaceSize.width, mSurfaceSize.height);
    } else {
      size = mWidget->GetClientSize();
    }
    RefPtr<DrawTarget> target = gfxPlatform::GetPlatform()->CreateOffscreenContentDrawTarget(IntSize(size.width, size.height), SurfaceFormat::B8G8R8A8);
    if (target) {
      CopyToTarget(target, nsIntPoint(), Matrix());
      WriteSnapshotToDumpFile(this, target);
    }
  }
#endif

  mContextStateTracker.PopOGLSection(gl(), "Frame");

  mFrameInProgress = false;

  if (mTarget) {
    CopyToTarget(mTarget, mTargetBounds.TopLeft(), Matrix());
    mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);
    mCurrentRenderTarget = nullptr;
    Compositor::EndFrame();
    return;
  }

  mCurrentRenderTarget = nullptr;

  if (mTexturePool) {
    mTexturePool->EndFrame();
  }

  mGLContext->SwapBuffers();
  mGLContext->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);

  // Unbind all textures
  for (GLuint i = 0; i <= 4; i++) {
    mGLContext->fActiveTexture(LOCAL_GL_TEXTURE0 + i);
    mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, 0);
    if (!mGLContext->IsGLES()) {
      mGLContext->fBindTexture(LOCAL_GL_TEXTURE_RECTANGLE_ARB, 0);
    }
  }

  Compositor::EndFrame();
}

void
CompositorOGL::EndFrameForExternalComposition(const gfx::Matrix& aTransform)
{
  MOZ_ASSERT(!mTarget);
  if (mTexturePool) {
    mTexturePool->EndFrame();
  }
}

void
CompositorOGL::SetDestinationSurfaceSize(const IntSize& aSize)
{
  mSurfaceSize.width = aSize.width;
  mSurfaceSize.height = aSize.height;
}

bool
CompositorOGL::JihadIsWindowTarget() const
{
  return mCurrentRenderTarget &&
         (mCurrentRenderTarget->IsWindow() ||
          (mJihadWindowFBO && mCurrentRenderTarget->GetFBO() == mJihadWindowFBO));
}

// Jihad T-502: RGBA rows as glReadPixels wrote them (GL row 0 = the BOTTOM of the flipped
// window view) -> BGRA rows with row 0 at the top, in place, one load and one store per pixel.
// aForceAlpha writes 0xFF into the alpha byte (the adapter blits the words raw).
static void
JihadSwizzleAndFlipRows(uint8_t* aData, int aStride, int aW, int aH, bool aForceAlpha)
{
  for (int y = 0; y < aH / 2; y++) {
    uint8_t* a = aData + (size_t)y * aStride;
    uint8_t* b = aData + (size_t)(aH - 1 - y) * aStride;
    int x = 0;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    for (; x + 16 <= aW; x += 16, a += 64, b += 64) {
      uint8x16x4_t va = vld4q_u8(a), vb = vld4q_u8(b);
      uint8x16_t r = va.val[0]; va.val[0] = va.val[2]; va.val[2] = r;
      r = vb.val[0]; vb.val[0] = vb.val[2]; vb.val[2] = r;
      if (aForceAlpha) { va.val[3] = vdupq_n_u8(0xff); vb.val[3] = vdupq_n_u8(0xff); }
      vst4q_u8(a, vb); vst4q_u8(b, va);
    }
#endif
    for (; x < aW; x++, a += 4, b += 4) {
      const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = aForceAlpha ? 0xff : a[3];
      a[0] = b[2]; a[1] = b[1]; a[2] = b[0]; a[3] = aForceAlpha ? 0xff : b[3];
      b[0] = a2; b[1] = a1; b[2] = a0; b[3] = a3;
    }
  }
  if (aH & 1) {
    uint8_t* p = aData + (size_t)(aH / 2) * aStride;
    for (int x = 0; x < aW; x++, p += 4) {
      const uint8_t r = p[0]; p[0] = p[2]; p[2] = r; if (aForceAlpha) p[3] = 0xff;
    }
  }
}

static GLuint
JihadCompileShader(GLContext* aGL, GLenum aType, const char* aSrc)
{
  GLuint sh = aGL->fCreateShader(aType);
  aGL->fShaderSource(sh, 1, &aSrc, nullptr);
  aGL->fCompileShader(sh);
  GLint ok = 0;
  aGL->fGetShaderiv(sh, LOCAL_GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {0};
    aGL->fGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
    fprintf(stderr, "[jihad-gpu] swizzle pass: shader failed: %s\n", log);
    aGL->fDeleteShader(sh);
    return 0;
  }
  return sh;
}

bool
CompositorOGL::JihadEnsureSwizzlePass(int aW, int aH)
{
  if (mJihadSwizzleFailed || !mJihadWindowTex) {
    return false;
  }
  GLContext* gl = mGLContext;
  if (!mJihadSwizzleProg) {
    // The window texture's image region (x 0..w, GL rows 0..h, the top row at y = h-1) drawn
    // flipped onto a w x h target: the target's bottom row samples the image's top row, so the
    // bottom-up read comes out top-down. Red and blue swapped and alpha forced in the shader.
    static const char* kVert =
      "attribute vec2 aPos; uniform vec2 uScale; varying vec2 vTex;\n"
      "void main() { vTex = vec2((aPos.x + 1.0) * 0.5 * uScale.x, (1.0 - aPos.y) * 0.5 * uScale.y);"
      " gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char* kFrag =
      "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
      "uniform sampler2D uTex; varying vec2 vTex;\n"
      "void main() { vec4 c = texture2D(uTex, vTex); gl_FragColor = vec4(c.b, c.g, c.r, 1.0); }\n";
    GLuint vs = JihadCompileShader(gl, LOCAL_GL_VERTEX_SHADER, kVert);
    GLuint fs = vs ? JihadCompileShader(gl, LOCAL_GL_FRAGMENT_SHADER, kFrag) : 0;
    if (!vs || !fs) {
      if (vs) gl->fDeleteShader(vs);
      mJihadSwizzleFailed = true;
      return false;
    }
    GLuint prog = gl->fCreateProgram();
    gl->fAttachShader(prog, vs);
    gl->fAttachShader(prog, fs);
    gl->fLinkProgram(prog);
    gl->fDeleteShader(vs);
    gl->fDeleteShader(fs);
    GLint ok = 0;
    gl->fGetProgramiv(prog, LOCAL_GL_LINK_STATUS, &ok);
    if (!ok) {
      fprintf(stderr, "[jihad-gpu] swizzle pass: link failed\n");
      gl->fDeleteProgram(prog);
      mJihadSwizzleFailed = true;
      return false;
    }
    mJihadSwizzleProg = prog;
    mJihadSwizzlePos = gl->fGetAttribLocation(prog, "aPos");
    mJihadSwizzleTex = gl->fGetUniformLocation(prog, "uTex");
    mJihadSwizzleScale = gl->fGetUniformLocation(prog, "uScale");
  }
  if (mJihadReadSize != gfx::IntSize(aW, aH)) {
    if (mJihadReadFBO) {
      gl->fDeleteFramebuffers(1, &mJihadReadFBO);
      gl->fDeleteRenderbuffers(1, &mJihadReadRB);
      mJihadReadFBO = mJihadReadRB = 0;
    }
    gl->fGenRenderbuffers(1, &mJihadReadRB);
    gl->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, mJihadReadRB);
    gl->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, aW, aH);
    gl->fGenFramebuffers(1, &mJihadReadFBO);
    gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadReadFBO);
    gl->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_RENDERBUFFER, mJihadReadRB);
    GLenum status = gl->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    if (status != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "[jihad-gpu] swizzle pass: read target %dx%d incomplete (0x%x)\n", aW, aH, status);
      gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);
      gl->fDeleteFramebuffers(1, &mJihadReadFBO);
      gl->fDeleteRenderbuffers(1, &mJihadReadRB);
      mJihadReadFBO = mJihadReadRB = 0;
      mJihadSwizzleFailed = true;
      return false;
    }
    mJihadReadSize = gfx::IntSize(aW, aH);
    JIHAD_GPU_LOG("[jihad-gpu] swizzle pass: program %u, read target %dx%d (FBO %u)\n", mJihadSwizzleProg, aW, aH, mJihadReadFBO);
  }
  return true;
}

// ---- Jihad T-502: the read thread --------------------------------------------------------------
namespace {
void JihadReadThreadInitTrampoline(CompositorOGL* aC) { aC->JihadReadThreadInit(); }
void JihadReadThreadRunTrampoline(CompositorOGL* aC, JihadFrameRequest* aReq) { aC->JihadReadOnReadThread(aReq); }
} // namespace

bool
CompositorOGL::JihadEnsureReadThread()
{
  if (mJihadReadInitDone) {
    return mJihadReadInitOk;
  }
  mJihadReadInitDone = true;
  // Off unless asked for (JIHAD_GPU_READ_THREAD=1): measured 2026-10-17 (runs 59-86) it gained
  // nothing on the animation page and lost a third on the scroll page, because on this driver
  // any GL call on the compositor context waits behind the read anyway.
  if (!mJihadTargetFBO[1] || !getenv("JIHAD_GPU_READ_THREAD")) {
    return false;
  }
  mJihadReadThread = new base::Thread("JihadRead");
  if (!mJihadReadThread->Start()) {
    fprintf(stderr, "[jihad-gpu] read thread: could not start\n");
    delete mJihadReadThread;
    mJihadReadThread = nullptr;
    return false;
  }
  // the context and its FBOs are made on the thread that uses them; wait for the verdict once
  bool answered = false;
  {
    MonitorAutoLock lock(mJihadReadMon);
    mJihadReadInitAnswered = false;
    mJihadReadThread->message_loop()->PostTask(NewRunnableFunction(&JihadReadThreadInitTrampoline, this));
    const TimeStamp deadline = TimeStamp::Now() + TimeDuration::FromSeconds(5);
    while (!mJihadReadInitAnswered && TimeStamp::Now() < deadline) {
      lock.Wait(PR_MillisecondsToInterval(100));
    }
    answered = mJihadReadInitAnswered;
  }
  if (!answered || !mJihadReadInitOk) {
    fprintf(stderr, "[jihad-gpu] read thread: %s, reading on the compositor thread\n", answered ? "no shared context" : "no answer in 5 s");
    JihadStopReadThread();
    mJihadReadInitOk = false;
  }
  return mJihadReadInitOk;
}

void
CompositorOGL::JihadReadThreadInit()
{
  bool ok = false;
  nsCString fail;
  RefPtr<GLContextEGL> ctx;
  if (mGLContext && mGLContext->GetContextType() == GLContextType::EGL) {
    ctx = GLContextEGL::JihadCreateSharedContext(GLContextEGL::Cast(mGLContext), &fail);
  }
  if (ctx && ctx->MakeCurrent()) {
    ok = true;
    for (int t = 0; t < 2 && ok; ++t) {
      GLuint fbo = 0;
      ctx->fGenFramebuffers(1, &fbo);
      ctx->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, fbo);
      ctx->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_RENDERBUFFER, mJihadTargetRB[t]);
      GLenum status = ctx->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
      if (status != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[jihad-gpu] read thread: target %d incomplete on the shared context (0x%x)\n", t, status);
        ctx->fDeleteFramebuffers(1, &fbo);
        ok = false;
        break;
      }
      mJihadReadTargetFBO[t] = fbo;
    }
    ctx->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);
  } else {
    fprintf(stderr, "[jihad-gpu] read thread: shared context failed (%s)\n", fail.get());
  }
  const bool fence = sEGLLibrary.IsExtensionSupported(GLLibraryEGL::KHR_fence_sync);
  JIHAD_GPU_LOG("[jihad-gpu] read thread: %s%s\n", ok ? "up, shared context on both window targets" : "DOWN",
          ok ? (fence ? ", EGL fences" : ", no EGL fence: glFinish on the compositor thread") : "");
  MonitorAutoLock lock(mJihadReadMon);
  if (ok) mJihadReadCtx = ctx;
  mJihadReadInitOk = ok;
  mJihadReadInitAnswered = true;
  lock.NotifyAll();
}

void
CompositorOGL::JihadStopReadThread()
{
  if (!mJihadReadThread) {
    return;
  }
  mJihadReadThread->Stop();   // runs what is queued (the reads touch our members), then joins
  delete mJihadReadThread;
  mJihadReadThread = nullptr;
  mJihadReadCtx = nullptr;    // its thread is gone; the objects die with the share group
  mJihadReadTargetFBO[0] = mJihadReadTargetFBO[1] = 0;
  mJihadReadInitOk = false;
}

void
CompositorOGL::JihadSelectTarget()
{
  if (!mJihadTargetFBO[1]) {
    // a single target (the pass, or the pbuffer itself); after a band frame it is the window again
    if (mJihadTargetFBO[0]) mJihadWindowFBO = mJihadTargetFBO[0];
    return;
  }
  const int idx = mJihadCur;
  {
    // the read thread may still be reading this target (two frames ago): wait, bounded
    MonitorAutoLock lock(mJihadReadMon);
    const TimeStamp t0 = TimeStamp::Now();
    static uint32_t sWaits = 0;
    if (mJihadTargetBusy[idx] && sWaits++ < 40) JIHAD_GPU_LOG("[jihad-gpu] compositor thread: target %d still being read, waiting\n", idx);
    while (mJihadTargetBusy[idx]) {
      lock.Wait(PR_MillisecondsToInterval(20));
      if (TimeStamp::Now() - t0 > TimeDuration::FromSeconds(2)) {
        JIHAD_GPU_LOG("[jihad-gpu] target %d still being read after 2 s: taking it\n", idx);
        mJihadTargetBusy[idx] = 0;
        break;
      }
    }
  }
  mJihadWindowFBO = mJihadTargetFBO[idx];
}

// T-502: the rows kept from the slot's last frame, moved by the card's scroll since (the
// compositor applies that scroll to the content; the daemon keeps what it had and asks only
// for what entered and the fixed rows). Positive = scrolled down: row r of the new frame is row
// r + shift of the old one.
// Round 141: the kept rects (opaque stuck layers that held still) are the base frame's pixels at
// the same place, not shifted with the content.
static bool
JihadKeepRect(const JihadFrameRequest* aReq, int k, int* x0, int* x1, int* y0, int* y1)
{
  const int* q = aReq->keep[k];
  *x0 = std::max(0, q[0]); *x1 = std::min(aReq->w, q[1]);
  *y0 = std::max(0, q[2]); *y1 = std::min(aReq->h, q[3]);
  return *x1 > *x0 && *y1 > *y0;
}

static void
JihadShiftRows(JihadFrameRequest* aReq)
{
  const int h = aReq->h, dy = aReq->shift;
  if (aReq->src && aReq->dest && aReq->src != aReq->dest && aReq->stride == aReq->w * 4 && dy < h && dy > -h) {
    // round 113: from the newest slot, one contiguous copy (different segments, no overlap)
    const size_t stride = (size_t)aReq->stride;
    const int from = dy > 0 ? dy : 0, to = dy > 0 ? 0 : -dy, rows = h - (dy > 0 ? dy : -dy);
    memcpy(aReq->dest + (size_t)to * stride, aReq->src + (size_t)from * stride, (size_t)rows * stride);
    for (int k = 0; k < aReq->nKeep && dy; k++) {
      int x0, x1, y0, y1;
      if (!JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) continue;
      for (int r = y0; r < y1; r++) {
        memcpy(aReq->dest + (size_t)r * stride + (size_t)x0 * 4, aReq->src + (size_t)r * stride + (size_t)x0 * 4, (size_t)(x1 - x0) * 4);
      }
    }
    return;
  }
  if (!dy || !aReq->dest || aReq->stride != aReq->w * 4 || dy >= h || dy <= -h) {
    return;
  }
  // T-517: the daemon moved the frame's window by the shift (the destination starts dy rows further
  // into its segment than the slot's frame did): the rows are where the shift would have put them.
  // Only the kept rects (still layers) are copied, from where they were: dy rows back, the slot's
  // frame's own rows
  if (aReq->windowMoved) {
    // every rect saved before any is written: a rect's old rows may be another rect's new ones
    // (two stacked bars), and the rows read and written may overlap
    static uint8_t* sKept = nullptr;
    static size_t sKeptCap = 0;
    const size_t stride = (size_t)aReq->stride;
    size_t total = 0;
    for (int k = 0; k < aReq->nKeep; k++) {
      int x0, x1, y0, y1;
      if (JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) total += (size_t)(x1 - x0) * 4 * (y1 - y0);
    }
    if (!total) return;
    if (total > sKeptCap) { free(sKept); sKept = (uint8_t*)malloc(total); sKeptCap = sKept ? total : 0; }
    if (!sKept) return;
    uint8_t* p = sKept;
    for (int k = 0; k < aReq->nKeep; k++) {
      int x0, x1, y0, y1;
      if (!JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) continue;
      const size_t rowBytes = (size_t)(x1 - x0) * 4;
      for (int r = y0; r < y1; r++, p += rowBytes) memcpy(p, aReq->dest + ((ptrdiff_t)r - dy) * (ptrdiff_t)stride + (size_t)x0 * 4, rowBytes);
    }
    p = sKept;
    for (int k = 0; k < aReq->nKeep; k++) {
      int x0, x1, y0, y1;
      if (!JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) continue;
      const size_t rowBytes = (size_t)(x1 - x0) * 4;
      for (int r = y0; r < y1; r++, p += rowBytes) memcpy(aReq->dest + (size_t)r * stride + (size_t)x0 * 4, p, rowBytes);
    }
    return;
  }
  // in place: the kept rects saved first and put back after the shift
  static uint8_t* sSaved = nullptr;
  static size_t sSavedCap = 0;
  size_t savedBytes = 0;
  for (int k = 0; k < aReq->nKeep; k++) {
    int x0, x1, y0, y1;
    if (JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) savedBytes += (size_t)(x1 - x0) * 4 * (y1 - y0);
  }
  if (savedBytes > sSavedCap) {
    free(sSaved);
    sSaved = (uint8_t*)malloc(savedBytes);
    sSavedCap = sSaved ? savedBytes : 0;
  }
  const bool keepOk = savedBytes && sSaved;
  if (keepOk) {
    uint8_t* p = sSaved;
    for (int k = 0; k < aReq->nKeep; k++) {
      int x0, x1, y0, y1;
      if (!JihadKeepRect(aReq, k, &x0, &x1, &y0, &y1)) continue;
      for (int r = y0; r < y1; r++, p += (size_t)(x1 - x0) * 4) {
        memcpy(p, aReq->dest + (size_t)r * aReq->stride + (size_t)x0 * 4, (size_t)(x1 - x0) * 4);
      }
    }
  }
  struct Restore {
    JihadFrameRequest* req; bool on;
    ~Restore() {
      if (!on) return;
      uint8_t* p = sSaved;
      for (int k = 0; k < req->nKeep; k++) {
        int x0, x1, y0, y1;
        if (!JihadKeepRect(req, k, &x0, &x1, &y0, &y1)) continue;
        for (int r = y0; r < y1; r++, p += (size_t)(x1 - x0) * 4) {
          memcpy(req->dest + (size_t)r * req->stride + (size_t)x0 * 4, p, (size_t)(x1 - x0) * 4);
        }
      }
    }
  } restore{aReq, keepOk};
  // row by row with memcpy (a row's source and destination never overlap, the rows do): the
  // toolchain's memmove took 10-13 ms for the 2.9 MB, memcpy has the NEON path
  const size_t stride = (size_t)aReq->stride;
  if (dy > 0) {
    for (int r = 0; r < h - dy; r++) memcpy(aReq->dest + (size_t)r * stride, aReq->dest + (size_t)(r + dy) * stride, stride);
  } else {
    for (int r = h - 1; r >= -dy; r--) memcpy(aReq->dest + (size_t)r * stride, aReq->dest + (size_t)(r + dy) * stride, stride);
  }
}

void
CompositorOGL::JihadShiftRowsNow(JihadFrameRequest* aReq)
{
  if (aReq->cancelled) {
    return;   // T-509
  }
  JihadShiftRows(aReq);
}

void
CompositorOGL::JihadEnsureTargetSize(int aWidth, int aHeight)
{
  static const bool sPbTargets = getenv("JIHAD_GPU_PB_TARGETS") != nullptr;
  if (sPbTargets || aWidth <= 0 || aHeight <= 0 || !mJihadTargetRB[0]) {
    return;   // the pbuffer-sized targets (A/B), or the texture target of the swizzle pass
  }
  if (mJihadTargetSize == gfx::IntSize(aWidth, aHeight)) {
    return;
  }
  // the context's surface at the frame's size too (round 104; JIHAD_GPU_KEEP_PB=1 for A/B)
  static const bool sKeepPb = getenv("JIHAD_GPU_KEEP_PB") != nullptr;
  bool pbOk = false;
  if (!sKeepPb && mGLContext->GetContextType() == GLContextType::EGL) {
    pbOk = static_cast<GLContextEGL*>(mGLContext.get())->JihadUsePBufferOfSize(aWidth, aHeight);
    if (pbOk) mJihadPbSize = gfx::IntSize(aWidth, aHeight);
  }
  mGLContext->MakeCurrent();
  bool ok = true;
  for (int t = 0; t < 2; ++t) {
    if (!mJihadTargetRB[t]) continue;
    mGLContext->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, mJihadTargetRB[t]);
    mGLContext->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, aWidth, aHeight);
    mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadTargetFBO[t]);
    const GLenum st = mGLContext->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    if (st != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "[jihad-gpu] window target %d at %dx%d: incomplete (0x%x)\n", t, aWidth, aHeight, st);
      ok = false;
    }
    mGLContext->fDisable(LOCAL_GL_SCISSOR_TEST);
    mGLContext->fViewport(0, 0, aWidth, aHeight);
    mGLContext->fClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    mGLContext->fClear(LOCAL_GL_COLOR_BUFFER_BIT);
  }
  mGLContext->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, 0);
  mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadWindowFBO);
  fprintf(stderr, "[jihad-gpu] window targets %s %dx%d (were %dx%d)%s; the pbuffer %s\n", ok ? "re-sized to" : "FAILED at",
          aWidth, aHeight, mJihadTargetSize.width, mJihadTargetSize.height, "",
          pbOk ? "too" : sKeepPb ? "kept (JIHAD_GPU_KEEP_PB)" : "NOT (kept its size)");
  mJihadTargetSize = gfx::IntSize(aWidth, aHeight);
}

int
CompositorOGL::JihadBeginBandPass(int aWidth)
{
  static const bool sOff = getenv("JIHAD_GPU_BAND_SURFACE") && !strcmp(getenv("JIHAD_GPU_BAND_SURFACE"), "0");
  if (sOff || aWidth <= 0 || mGLContext->GetContextType() != GLContextType::EGL) {
    return 0;
  }
  GLContextEGL* egl = static_cast<GLContextEGL*>(mGLContext.get());
  if (!egl->JihadUseBandPBuffer(aWidth, JihadBandRows())) {
    return 0;
  }
  if (!mJihadBandFBO || mJihadBandW != aWidth) {
    if (!mJihadBandRB) mGLContext->fGenRenderbuffers(1, &mJihadBandRB);
    mGLContext->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, mJihadBandRB);
    mGLContext->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, aWidth, JihadBandRows());
    mGLContext->fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, 0);
    if (!mJihadBandFBO) mGLContext->fGenFramebuffers(1, &mJihadBandFBO);
    mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadBandFBO);
    mGLContext->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_RENDERBUFFER, mJihadBandRB);
    const GLenum st = mGLContext->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    if (st != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "[jihad-gpu] band target %dx%d incomplete (0x%x)\n", aWidth, JihadBandRows(), st);
      egl->JihadUseFramePBuffer();
      return 0;
    }
    mJihadBandW = aWidth;
    JIHAD_GPU_LOG("[jihad-gpu] band target %dx%d: FBO %u on its own pbuffer\n", aWidth, JihadBandRows(), mJihadBandFBO);
  }
  mJihadWindowFBO = mJihadBandFBO;
  return JihadBandRows();
}

void
CompositorOGL::JihadEndBandPass()
{
  // The band surface stays current: making it current costs 11-13 ms on the Adreno 220, going
  // back 0.3 ms (round 107), and a pan is band frame after band frame. A whole composite puts
  // the frame's surface back first (JihadUseFrameSurface).
  JihadSelectTarget();
}

void
CompositorOGL::JihadUseFrameSurface()
{
  if (mGLContext->GetContextType() == GLContextType::EGL) {
    static_cast<GLContextEGL*>(mGLContext.get())->JihadUseFramePBuffer();
  }
}

uint8_t*
CompositorOGL::JihadBandScratch(size_t aBytes)
{
  static size_t sHave = 0;
  if (!mJihadBandScratch || sHave < aBytes) {
    mJihadBandScratch = MakeUnique<uint8_t[]>(aBytes);
    sHave = aBytes;
  }
  return mJihadBandScratch.get();
}

bool
CompositorOGL::JihadReadAsync(JihadFrameRequest* aReq)
{
  if (aReq && aReq->cancelled) {
    return false;   // T-509: given up, dest untouched
  }
  if (!aReq || !aReq->done) {
    return false;
  }
  aReq->composed = true;
  if (!JihadEnsureReadThread()) {
    const TimeStamp t0 = TimeStamp::Now();
    if (aReq->shift || aReq->src) mGLContext->fFlush();   // the GPU starts the composite while the rows are copied
    JihadShiftRows(aReq);
    const TimeStamp t1 = TimeStamp::Now();
    aReq->ok = true;
    // T-511: the bands as one column (readX0..readX1) when the compositor found only it stale
    const int surfaceRows = std::min<int>(aReq->h, mWidgetSize.height);
    auto readBand = [&](int aLo, int aHi) {
      if (aReq->readX1 > aReq->readX0) {
        const int lo = std::max(0, aLo), hi = std::min(surfaceRows, aHi);
        return hi <= lo || JihadReadColumns(aReq->dest, aReq->stride, surfaceRows, aReq->readX0, aReq->readX1, lo, hi - lo, lo, aReq->readX0);
      }
      return JihadReadFrame(aReq->dest, aReq->stride, aReq->w, aReq->h, aLo, aHi);
    };
    if (aReq->hi > aReq->lo) aReq->ok = readBand(aReq->lo, aReq->hi);
    const TimeStamp t2 = TimeStamp::Now();
    if (aReq->ok && aReq->hi2 > aReq->lo2) aReq->ok = readBand(aReq->lo2, aReq->hi2);
    // round 138: the fixed and stuck layers' columns
    {
      const int surfaceH = std::min<int>(aReq->h, mWidgetSize.height);
      for (int c = 0; c < aReq->nCols && aReq->ok; c++) {
        const int* cc = aReq->cols[c];
        aReq->ok = JihadReadColumns(aReq->dest, aReq->stride, surfaceH, cc[0], cc[1], cc[2], cc[3] - cc[2], cc[2]);
      }
    }
    // JIHAD_GPU_DUMP_TARGET=<path>: the first time a band read comes back all white (three
    // samples a row), the whole target is read into <path> (raw BGRA, top-down) for a look
    static const char* sDumpTarget = getenv("JIHAD_GPU_DUMP_TARGET");
    static bool sDumpedTarget = false;
    if (sDumpTarget && !sDumpedTarget && aReq->ok && aReq->scrollY > 100.0f && aReq->hi - aReq->lo < aReq->h) {   // a pan's partial frame
      const int bands[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
      for (int b = 0; b < 2 && !sDumpedTarget; b++) {
        const int lo = bands[b][0], hi = bands[b][1];
        if (hi - lo < 8) continue;
        bool allWhite = true;
        for (int r = lo; r < hi && allWhite; r++) {
          const uint8_t* row = aReq->dest + (size_t)r * aReq->stride;
          for (int x : { aReq->w / 8, aReq->w / 2, aReq->w - aReq->w / 8 }) { const uint8_t* px = row + (size_t)x * 4; if (!(px[0] == 255 && px[1] == 255 && px[2] == 255)) { allWhite = false; break; } }
        }
        if (allWhite) {
          sDumpedTarget = true;
          uint8_t* whole = (uint8_t*)malloc((size_t)aReq->stride * aReq->h);
          if (whole && JihadReadFrame(whole, aReq->stride, aReq->w, aReq->h, 0, aReq->h)) {
            FILE* f = fopen(sDumpTarget, "wb");
            if (f) { fwrite(whole, 1, (size_t)aReq->stride * aReq->h, f); fclose(f); }
          }
          free(whole);
          JIHAD_GPU_LOG("[jihad-gpu] band %d..%d read back all white: the whole target dumped to %s (request: bands %d..%d %d..%d, shift %d, scroll %.1f,%.1f valid %d)\n",
                  lo, hi, sDumpTarget, aReq->lo, aReq->hi, aReq->lo2, aReq->hi2, aReq->shift, aReq->scrollX, aReq->scrollY, (int)aReq->scrollValid);
        }
      }
    }
    static uint32_t sN = 0;
    if (sN++ < 12 || (sN % 50) == 0) {
      JIHAD_GPU_LOG("[jihad-gpu] read: shift %d rows %.1f ms, band %d..%d %.1f ms, band %d..%d %.1f ms\n",
              aReq->shift, (t1 - t0).ToMilliseconds(), aReq->lo, aReq->hi, (t2 - t1).ToMilliseconds(),
              aReq->lo2, aReq->hi2, (TimeStamp::Now() - t2).ToMilliseconds());
    }
    aReq->done(aReq);
    return true;
  }
  aReq->target = mJihadCur;
  aReq->fence = nullptr;
  // JIHAD_GPU_NO_FENCE: wait the compose out here (glFinish) instead of an EGL fence the read
  // thread waits on; the device rounds compare the two (a fence wait that spins costs a core).
  static const bool sNoFence = getenv("JIHAD_GPU_NO_FENCE") != nullptr;
  if (!sNoFence && sEGLLibrary.IsExtensionSupported(GLLibraryEGL::KHR_fence_sync)) {
    aReq->fence = sEGLLibrary.fCreateSync(EGL_DISPLAY(), LOCAL_EGL_SYNC_FENCE, nullptr);
  }
  const TimeStamp tf0 = TimeStamp::Now();
  if (aReq->fence) {
    mGLContext->fFlush();
  } else {
    mGLContext->fFinish();
  }
  {
    static int sN = 0;
    if (sN++ < 12 || (sN % 100) == 0) {
      JIHAD_GPU_LOG("[jihad-gpu] compositor thread: %s %.1f ms\n", aReq->fence ? "fence+flush" : "glFinish",
              (TimeStamp::Now() - tf0).ToMilliseconds());
    }
  }
  {
    MonitorAutoLock lock(mJihadReadMon);
    mJihadTargetBusy[aReq->target] = 1;
  }
  mJihadCur ^= 1;
  mJihadReadThread->message_loop()->PostTask(NewRunnableFunction(&JihadReadThreadRunTrampoline, this, aReq));
  return true;
}

void
CompositorOGL::JihadReadOnReadThread(JihadFrameRequest* aReq)
{
  bool ok = false;
  const TimeStamp t0 = TimeStamp::Now();
  TimeStamp t1 = t0, t2 = t0;
  if (mJihadReadCtx && mJihadReadCtx->MakeCurrent()) {
    GLContext* gl = mJihadReadCtx;
    struct timespec cpu0, cpu1;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
    static uint32_t sReads = 0;
    const uint32_t readNo = ++sReads;
    if (readNo <= 40) JIHAD_GPU_LOG("[jihad-gpu] read thread: read %u of target %d, rows %d..%d, %s\n", readNo, aReq->target, aReq->lo, aReq->hi, aReq->fence ? "waiting on the fence" : "no fence");
    if (aReq->fence) {
      // bounded: a fence that never signals (2026-10-17, run 55: the first partial composite)
      // must not hang the pipeline; after the timeout the read goes ahead (the compositor
      // context was flushed, and the driver orders one process's command streams)
      const EGLint r = sEGLLibrary.fClientWaitSync(EGL_DISPLAY(), (EGLSync)aReq->fence, 0, 1000000000ull /* 1 s */);
      if (r != LOCAL_EGL_CONDITION_SATISFIED) fprintf(stderr, "[jihad-gpu] read thread: fence wait returned 0x%x after 1 s (read %u, target %d)\n", (unsigned)r, readNo, aReq->target);
      sEGLLibrary.fDestroySync(EGL_DISPLAY(), (EGLSync)aReq->fence);
      aReq->fence = nullptr;
    }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
    const double waitCpuMs = (cpu1.tv_sec - cpu0.tv_sec) * 1000.0 + (cpu1.tv_nsec - cpu0.tv_nsec) / 1e6;
    t1 = TimeStamp::Now();
    const int w = aReq->w, h = aReq->h;
    JihadShiftRows(aReq);
    const int bands[2][2] = { {aReq->lo, aReq->hi}, {aReq->lo2, aReq->hi2} };
    for (int b = 0; b < 2; b++) {
      int lo = bands[b][0], hi = bands[b][1];
      if (lo < 0) lo = 0;
      if (hi > h) hi = h;
      if (hi <= lo || !aReq->dest || aReq->stride != w * 4) continue;
      const int rows = hi - lo;
      uint8_t* band = aReq->dest + (size_t)lo * aReq->stride;
      gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadReadTargetFBO[aReq->target]);
      {
        // the flipped window view: frame row r is GL row h-1-r (see JihadReadFrame)
        ScopedPackState safePackState(gl);
        gl->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
        gl->fReadPixels(0, h - hi, w, rows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, band);
      }
      t2 = TimeStamp::Now();
      JihadSwizzleAndFlipRows(band, aReq->stride, w, rows, true);
      static int sTimed = 0;
      if (sTimed++ < 12 || (sTimed % 100) == 0) {
        JIHAD_GPU_LOG("[jihad-gpu] read thread: fence wait %.1f ms (%.1f ms of CPU), glReadPixels %d rows %.1f ms, swizzle+flip %.1f ms\n",
                (t1 - t0).ToMilliseconds(), waitCpuMs, rows, (t2 - t1).ToMilliseconds(), (TimeStamp::Now() - t2).ToMilliseconds());
      }
    }
    ok = true;
  }
  {
    MonitorAutoLock lock(mJihadReadMon);
    mJihadTargetBusy[aReq->target] = 0;
    lock.NotifyAll();
  }
  aReq->ok = ok;
  aReq->done(aReq);
}

bool
CompositorOGL::JihadReadFrame(uint8_t* aDest, int aStride, int aWidth, int aHeight, int aRowLo, int aRowHi)
{
  if (!mGLContext || !aDest || aWidth <= 0 || aHeight <= 0 || aStride != aWidth * 4) {
    return false;
  }
  const int w = std::min<int>(aWidth, mWidgetSize.width), h = std::min<int>(aHeight, mWidgetSize.height);
  if (w <= 0 || h <= 0) {
    return false;
  }
  // the rows to read, top-down: (0, 0) = every row
  int lo = aRowLo, hi = aRowHi;
  if (hi <= lo) { lo = 0; hi = h; }
  if (lo < 0) lo = 0;
  if (hi > h) hi = h;
  if (hi <= lo) {
    return true;
  }
  const int rows = hi - lo;
  uint8_t* band = aDest + (size_t)lo * aStride;
  mGLContext->MakeCurrent();
  static int sTimed = 0;
  TimeStamp t0 = TimeStamp::Now();
  if (JihadEnsureSwizzlePass(w, h)) {
    GLContext* gl = mGLContext;
    const bool blend = gl->fIsEnabled(LOCAL_GL_BLEND), scissor = gl->fIsEnabled(LOCAL_GL_SCISSOR_TEST);
    if (blend) gl->fDisable(LOCAL_GL_BLEND);
    if (scissor) gl->fDisable(LOCAL_GL_SCISSOR_TEST);
    gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadReadFBO);
    gl->fViewport(0, 0, w, h);
    gl->fUseProgram(mJihadSwizzleProg);
    gl->fActiveTexture(LOCAL_GL_TEXTURE0);
    gl->fBindTexture(LOCAL_GL_TEXTURE_2D, mJihadWindowTex);
    gl->fUniform1i(mJihadSwizzleTex, 0);
    gl->fUniform2f(mJihadSwizzleScale, float(w) / float(mJihadPbSize.width), float(h) / float(mJihadPbSize.height));
    static const GLfloat kQuad[8] = { -1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f };
    gl->fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);
    gl->fEnableVertexAttribArray(mJihadSwizzlePos);
    gl->fVertexAttribPointer(mJihadSwizzlePos, 2, LOCAL_GL_FLOAT, LOCAL_GL_FALSE, 0, kQuad);
    gl->fDrawArrays(LOCAL_GL_TRIANGLE_STRIP, 0, 4);
    gl->fDisableVertexAttribArray(mJihadSwizzlePos);
    gl->fBindTexture(LOCAL_GL_TEXTURE_2D, 0);
    TimeStamp t1 = TimeStamp::Now();
    {
      // the read target holds the frame top-down (the pass flipped it): its GL row r is frame row r
      ScopedPackState safePackState(gl);
      gl->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
      gl->fReadPixels(0, lo, w, rows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, band);
    }
    if (blend) gl->fEnable(LOCAL_GL_BLEND);
    if (scissor) gl->fEnable(LOCAL_GL_SCISSOR_TEST);
    gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadWindowFBO);
    if (sTimed++ < 12 || (sTimed % 100) == 0) {
      JIHAD_GPU_LOG("[jihad-gpu] swizzle pass %.1f ms (issued), glReadPixels %d rows %.1f ms\n",
              (t1 - t0).ToMilliseconds(), rows, (TimeStamp::Now() - t1).ToMilliseconds());
    }
    return true;
  }
  mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadWindowFBO);
  // JIHAD_GPU_READ_FINISH=1: a glFinish before the read (round 100's guess at the white rows,
  // which were the driver binning by the surface's size, round 104; 19 ms a whole frame, off)
  static const bool sReadFinish = getenv("JIHAD_GPU_READ_FINISH") != nullptr;
  if (sReadFinish) {
    mGLContext->fFinish();
  }
  TimeStamp tFin = TimeStamp::Now();
  {
    // The window view is flipped: frame row r is GL row h-1-r, so the band's GL rows are
    // [h-hi, h-lo), read bottom-up into the band's rows and then reversed in place.
    // Round 102 A/B: JIHAD_GPU_READ_SCRATCH=1 reads into private memory and copies;
    // JIHAD_GPU_READ_CHUNK=<rows> reads in chunks of that many rows.
    static const bool sScratch = getenv("JIHAD_GPU_READ_SCRATCH") != nullptr;
    static const int sChunk = getenv("JIHAD_GPU_READ_CHUNK") ? atoi(getenv("JIHAD_GPU_READ_CHUNK")) : 0;
    ScopedPackState safePackState(mGLContext);
    mGLContext->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
    if (sScratch) {
      uint8_t* tmp = JihadBandScratch((size_t)aStride * rows);
      mGLContext->fReadPixels(0, h - hi, w, rows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, tmp);
      memcpy(band, tmp, (size_t)aStride * rows);
    } else if (sChunk > 0) {
      for (int done = 0; done < rows; done += sChunk) {
        const int n = std::min(sChunk, rows - done);
        mGLContext->fReadPixels(0, h - hi + done, w, n, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, band + (size_t)done * aStride);
      }
    } else {
      mGLContext->fReadPixels(0, h - hi, w, rows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, band);
    }
    // JIHAD_GPU_READ_VERIFY=1: when frame row 700 (GL row h-1-700) of a whole read is white,
    // read the region again into private memory and say what the second read holds there
    static const bool sReadVerify = getenv("JIHAD_GPU_READ_VERIFY") != nullptr;
    if (sReadVerify && lo == 0 && hi == h && h > 700) {
      auto whiteAt = [&](const uint8_t* buf, int glRow) {
        const uint8_t* row = buf + (size_t)glRow * aStride;
        for (int x : { w / 8, w / 2, w - w / 8 }) { const uint8_t* px = row + (size_t)x * 4; if (!(px[0] == 255 && px[1] == 255 && px[2] == 255)) return false; }
        return true;
      };
      auto whiteRunFromBottom = [&](const uint8_t* buf) { int g = 0; while (g < rows && whiteAt(buf, g)) ++g; return g; };
      static uint32_t sChecked = 0, sWhite = 0, sSecondGood = 0;
      ++sChecked;
      if (whiteAt(band, h - 1 - 700)) {
        ++sWhite;
        uint8_t* again = (uint8_t*)malloc((size_t)aStride * rows);
        if (again) {
          mGLContext->fReadPixels(0, h - hi, w, rows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, again);
          const bool secondWhite = whiteAt(again, h - 1 - 700);
          if (!secondWhite) ++sSecondGood;
          JIHAD_GPU_LOG("[jihad-gpu] read verify: first read white from GL row 0 for %d rows; second read (private memory) %s, white for %d rows (white firsts %u of %u checked, second good %u)\n",
                  whiteRunFromBottom(band), secondWhite ? "WHITE too" : "HAS CONTENT", whiteRunFromBottom(again), sWhite, sChecked, sSecondGood);
          free(again);
        }
      }
    }
  }
  TimeStamp t1 = TimeStamp::Now();
  JihadSwizzleAndFlipRows(band, aStride, w, rows, true);
  {
    static const bool sDrawStats = getenv("JIHAD_GPU_DRAWSTATS") != nullptr;
    if (sDrawStats && lo == 0 && hi == h && h > 700) {
      const uint8_t* row = aDest + (size_t)700 * aStride;
      bool white = true;
      for (int x : { w / 8, w / 2, w - w / 8 }) { const uint8_t* px = row + (size_t)x * 4; if (!(px[0] == 255 && px[1] == 255 && px[2] == 255)) white = false; }
      {
        // the sequence of whole reads: W/C (white at row 700 or content) with the target's FBO
        static char sSeq[64]; static int sSeqN = 0;
        const int fboIdx = (mJihadWindowFBO == mJihadTargetFBO[0]) ? 0 : (mJihadWindowFBO == mJihadTargetFBO[1]) ? 1 : 9;
        bool magenta = true;
        for (int x : { w / 8, w / 2, w - w / 8 }) { const uint8_t* px = row + (size_t)x * 4; if (!(px[0] == 255 && px[1] == 0 && px[2] == 255)) magenta = false; }
        sSeq[sSeqN++] = white ? 'W' : magenta ? 'M' : 'C'; sSeq[sSeqN++] = (char)('0' + fboIdx);
        if (sSeqN >= 60) { sSeq[sSeqN] = 0; JIHAD_GPU_LOG("[jihad-gpu] read sequence (W/C + target): %s\n", sSeq); sSeqN = 0; }
      }
      static uint32_t sN = 0;
      static int sListedWhite = 0, sListedContent = 0;
      if (mJihadDraws >= 20 && ((white && sListedWhite < 2) || (!white && sListedContent < 1))) {
        if (white) ++sListedWhite; else ++sListedContent;
        for (int i = 0; i < std::min(mJihadDraws, 48); i++) {
          const JihadDrawRec& d = mJihadDrawRecs[i];
          JIHAD_GPU_LOG("[jihad-gpu] draw %s #%d: rect %.0f,%.0f %.0fx%.0f clip y %d..%d type %d colour %.2f,%.2f,%.2f,%.2f opacity %.2f\n",
                  white ? "W" : "C", i, d.x, d.y, d.w, d.h, d.cy, d.cb, d.type, d.r, d.g, d.b, d.a, d.opacity);
        }
      }
      if (white || (sN++ % 20) == 0) {
        JIHAD_GPU_LOG("[jihad-gpu] drawstats: row 700 %s; window draws %d (surface draws %d), clip bottom %d..%d, quad bottom max %d, viewport %dx%d; first draw: scissor raw %d,%d %dx%d cached %d,%d %dx%d, viewport raw %d,%d %dx%d cached %d,%d %dx%d\n",
                white ? "WHITE" : "content", mJihadDraws, mJihadSurfaceDraws, mJihadClipBottomMin == (1 << 30) ? -1 : mJihadClipBottomMin,
                mJihadClipBottomMax, mJihadQuadBottomMax, mViewportSize.width, mViewportSize.height,
                mJihadRawSc[0], mJihadRawSc[1], mJihadRawSc[2], mJihadRawSc[3], mJihadCacheSc[0], mJihadCacheSc[1], mJihadCacheSc[2], mJihadCacheSc[3],
                mJihadRawVp[0], mJihadRawVp[1], mJihadRawVp[2], mJihadRawVp[3], mJihadCacheVp[0], mJihadCacheVp[1], mJihadCacheVp[2], mJihadCacheVp[3]);
      }
    }
  }
  static const bool sTimelineRead = getenv("JIHAD_GPU_TIMELINE") != nullptr;   // T-515 device rounds: every read
  if (sTimed++ < 12 || (sTimed % 100) == 0 || sTimelineRead) {
    JIHAD_GPU_LOG("[jihad-gpu] glFinish %.1f ms, glReadPixels %d rows %.1f ms, CPU swizzle+flip %.1f ms\n",
            (tFin - t0).ToMilliseconds(), rows, (t1 - tFin).ToMilliseconds(), (TimeStamp::Now() - t1).ToMilliseconds());
  }
  return true;
}

bool
CompositorOGL::JihadReadColumns(uint8_t* aDest, int aStride, int aSurfaceH, int aX0, int aX1, int aSrcRowLo, int aRows, int aDestRowLo,
                                int aDestX)
{
  // Round 138: a stuck sidebar is ~540 rows but a fifth of the width; glReadPixels costs by the
  // pixel on this driver (gpu-bench section 10: a 170-wide column of 542 rows 23% of the full
  // rows), so the fixed and stuck layers are read as their columns. The view is flipped (target
  // row r is GL row aSurfaceH-1-r): read bottom-up into private memory, swizzled and reversed
  // there, then each row's span copied into the frame.
  const int w = aX1 - aX0;
  const int destX = aDestX < 0 ? aX0 : aDestX;
  if (!mGLContext || !aDest || w <= 0 || aRows <= 0 || aSrcRowLo < 0 || aSrcRowLo + aRows > aSurfaceH ||
      aX0 < 0 || aX1 * 4 > aStride || (destX + w) * 4 > aStride || aDestRowLo < 0) {
    return false;
  }
  static uint8_t* sBuf = nullptr;
  static size_t sCap = 0;
  const size_t bytes = (size_t)w * 4 * aRows;
  if (sCap < bytes) {
    free(sBuf);
    sBuf = (uint8_t*)malloc(bytes);
    sCap = sBuf ? bytes : 0;
    if (!sBuf) return false;
  }
  mGLContext->MakeCurrent();
  mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mJihadWindowFBO);
  const TimeStamp t0 = TimeStamp::Now();
  {
    ScopedPackState safePackState(mGLContext);
    mGLContext->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
    mGLContext->fReadPixels(aX0, aSurfaceH - (aSrcRowLo + aRows), w, aRows, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, sBuf);
  }
  const TimeStamp t1 = TimeStamp::Now();
  JihadSwizzleAndFlipRows(sBuf, w * 4, w, aRows, true);
  for (int r = 0; r < aRows; r++) {
    memcpy(aDest + (size_t)(aDestRowLo + r) * aStride + (size_t)destX * 4, sBuf + (size_t)r * w * 4, (size_t)w * 4);
  }
  static uint32_t sN = 0;
  if (sN++ < 12 || (sN % 100) == 0) {
    JIHAD_GPU_LOG("[jihad-gpu] column read x %d..%d, rows %d..%d (target rows from %d of %d): glReadPixels %.1f ms, swizzle+copy %.1f ms\n",
            aX0, aX1, aDestRowLo, aDestRowLo + aRows, aSrcRowLo, aSurfaceH, (t1 - t0).ToMilliseconds(), (TimeStamp::Now() - t1).ToMilliseconds());
  }
  return true;
}

void
CompositorOGL::CopyToTarget(DrawTarget* aTarget, const nsIntPoint& aTopLeft, const gfx::Matrix& aTransform)
{
  MOZ_ASSERT(aTarget);
  IntRect rect;
  if (mUseExternalSurfaceSize) {
    rect = IntRect(0, 0, mSurfaceSize.width, mSurfaceSize.height);
  } else {
    rect = IntRect(0, 0, mWidgetSize.width, mWidgetSize.height);
  }
  GLint width = rect.width;
  GLint height = rect.height;

  if ((int64_t(width) * int64_t(height) * int64_t(4)) > INT32_MAX) {
    NS_ERROR("Widget size too big - integer overflow!");
    return;
  }

  mGLContext->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);

  if (!mGLContext->IsGLES()) {
    // GLES2 promises that binding to any custom FBO will attach
    // to GL_COLOR_ATTACHMENT0 attachment point.
    mGLContext->fReadBuffer(LOCAL_GL_BACK);
  }

  // Jihad T-502: read the frame straight into the target's pixels. The generic road below reads
  // RGBA into a temporary surface, converts it to BGRA through Factory::CopyDataSourceSurface and
  // draws it flipped through cairo; and on an offscreen context (no view flip: the image's row 0
  // is at GL y = 0) that flip turned the frame upside down. Here: one glReadPixels of the rows
  // the target wants, then an in-place RGBA->BGRA swizzle, row 0 to row 0. (NEON where the build has it.)
  static const bool sJihadDirect = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
  if (sJihadDirect && mGLContext->IsOffscreen()) {
    uint8_t* data = nullptr; IntSize dsize; int32_t dstride = 0; SurfaceFormat dfmt;
    if (aTarget->LockBits(&data, &dsize, &dstride, &dfmt)) {
      const int w = std::min<int>(width, dsize.width), h = std::min<int>(height, dsize.height);
      TimeStamp t0 = TimeStamp::Now();
      bool okRead = (dstride == w * 4);
      if (okRead) {
        ScopedPackState safePackState(mGLContext);
        mGLContext->fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
        mGLContext->fReadPixels(0, 0, w, h, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, data);
      }
      TimeStamp t1 = TimeStamp::Now();
      if (okRead) {
        // the view is flipped (window convention, see PrepareViewport) whenever the frame has
        // a window FBO; a plain offscreen draw is not
        if (mJihadWindowFBO) {
          JihadSwizzleAndFlipRows(data, dstride, w, h, false);
        } else {
          for (int y = 0; y < h; y++) {
            uint8_t* p = data + (size_t)y * dstride;
            for (int x = 0; x < w; x++, p += 4) {
              const uint8_t r = p[0]; p[0] = p[2]; p[2] = r;   // memory R,G,B,A -> B,G,R,A
            }
          }
        }
      }
      aTarget->ReleaseBits(data);
      TimeStamp t2 = TimeStamp::Now();
      JIHAD_GPU_LOG("[jihad-gpu] direct read %dx%d (frame %dx%d): glReadPixels %.1f ms, swizzle %.1f ms%s\n",
              w, h, width, height, (t1 - t0).ToMilliseconds(), (t2 - t1).ToMilliseconds(),
              okRead ? "" : " (stride mismatch: not read)");
      if (okRead) {
        return;
      }
    }
  }

  RefPtr<DataSourceSurface> source =
        Factory::CreateDataSourceSurface(rect.Size(), gfx::SurfaceFormat::B8G8R8A8);
  if (NS_WARN_IF(!source)) {
    return;
  }

  // Jihad T-502: the two halves of the readback, timed (JIHAD_GPU_COMPOSITOR).
  static const bool sJihadGpuLog = getenv("JIHAD_GPU_COMPOSITOR") != nullptr;
  TimeStamp jihadT0 = TimeStamp::Now();
  ReadPixelsIntoDataSurface(mGLContext, source);
  TimeStamp jihadT1 = TimeStamp::Now();

  // Map from GL space to Cairo space and reverse the world transform.
  Matrix glToCairoTransform = aTransform;
  glToCairoTransform.Invert();
  glToCairoTransform.PreScale(1.0, -1.0);
  glToCairoTransform.PreTranslate(0.0, -height);

  glToCairoTransform.PostTranslate(-aTopLeft.x, -aTopLeft.y);

  Matrix oldMatrix = aTarget->GetTransform();
  aTarget->SetTransform(glToCairoTransform);
  Rect floatRect = Rect(rect.x, rect.y, rect.width, rect.height);
  aTarget->DrawSurface(source, floatRect, floatRect, DrawSurfaceOptions(), DrawOptions(1.0f, CompositionOp::OP_SOURCE));
  aTarget->SetTransform(oldMatrix);
  if (sJihadGpuLog) {
    TimeStamp jihadT2 = TimeStamp::Now();
    JIHAD_GPU_LOG("[jihad-gpu] copy-to-target %dx%d: glReadPixels(+convert) %.1f ms, flipped draw %.1f ms\n",
            width, height, (jihadT1 - jihadT0).ToMilliseconds(), (jihadT2 - jihadT1).ToMilliseconds());
  }
  aTarget->Flush();
}

void
CompositorOGL::Pause()
{
  // This was only used on Android
}

bool
CompositorOGL::Resume()
{
#if defined(MOZ_WIDGET_UIKIT)
  if (!gl() || gl()->IsDestroyed())
    return false;

  // RenewSurface internally calls MakeCurrent.
  return gl()->RenewSurface(GetWidget()->RealWidget());
#endif
  return true;
}

already_AddRefed<DataTextureSource>
CompositorOGL::CreateDataTextureSource(TextureFlags aFlags)
{
  return MakeAndAddRef<TextureImageTextureSourceOGL>(this, aFlags);
}

bool
CompositorOGL::SupportsPartialTextureUpdate()
{
  return CanUploadSubTextures(mGLContext);
}

int32_t
CompositorOGL::GetMaxTextureSize() const
{
  MOZ_ASSERT(mGLContext);
  GLint texSize = 0;
  mGLContext->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE,
                            &texSize);
  MOZ_ASSERT(texSize != 0);
  return texSize;
}

void
CompositorOGL::MakeCurrent(MakeCurrentFlags aFlags) {
  if (mDestroyed) {
    NS_WARNING("Call on destroyed layer manager");
    return;
  }
  mGLContext->MakeCurrent(aFlags & ForceMakeCurrent);
}

GLBlitTextureImageHelper*
CompositorOGL::BlitTextureImageHelper()
{
    if (!mBlitTextureImageHelper) {
        mBlitTextureImageHelper = MakeUnique<GLBlitTextureImageHelper>(this);
    }

    return mBlitTextureImageHelper.get();
}



GLuint
CompositorOGL::GetTemporaryTexture(GLenum aTarget, GLenum aUnit)
{
  if (!mTexturePool) {
    mTexturePool = new PerUnitTexturePoolOGL(gl());
  }
  return mTexturePool->GetTexture(aTarget, aUnit);
}

GLuint
PerUnitTexturePoolOGL::GetTexture(GLenum aTarget, GLenum aTextureUnit)
{
  if (mTextureTarget == 0) {
    mTextureTarget = aTarget;
  }
  MOZ_ASSERT(mTextureTarget == aTarget);

  size_t index = aTextureUnit - LOCAL_GL_TEXTURE0;
  // lazily grow the array of temporary textures
  if (mTextures.Length() <= index) {
    size_t prevLength = mTextures.Length();
    mTextures.SetLength(index + 1);
    for(unsigned int i = prevLength; i <= index; ++i) {
      mTextures[i] = 0;
    }
  }
  // lazily initialize the temporary textures
  if (!mTextures[index]) {
    if (!mGL->MakeCurrent()) {
      return 0;
    }
    mGL->fGenTextures(1, &mTextures[index]);
    mGL->fBindTexture(aTarget, mTextures[index]);
    mGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_WRAP_S, LOCAL_GL_CLAMP_TO_EDGE);
    mGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_WRAP_T, LOCAL_GL_CLAMP_TO_EDGE);
  }
  return mTextures[index];
}

void
PerUnitTexturePoolOGL::DestroyTextures()
{
  if (mGL && mGL->MakeCurrent()) {
    if (mTextures.Length() > 0) {
      mGL->fDeleteTextures(mTextures.Length(), &mTextures[0]);
    }
  }
  mTextures.SetLength(0);
}

void
PerFrameTexturePoolOGL::DestroyTextures()
{
  if (!mGL->MakeCurrent()) {
    return;
  }

  if (mUnusedTextures.Length() > 0) {
    mGL->fDeleteTextures(mUnusedTextures.Length(), &mUnusedTextures[0]);
    mUnusedTextures.Clear();
  }

  if (mCreatedTextures.Length() > 0) {
    mGL->fDeleteTextures(mCreatedTextures.Length(), &mCreatedTextures[0]);
    mCreatedTextures.Clear();
  }
}

GLuint
PerFrameTexturePoolOGL::GetTexture(GLenum aTarget, GLenum)
{
  if (mTextureTarget == 0) {
    mTextureTarget = aTarget;
  }

  // The pool should always use the same texture target because it is illegal
  // to change the target of an already exisiting gl texture.
  // If we need to use several targets, a pool with several sub-pools (one per
  // target) will have to be implemented.
  // At the moment this pool is only used with tiling on b2g so we always need
  // the same target.
  MOZ_ASSERT(mTextureTarget == aTarget);

  GLuint texture = 0;

  if (!mUnusedTextures.IsEmpty()) {
    // Try to reuse one from the unused pile first
    texture = mUnusedTextures[0];
    mUnusedTextures.RemoveElementAt(0);
  } else if (mGL->MakeCurrent()) {
    // There isn't one to reuse, create one.
    mGL->fGenTextures(1, &texture);
    mGL->fBindTexture(aTarget, texture);
    mGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_WRAP_S, LOCAL_GL_CLAMP_TO_EDGE);
    mGL->fTexParameteri(aTarget, LOCAL_GL_TEXTURE_WRAP_T, LOCAL_GL_CLAMP_TO_EDGE);
  }

  if (texture) {
    mCreatedTextures.AppendElement(texture);
  }

  return texture;
}

void
PerFrameTexturePoolOGL::EndFrame()
{
  if (!mGL->MakeCurrent()) {
    // this means the context got destroyed underneith us somehow, and the driver
    // already has destroyed the textures.
    mCreatedTextures.Clear();
    mUnusedTextures.Clear();
    return;
  }

  // Some platforms have issues unlocking Gralloc buffers even when they're
  // rebound.
  if (gfxPrefs::OverzealousGrallocUnlocking()) {
    mUnusedTextures.AppendElements(mCreatedTextures);
    mCreatedTextures.Clear();
  }

  // Delete unused textures
  for (size_t i = 0; i < mUnusedTextures.Length(); i++) {
    GLuint texture = mUnusedTextures[i];
    mGL->fDeleteTextures(1, &texture);
  }
  mUnusedTextures.Clear();

  // Move all created textures into the unused pile
  mUnusedTextures.AppendElements(mCreatedTextures);
  mCreatedTextures.Clear();
}

} // namespace layers
} // namespace mozilla
