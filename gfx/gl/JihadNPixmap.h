/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Jihad T-636 (JIHAD_GPU_NPIXMAP_TILES=1, off by default): CPU-painted tiles reach the GPU with no
// glTexImage2D. On the HP TouchPad (Adreno 220, webOS 3) a Palm native pixmap (libnapp.so's
// NPixmapPrivate, the kind WebKit's ContentLayerPalm uses) becomes a texture through
// eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR) + glEGLImageTargetTexture2DOES, and that texture samples
// the pixmap's CPU memory LIVE (gpu-bench section 20, 2026-11-11): bytes written in B,G,R,A order are
// drawn at once, with no upload call. A GL upload of a 256x256 tile costs 17-26 ms on this device; a
// row copy of the same tile is well under a millisecond. The pixmap is no render target (GPU writes
// never reach its memory), so it serves texture sources only.
//
// Everything here runs on the thread that owns the GL context (the compositor's).

#ifndef JIHADNPIXMAP_H_
#define JIHADNPIXMAP_H_

#include "GLTypes.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/gfx/Types.h"
#include "nsRegionFwd.h"

namespace mozilla {
namespace gfx {
class DataSourceSurface;
} // namespace gfx
namespace gl {

class GLContext;

// One native pixmap, its EGLImage and the GL texture bound to it. The three live and die together
// and are pooled together: binding a name to an image is the part whose cost is not known.
struct JihadNPixmap {
    GLContext* gl;          // the context the texture name belongs to
    GLuint texture;
    EGLImage image;
    void* object;           // the NPixmapPrivate (0x30 bytes in our own storage)
    uint8_t* data;          // the pixels' CPU address, B8G8R8A8/X8 rows of `stride` bytes
    int32_t stride;
    gfx::IntSize size;
    uint64_t lastBind;      // the draw sequence number of its last bind (0: never bound)
};

// The switch is on, nothing has failed in this process, and the size is one the path takes (the
// width a multiple of 32 pixels, so that the pixmap's pitch is the width; the memory cap not met).
bool JihadNPixmapWanted(const gfx::IntSize& aSize);

// A pixmap of exactly aSize from the pool, or a new one; aGL current. Null: use the GL upload (a
// failure to load or to create turns the path off for the process, with one line on stderr).
JihadNPixmap* JihadNPixmapTake(GLContext* aGL, const gfx::IntSize& aSize);

// Back to the pool, or destroyed when the pool is full. aGL is the current context, or null when
// there is none (the name then goes with its context).
void JihadNPixmapGive(GLContext* aGL, JihadNPixmap* aPixmap);

// The texture is about to be drawn with: the GPU may read the memory until the next sync.
void JihadNPixmapBound(JihadNPixmap* aPixmap);

// Copies aRegion (texture coordinates; the source pixel of (x, y) is aFrom + (x, y) in aSurf) into
// the pixmap, after waiting for the GPU if it may still be reading this pixmap. False: aSurf does
// not cover the region or is not B8G8R8A8/X8 (nothing written).
bool JihadNPixmapWrite(GLContext* aGL, JihadNPixmap* aPixmap, gfx::DataSourceSurface* aSurf,
                       const nsIntRegion& aRegion, const gfx::IntPoint& aFrom);

// A context going away: its pooled pixmaps are destroyed (aDelete: the names deleted too).
void JihadNPixmapDropPool(GLContext* aGL, bool aDelete);

// The A/B's other side: every GL upload's time, for the "upload totals" line.
void JihadNPixmapNoteGLUpload(double aMs);

} // namespace gl
} // namespace mozilla

#endif // JIHADNPIXMAP_H_
