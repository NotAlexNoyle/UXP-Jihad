/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GLCONTEXTEGL_H_
#define GLCONTEXTEGL_H_

#include "GLContext.h"
#include "GLLibraryEGL.h"

namespace mozilla {
namespace gl {

class GLContextEGL : public GLContext
{
    friend class TextureImageEGL;

    static already_AddRefed<GLContextEGL>
    CreateGLContext(CreateContextFlags flags,
                    const SurfaceCaps& caps,
                    GLContextEGL* shareContext,
                    bool isOffscreen,
                    EGLConfig config,
                    EGLSurface surface,
                    nsACString* const out_failureId);

public:
    MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(GLContextEGL, override)
    GLContextEGL(CreateContextFlags flags,
                 const SurfaceCaps& caps,
                 GLContext* shareContext,
                 bool isOffscreen,
                 EGLConfig config,
                 EGLSurface surface,
                 EGLContext context);

    ~GLContextEGL();

    virtual GLContextType GetContextType() const override { return GLContextType::EGL; }
    // Jihad T-502: upload 32-bit surfaces as BGRA where the driver takes them (the content is
    // BGRA already). On the Adreno 220 an RGBA upload of a 768x942 layer costs 35 ms and a
    // BGRA_EXT one 8 (tools/device/gpu-bench.c, 2026-10-17); as RGBA the CPU swizzled every
    // tile first and the driver swizzled it back.
    virtual GLenum GetPreferredARGB32Format() const override {
        return IsExtensionSupported(EXT_texture_format_BGRA8888) ? LOCAL_GL_BGRA : LOCAL_GL_RGBA;
    }

    // Jihad T-502: a 1x1 pbuffer context sharing aShare's objects (the compositor's
    // renderbuffers), made on and for the readback thread.
    static already_AddRefed<GLContextEGL>
    JihadCreateSharedContext(GLContextEGL* aShare, nsACString* const out_failureId);

    static GLContextEGL* Cast(GLContext* gl) {
        MOZ_ASSERT(gl->GetContextType() == GLContextType::EGL);
        return static_cast<GLContextEGL*>(gl);
    }

    static EGLSurface CreateSurfaceForWindow(nsIWidget* aWidget);

    static void DestroySurface(EGLSurface aSurface);

    bool Init() override;

    virtual bool IsDoubleBuffered() const override {
        return mIsDoubleBuffered;
    }

    void SetIsDoubleBuffered(bool aIsDB) {
        mIsDoubleBuffered = aIsDB;
    }

    virtual bool SupportsRobustness() const override {
        return sEGLLibrary.HasRobustness();
    }

    virtual bool IsANGLE() const override {
        return sEGLLibrary.IsANGLE();
    }

    virtual bool IsWARP() const override {
        return sEGLLibrary.IsWARP();
    }

    virtual bool BindTexImage() override;

    virtual bool ReleaseTexImage() override;

    void SetEGLSurfaceOverride(EGLSurface surf);

    virtual bool MakeCurrentImpl(bool aForce) override;

    virtual bool IsCurrent() override;

    virtual bool RenewSurface(nsIWidget* aWidget) override;

    virtual void ReleaseSurface() override;

    virtual bool SetupLookupFunction() override;

    virtual bool SwapBuffers() override;

    virtual void GetWSIInfo(nsCString* const out) const override;

    // hold a reference to the given surface
    // for the lifetime of this context.
    void HoldSurface(gfxASurface* aSurf);

    EGLSurface GetEGLSurface() const {
        return mSurface;
    }

    EGLDisplay GetEGLDisplay() const {
        return sEGLLibrary.Display();
    }

    bool BindTex2DOffscreen(GLContext* aOffscreen);
    void UnbindTex2DOffscreen(GLContext* aOffscreen);
    void BindOffscreenFramebuffer();

    static already_AddRefed<GLContextEGL>
    CreateEGLPBufferOffscreenContext(CreateContextFlags flags,
                                     const gfx::IntSize& size,
                                     const SurfaceCaps& minCaps,
                                     nsACString* const out_FailureId);

protected:
    friend class GLContextProviderEGL;

public:
    const EGLConfig  mConfig;
protected:
    EGLSurface mSurface;
public:
    const EGLContext mContext;
protected:
    EGLSurface mSurfaceOverride;
    RefPtr<gfxASurface> mThebesSurface;
    bool mBound;

    bool mIsPBuffer;
    bool mIsDoubleBuffered;
    bool mCanBindToTexture;
    bool mShareWithEGLImage;
    bool mOwnsContext;

    static EGLSurface CreatePBufferSurfaceTryingPowerOfTwo(EGLConfig config,
                                                           EGLenum bindToTextureFormat,
                                                           gfx::IntSize& pbsize);
};

} // namespace gl
} // namespace mozilla

#endif // GLCONTEXTEGL_H_
