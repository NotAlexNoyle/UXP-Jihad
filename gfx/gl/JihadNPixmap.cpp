/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Jihad T-636: Palm's native pixmaps as live texture sources (JihadNPixmap.h says why).
//
// libnapp.so is a device library (GCC 4.x, glibc 2.8) loaded into a glibc 2.23 process. That is the
// same load the Flash plugin already causes: libflashplayer.so and libWebKitLuna.so both name
// libnapp.so as DT_NEEDED, and the plugin scan dlopens Flash in the daemon itself. The calls made
// here stay clear of the cross-ABI traps: NPixmapPrivate takes its pixels from libdlmalloc.so's own
// heap (SysV segments removed at creation and mlock()ed; no sbrk, no named semaphore, no
// process-shared mutex), the object lives in storage of ours, and nothing of it is ever handed to
// operator delete. The shared kind (NPixmap::Create: a PIpcBuffer with two process-shared mutexes
// in its header) is the one that would cross the ABI line, and it is not used.
//
// The objects are ARM C++ ones: a member function is a C function whose first argument is `this`,
// so they are called through dlsym with the mangled names (read from the device's libnapp.so, as
// tools/device/gpu-bench-npixmap.h in Jihad Browser records: the layout, the vtable, the addresses).

#include "JihadNPixmap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

#include "GLContext.h"
#include "GLLibraryEGL.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/JihadGpuLog.h"
#include "nsRegion.h"

// The calling convention above holds on 32-bit ARM Linux alone; elsewhere the path does not exist.
#if defined(__arm__) && defined(__linux__) && !defined(ANDROID)
#define JIHAD_NPIXMAP_ABI 1
#include <dlfcn.h>
#endif

namespace mozilla {
namespace gl {

extern std::atomic<uint32_t> gJihadUploads;    // GLUploadHelpers.cpp (T-517): the timeline's counts
extern std::atomic<uint64_t> gJihadUploadUs;

namespace {

// What the calls return is cross-checked against the object's fields; never written.
struct NPixLayout {
    void* vptr; void* ipc; int32_t locks; int32_t size; int32_t offset; uint8_t lockFree; uint8_t pad[3];
    int32_t width; int32_t height; int32_t pitch; int32_t yuv; void* mem; int32_t extra;
};

struct NPixApi {
    void* (*privCtor)(void* self, int w, int h, int pitch);   // _ZN14NPixmapPrivateC1Eiii
    void* (*privVaddr)(void* self);                           // _ZN14NPixmapPrivate5VaddrEv
    void (*privFree)(void* self);                             // _ZN14NPixmapPrivate4FreeEv (dlfree; the object stays)
    EGLImage (*createImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);   // eglCreateImageKHR
    EGLBoolean (*destroyImage)(EGLDisplay, EGLImage);                                          // eglDestroyImageKHR
    void (*imageTargetTexture)(GLenum, EGLImage);             // glEGLImageTargetTexture2DOES
};

enum SyncMode { SyncFence, SyncFinish, SyncNone };

mozilla::StaticMutex sLock;        // the pool and the counters; the GL work itself is one thread's
std::vector<JihadNPixmap*> sPool;
NPixApi sApi;
bool sLoaded = false;
std::atomic<bool> sFailed(false);  // permanent for the process
size_t sLiveBytes = 0;             // attached and pooled
uint32_t sLive = 0;
uint64_t sBindSeq = 0;             // counts binds of pixmap textures
uint64_t sSyncedSeq = 0;           // every bind up to this one has been drawn by the GPU

// The totals of both paths, since the process began
uint32_t sGLUploads = 0, sPixUploads = 0, sSyncWaits = 0, sMade = 0, sReused = 0, sCapRefused = 0, sDestroyed = 0;
double sGLMs = 0.0, sPixMs = 0.0, sSyncMs = 0.0, sCopyMs = 0.0, sMadeMs = 0.0;
uint64_t sPixPixels = 0;

int Mode()
{
    static const int mode = getenv("JIHAD_GPU_NPIXMAP_TILES") ? atoi(getenv("JIHAD_GPU_NPIXMAP_TILES")) : 0;
    return mode;
}

SyncMode Sync()
{
    // JIHAD_GPU_NPIXMAP_SYNC=finish|none: glFinish in the fence's place, or no wait at all (the
    // A/B's ceiling; a tile rewritten under a draw the GPU has not run yet may tear)
    static const SyncMode mode = !getenv("JIHAD_GPU_NPIXMAP_SYNC") ? SyncFence
                               : !strcmp(getenv("JIHAD_GPU_NPIXMAP_SYNC"), "none") ? SyncNone
                               : !strcmp(getenv("JIHAD_GPU_NPIXMAP_SYNC"), "finish") ? SyncFinish : SyncFence;
    return mode;
}

size_t CapBytes()
{
    // The pixels are mlock()ed shared memory: a cap on what the path may pin (JIHAD_GPU_NPIXMAP_MB,
    // 48: 180 tiles of 256x256). Past it a texture takes the GL upload.
    static const size_t cap = (size_t)(getenv("JIHAD_GPU_NPIXMAP_MB") ? atoi(getenv("JIHAD_GPU_NPIXMAP_MB")) : 48) << 20;
    return cap;
}

size_t PoolCap()
{
    static const size_t cap = getenv("JIHAD_GPU_NPIXMAP_POOL") ? (size_t)atoi(getenv("JIHAD_GPU_NPIXMAP_POOL")) : 48;
    return cap;
}

// NPixmap::Calc: the pitch is the width rounded up to 32 pixels, times four bytes.
int32_t PitchFor(int32_t aWidth) { return ((aWidth + 31) & ~31) * 4; }

void Fail(const char* aWhy, const char* aDetail = nullptr, unsigned aCode = 0)
{
    if (!sFailed.exchange(true)) {
        fprintf(stderr, "[jihad-gpu] npixmap tiles OFF for this process, GL uploads from here on: %s%s%s (0x%x)\n",
                aWhy, aDetail ? ": " : "", aDetail ? aDetail : "", aCode);
    }
}

bool Load()
{
    if (sLoaded) {
        return true;
    }
#ifdef JIHAD_NPIXMAP_ABI
    void* napp = dlopen("/usr/lib/libnapp.so", RTLD_NOW | RTLD_LOCAL);
    if (!napp) napp = dlopen("libnapp.so", RTLD_NOW | RTLD_LOCAL);
    if (!napp) {
        Fail("libnapp.so does not load", dlerror());
        return false;
    }
    // The handles the engine already holds (GLLibraryEGL, GLContextProviderEGL), taken once more
    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gles) {
        Fail("libEGL.so or libGLESv2.so does not load", dlerror());
        return false;
    }
#define JIHAD_NPIX_SYM(lib, field, name) \
    do { *(void**)&sApi.field = dlsym(lib, name); if (!sApi.field) { Fail("missing symbol", name); return false; } } while (0)
    JIHAD_NPIX_SYM(napp, privCtor, "_ZN14NPixmapPrivateC1Eiii");
    JIHAD_NPIX_SYM(napp, privVaddr, "_ZN14NPixmapPrivate5VaddrEv");
    JIHAD_NPIX_SYM(napp, privFree, "_ZN14NPixmapPrivate4FreeEv");
    JIHAD_NPIX_SYM(egl, createImage, "eglCreateImageKHR");
    JIHAD_NPIX_SYM(egl, destroyImage, "eglDestroyImageKHR");
    JIHAD_NPIX_SYM(gles, imageTargetTexture, "glEGLImageTargetTexture2DOES");
#undef JIHAD_NPIX_SYM
    sLoaded = true;
    JIHAD_GPU_LOG("[jihad-gpu] npixmap tiles: libnapp.so loaded; sync %s, %s; cap %zu MB, pool %zu\n",
                  Sync() == SyncNone ? "none" : Sync() == SyncFinish ? "glFinish" : "fence",
                  sEGLLibrary.IsExtensionSupported(GLLibraryEGL::KHR_fence_sync) ? "EGL_KHR_fence_sync present" : "no EGL_KHR_fence_sync (glFinish)",
                  CapBytes() >> 20, PoolCap());
    return true;
#else
    Fail("not a 32-bit ARM Linux build");
    return false;
#endif
}

// Every draw issued so far is done before this returns (or the mode says not to wait).
double WaitForGPU(GLContext* aGL)
{
    const uint64_t seq = sBindSeq;
    const TimeStamp t0 = TimeStamp::Now();
    if (Sync() != SyncNone) {
        bool waited = false;
        if (Sync() == SyncFence && sEGLLibrary.IsExtensionSupported(GLLibraryEGL::KHR_fence_sync)) {
            EGLSync sync = sEGLLibrary.fCreateSync(EGL_DISPLAY(), LOCAL_EGL_SYNC_FENCE, nullptr);
            if (sync) {
                waited = sEGLLibrary.fClientWaitSync(EGL_DISPLAY(), sync, LOCAL_EGL_SYNC_FLUSH_COMMANDS_BIT,
                                                     1000000000ull) == LOCAL_EGL_CONDITION_SATISFIED;
                sEGLLibrary.fDestroySync(EGL_DISPLAY(), sync);
            }
        }
        if (!waited) {
            aGL->fFinish();
        }
    }
    sSyncedSeq = seq;
    const double ms = (TimeStamp::Now() - t0).ToMilliseconds();
    mozilla::StaticMutexAutoLock lock(sLock);
    sSyncWaits++;
    sSyncMs += ms;
    return ms;
}

// aGL current, or null (no context left: the name goes with it). The GPU is waited for first: the
// driver maps the pixels, and they must not go while a draw still reads them.
void Destroy(GLContext* aGL, JihadNPixmap* aPixmap)
{
    if (aGL) {
        if (aPixmap->lastBind > sSyncedSeq) {
            WaitForGPU(aGL);
        }
        aGL->fDeleteTextures(1, &aPixmap->texture);
    }
    sApi.destroyImage(EGL_DISPLAY(), aPixmap->image);
    sApi.privFree(aPixmap->object);
    free(aPixmap->object);
    {
        mozilla::StaticMutexAutoLock lock(sLock);
        sLiveBytes -= (size_t)aPixmap->stride * aPixmap->size.height;
        sLive--;
        sDestroyed++;
    }
    delete aPixmap;
}

void TotalsLocked()
{
    JIHAD_GPU_LOG("[jihad-gpu] upload totals: GL %u uploads, %.2f ms each (%.0f ms); npixmap %u uploads, %.2f ms each (%.0f ms: "
                  "%u GPU waits %.2f ms each, copy %.2f ms each, %.0f kpx each); pixmaps: %u made (%.2f ms each), %u reused, "
                  "%u destroyed, %zu pooled, %u live (%.1f MB), %u refused by the cap\n",
                  sGLUploads, sGLUploads ? sGLMs / sGLUploads : 0.0, sGLMs,
                  sPixUploads, sPixUploads ? sPixMs / sPixUploads : 0.0, sPixMs,
                  sSyncWaits, sSyncWaits ? sSyncMs / sSyncWaits : 0.0, sPixUploads ? sCopyMs / sPixUploads : 0.0,
                  sPixUploads ? sPixPixels / 1000.0 / sPixUploads : 0.0,
                  sMade, sMade ? sMadeMs / sMade : 0.0, sReused, sDestroyed, sPool.size(), sLive, sLiveBytes / 1048576.0,
                  sCapRefused);
}

} // namespace

bool
JihadNPixmapWanted(const gfx::IntSize& aSize)
{
    if (Mode() <= 0 || sFailed) {
        return false;
    }
    // Widths of 32 pixels' multiples alone (every tile is one): the pitch is then the width, the
    // one case the device run covers.
    return aSize.width >= 32 && aSize.height >= 32 && aSize.width <= 2048 && aSize.height <= 2048 &&
           (aSize.width & 31) == 0;
}

JihadNPixmap*
JihadNPixmapTake(GLContext* aGL, const gfx::IntSize& aSize)
{
    if (!JihadNPixmapWanted(aSize)) {
        return nullptr;
    }
    const int32_t pitch = PitchFor(aSize.width);
    const size_t bytes = (size_t)pitch * aSize.height;
    {
        mozilla::StaticMutexAutoLock lock(sLock);
        for (size_t i = sPool.size(); i-- > 0;) {
            if (sPool[i]->gl == aGL && sPool[i]->size == aSize) {
                JihadNPixmap* pixmap = sPool[i];
                sPool.erase(sPool.begin() + i);
                sReused++;
                return pixmap;
            }
        }
        if (sLiveBytes + bytes > CapBytes()) {
            sCapRefused++;
            return nullptr;
        }
    }
    if (!Load()) {
        return nullptr;
    }
    const TimeStamp t0 = TimeStamp::Now();
    void* object = calloc(1, 64);   // the object is 0x30 bytes
    if (!object) {
        return nullptr;
    }
    sApi.privCtor(object, aSize.width, aSize.height, pitch);
    const NPixLayout* layout = static_cast<const NPixLayout*>(object);
    if (!layout->mem) {
        free(object);
        Fail("NPixmapPrivate: dlmalloc returned NULL");
        return nullptr;
    }
    uint8_t* data = static_cast<uint8_t*>(sApi.privVaddr(object));
    if (!data || layout->width != aSize.width || layout->height != aSize.height || layout->pitch != pitch) {
        sApi.privFree(object);
        free(object);
        Fail("NPixmapPrivate: the object's layout is not the one read from libnapp.so");
        return nullptr;
    }
    static const EGLint sAttrs[] = { LOCAL_EGL_IMAGE_PRESERVED_KHR, 0, LOCAL_EGL_NONE };
    EGLImage image = sApi.createImage(EGL_DISPLAY(), LOCAL_EGL_NO_CONTEXT, LOCAL_EGL_NATIVE_PIXMAP_KHR,
                                      (EGLClientBuffer)object, sAttrs);
    if (!image) {
        const unsigned err = (unsigned)sEGLLibrary.fGetError();
        sApi.privFree(object);
        free(object);
        Fail("eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR) gave no image", nullptr, err);
        return nullptr;
    }
    GLuint texture = 0;
    aGL->fGetError();   // an earlier call's error is not this path's
    aGL->fGenTextures(1, &texture);
    aGL->fActiveTexture(LOCAL_GL_TEXTURE0);
    aGL->fBindTexture(LOCAL_GL_TEXTURE_2D, texture);
    sApi.imageTargetTexture(LOCAL_GL_TEXTURE_2D, image);
    const GLenum err = aGL->fGetError();
    if (!texture || err) {
        aGL->fDeleteTextures(1, &texture);
        sApi.destroyImage(EGL_DISPLAY(), image);
        sApi.privFree(object);
        free(object);
        Fail("glEGLImageTargetTexture2DOES failed", nullptr, (unsigned)err);
        return nullptr;
    }
    JihadNPixmap* pixmap = new JihadNPixmap{aGL, texture, image, object, data, pitch, aSize, 0};
    const double ms = (TimeStamp::Now() - t0).ToMilliseconds();
    mozilla::StaticMutexAutoLock lock(sLock);
    sLiveBytes += bytes;
    sLive++;
    sMade++;
    sMadeMs += ms;
    if (sMade <= 20 || ms >= 8.0) {
        if (sMade < 300) JIHAD_GPU_LOG("[jihad-gpu] npixmap made %dx%d: object %p, pixels %p, pitch %d, image %p, texture %u: %.2f ms\n",
                                       aSize.width, aSize.height, object, data, pitch, image, texture, ms);
    }
    return pixmap;
}

void
JihadNPixmapGive(GLContext* aGL, JihadNPixmap* aPixmap)
{
    if (!aPixmap) {
        return;
    }
    if (aGL && aGL == aPixmap->gl) {
        mozilla::StaticMutexAutoLock lock(sLock);
        if (sPool.size() < PoolCap()) {
            sPool.push_back(aPixmap);
            return;
        }
    }
    Destroy(aGL, aPixmap);
}

void
JihadNPixmapBound(JihadNPixmap* aPixmap)
{
    aPixmap->lastBind = ++sBindSeq;
}

bool
JihadNPixmapWrite(GLContext* aGL, JihadNPixmap* aPixmap, gfx::DataSourceSurface* aSurf,
                  const nsIntRegion& aRegion, const gfx::IntPoint& aFrom)
{
    const gfx::SurfaceFormat format = aSurf->GetFormat();
    if (format != gfx::SurfaceFormat::B8G8R8A8 && format != gfx::SurfaceFormat::B8G8R8X8) {
        return false;
    }
    const gfx::IntRect bounds = aRegion.GetBounds();
    const gfx::IntRect src = bounds + aFrom;
    if (!gfx::IntRect(gfx::IntPoint(0, 0), aPixmap->size).Contains(bounds) ||
        !gfx::IntRect(gfx::IntPoint(0, 0), aSurf->GetSize()).Contains(src)) {
        return false;
    }
    const TimeStamp t0 = TimeStamp::Now();
    // The GPU reads these pixels where it stands: a draw issued and not yet run would show the new
    // content, in part. So the first write after a bind waits for the draws issued so far (a frame
    // that ended in a readback or a swap has run them already, and the fence is met at once).
    const double syncMs = aPixmap->lastBind > sSyncedSeq ? WaitForGPU(aGL) : 0.0;
    const TimeStamp t1 = TimeStamp::Now();
    const uint8_t* from = aSurf->GetData();
    const int32_t fromStride = aSurf->Stride();
    uint32_t pixels = 0, rects = 0;
    for (auto iter = aRegion.RectIter(); !iter.Done(); iter.Next()) {
        const gfx::IntRect& r = iter.Get();
        const uint8_t* s = from + (size_t)(r.y + aFrom.y) * fromStride + (size_t)(r.x + aFrom.x) * 4;
        uint8_t* d = aPixmap->data + (size_t)r.y * aPixmap->stride + (size_t)r.x * 4;
        if (r.width * 4 == fromStride && fromStride == aPixmap->stride) {
            memcpy(d, s, (size_t)fromStride * r.height);   // whole rows of one pitch: one copy
        } else {
            for (int32_t y = 0; y < r.height; y++, s += fromStride, d += aPixmap->stride) {
                memcpy(d, s, (size_t)r.width * 4);
            }
        }
        pixels += (uint32_t)(r.width * r.height);
        rects++;
    }
    const TimeStamp t2 = TimeStamp::Now();
    const double copyMs = (t2 - t1).ToMilliseconds(), all = (t2 - t0).ToMilliseconds();
    ++gJihadUploads;
    gJihadUploadUs += (uint64_t)(all * 1000.0);
    mozilla::StaticMutexAutoLock lock(sLock);
    sPixUploads++;
    sPixMs += all;
    sCopyMs += copyMs;
    sPixPixels += pixels;
    if (sPixUploads <= 40 || all >= 8.0) {
        if (sPixUploads < 400) JIHAD_GPU_LOG("[jihad-gpu] npixmap upload %dx%d: GPU wait %.2f ms, copy %.2f ms (%u px in %u rects), all %.2f ms\n",
                                             aPixmap->size.width, aPixmap->size.height, syncMs, copyMs, pixels, rects, all);
    }
    if ((sGLUploads + sPixUploads) % 100 == 0) {
        TotalsLocked();
    }
    return true;
}

void
JihadNPixmapDropPool(GLContext* aGL, bool aDelete)
{
    std::vector<JihadNPixmap*> mine;
    {
        mozilla::StaticMutexAutoLock lock(sLock);
        for (size_t i = 0; i < sPool.size();) {
            if (sPool[i]->gl == aGL) {
                mine.push_back(sPool[i]);
                sPool.erase(sPool.begin() + i);
            } else {
                ++i;
            }
        }
    }
    const bool current = aDelete && !mine.empty() && aGL && aGL->MakeCurrent();
    for (JihadNPixmap* pixmap : mine) {
        Destroy(current ? aGL : nullptr, pixmap);
    }
}

void
JihadNPixmapNoteGLUpload(double aMs)
{
    mozilla::StaticMutexAutoLock lock(sLock);
    sGLUploads++;
    sGLMs += aMs;
    if ((sGLUploads + sPixUploads) % 100 == 0) {
        TotalsLocked();
    }
}

} // namespace gl
} // namespace mozilla
