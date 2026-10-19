/* -*- Mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; tab-width: 40; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GLTextureImage.h"
#include <utility>   // Jihad: std::swap
#include <vector>
#include "mozilla/TimeStamp.h"
#include "mozilla/StaticMutex.h"
#include "GLContext.h"
#include "gfxContext.h"
#include "gfxPlatform.h"
#include "gfxUtils.h"
#include "gfx2DGlue.h"
#include "mozilla/gfx/2D.h"
#include "ScopedGLHelpers.h"
#include "GLUploadHelpers.h"
#include "GfxTexturesReporter.h"

#include "TextureImageEGL.h"

#include "mozilla/gfx/JihadGpuLog.h"   // Jihad (T-512): JIHAD_GPU_LOG

using namespace mozilla::gfx;

namespace mozilla {
namespace gl {

already_AddRefed<TextureImage>
CreateTextureImage(GLContext* gl,
                   const gfx::IntSize& aSize,
                   TextureImage::ContentType aContentType,
                   GLenum aWrapMode,
                   TextureImage::Flags aFlags,
                   TextureImage::ImageFormat aImageFormat)
{
    switch (gl->GetContextType()) {
        case GLContextType::EGL:
            return CreateTextureImageEGL(gl, aSize, aContentType, aWrapMode, aFlags, aImageFormat);
        default: {
            GLint maxTextureSize;
            gl->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, &maxTextureSize);
            if (aSize.width > maxTextureSize || aSize.height > maxTextureSize) {
              NS_ASSERTION(aWrapMode == LOCAL_GL_CLAMP_TO_EDGE, "Can't support wrapping with tiles!");
              return CreateTiledTextureImage(gl, aSize, aContentType, aFlags, aImageFormat);
            } else {
              return CreateBasicTextureImage(gl, aSize, aContentType, aWrapMode, aFlags);
            }
        }
    }
}


static already_AddRefed<TextureImage>
TileGenFunc(GLContext* gl,
            const IntSize& aSize,
            TextureImage::ContentType aContentType,
            TextureImage::Flags aFlags,
            TextureImage::ImageFormat aImageFormat)
{
    switch (gl->GetContextType()) {
        case GLContextType::EGL:
            return TileGenFuncEGL(gl, aSize, aContentType, aFlags, aImageFormat);
        default:
            return CreateBasicTextureImage(gl, aSize, aContentType,
                                           LOCAL_GL_CLAMP_TO_EDGE, aFlags);
    }
}

already_AddRefed<TextureImage>
TextureImage::Create(GLContext* gl,
                     const gfx::IntSize& size,
                     TextureImage::ContentType contentType,
                     GLenum wrapMode,
                     TextureImage::Flags flags)
{
    return CreateTextureImage(gl, size, contentType, wrapMode, flags);
}

bool
TextureImage::UpdateFromDataSource(gfx::DataSourceSurface* aSurface,
                                   const nsIntRegion* aDestRegion,
                                   const gfx::IntPoint* aSrcPoint)
{
    nsIntRegion destRegion = aDestRegion ? *aDestRegion
                                         : IntRect(0, 0,
                                                     aSurface->GetSize().width,
                                                     aSurface->GetSize().height);
    gfx::IntPoint srcPoint = aSrcPoint ? *aSrcPoint
                                       : gfx::IntPoint(0, 0);
    return DirectUpdate(aSurface, destRegion, srcPoint);
}

gfx::IntRect TextureImage::GetTileRect() {
    return gfx::IntRect(gfx::IntPoint(0,0), mSize);
}

gfx::IntRect TextureImage::GetSrcTileRect() {
    return GetTileRect();
}

void
TextureImage::UpdateUploadSize(size_t amount)
{
    if (mUploadSize > 0) {
        GfxTexturesReporter::UpdateAmount(GfxTexturesReporter::MemoryFreed, mUploadSize);
    }
    mUploadSize = amount;
    GfxTexturesReporter::UpdateAmount(GfxTexturesReporter::MemoryAllocated, mUploadSize);
}

BasicTextureImage::~BasicTextureImage()
{
    GLContext* ctx = mGLContext;
    if (ctx->IsDestroyed() || !ctx->IsOwningThreadCurrent()) {
        ctx = ctx->GetSharedContext();
    }

    // If we have a context, then we need to delete the texture;
    // if we don't have a context (either real or shared),
    // then they went away when the contex was deleted, because it
    // was the only one that had access to it.
    if (ctx && ctx->MakeCurrent()) {
        // Jihad T-504: a texture with storage goes to the pool instead (JihadPoolTexture)
        if (ctx == mGLContext && !mTexture2 && mTextureState != Created &&
            JihadPoolTexture(ctx, mTexture, mSize)) {
            return;
        }
        ctx->fDeleteTextures(1, &mTexture);
        if (mTexture2) {
            ctx->fDeleteTextures(1, &mTexture2);
        }
    }
}

// ---- Jihad T-504: the GPU path's tile textures are kept and reused, not deleted -----------------
//
// On the TouchPad's Adreno 220 the first write into a texture after its first draw costs ~18 ms for
// 256x256 (~75 ms for 512x512, ~285 ms for 1024x1024: the driver converts the texture's storage at
// about 14 MB/s), whatever the format, size parity, filter or wrap; every write after that costs
// ~1 ms, under a queued draw that reads the texture too; a deleted and regenerated name pays again
// (tools/device/gpu-bench.c sections 12 and 13, 2026-10-18). The compositor deletes a tile's texture
// with the tile, so every new tile paid the 18 ms on its first change: on desktop Wikipedia ~50 a
// zoom, a second of the compositor thread with no frame (the daemon logged 59 re-uploads at 37 ms
// average, the first uploads 1.5 ms). So on the GPU path the textures of BasicTextureImages that had
// storage go to this pool, up to JIHAD_GPU_TEX_POOL (48: 12 MB of 256x256 tiles), and the next
// texture image takes one, whatever its size: its uploads are the fast kind. A texture the pool
// cannot give is converted while it is 1x1 (JihadConvertTexture; JIHAD_GPU_NO_TEX_CONVERT=1 skips
// it for an A/B).
namespace {
struct JihadPooledTexture {
    GLContext* gl;
    GLuint tex;
    gfx::IntSize size;
};
mozilla::StaticMutex sJihadPoolLock;
std::vector<JihadPooledTexture> sJihadPool;
size_t JihadPoolCap()
{
    static const size_t cap = !getenv("JIHAD_GPU_COMPOSITOR") ? 0
                            : getenv("JIHAD_GPU_TEX_POOL") ? (size_t)atoi(getenv("JIHAD_GPU_TEX_POOL")) : 48;
    return cap;
}
uint32_t sJihadPoolHits = 0, sJihadPoolMisses = 0, sJihadPoolKept = 0, sJihadPoolFull = 0;
GLContext* sJihadConvertGL = nullptr;
GLuint sJihadConvertFb = 0;
uint32_t sJihadConverted = 0;
double sJihadConvertMs = 0.0;

// A new texture converted while it is 1x1: a 1x1 image, a clear of it through a framebuffer, a 1x1
// write. The conversion stays when the texture is re-specified at any size, and costs ~1-3 ms
// against ~18 for a 256x256 tile's first write after its first draw (gpu-bench.c sections 14-15:
// a draw that samples the texture converts it only once the draw has finished; a clear does
// without a finish). The scissor test is off for the clear and the framebuffer binding restored.
void JihadConvertTexture(GLContext* aGL, GLuint aTexture)
{
    const mozilla::TimeStamp t0 = mozilla::TimeStamp::Now();
    if (sJihadConvertGL != aGL) {
        sJihadConvertGL = aGL;
        sJihadConvertFb = 0;
        aGL->fGenFramebuffers(1, &sJihadConvertFb);
    }
    if (!sJihadConvertFb) {
        return;
    }
    const uint32_t px = 0xffffffff;
    ScopedBindTexture bind(aGL, aTexture);
    aGL->fTexImage2D(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_BGRA, 1, 1, 0, LOCAL_GL_BGRA, LOCAL_GL_UNSIGNED_BYTE, &px);
    {
        ScopedBindFramebuffer fb(aGL, sJihadConvertFb);
        ScopedGLState scissor(aGL, LOCAL_GL_SCISSOR_TEST, false);
        aGL->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_TEXTURE_2D, aTexture, 0);
        static int sStatusLogged = 0;
        if (sStatusLogged++ < 1) {
            JIHAD_GPU_LOG("[jihad-gpu] texture conversion: framebuffer status 0x%x\n", aGL->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER));
        }
        aGL->fClear(LOCAL_GL_COLOR_BUFFER_BIT);
        aGL->fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0, LOCAL_GL_TEXTURE_2D, 0, 0);
    }
    aGL->fTexSubImage2D(LOCAL_GL_TEXTURE_2D, 0, 0, 0, 1, 1, LOCAL_GL_BGRA, LOCAL_GL_UNSIGNED_BYTE, &px);
    sJihadConvertMs += (mozilla::TimeStamp::Now() - t0).ToMilliseconds();
    sJihadConverted++;
}
}

bool JihadPoolTexture(GLContext* aGL, GLuint aTexture, const gfx::IntSize& aSize)
{
    if (!JihadPoolCap() || !aTexture) {
        return false;
    }
    mozilla::StaticMutexAutoLock lock(sJihadPoolLock);
    if (sJihadPool.size() >= JihadPoolCap()) {
        sJihadPoolFull++;
        return false;
    }
    sJihadPool.push_back(JihadPooledTexture{aGL, aTexture, aSize});
    sJihadPoolKept++;
    return true;
}

GLuint JihadTakePooledTexture(GLContext* aGL, const gfx::IntSize& aSize)
{
    if (!JihadPoolCap()) {
        return 0;
    }
    GLuint tex = 0;
    {
        mozilla::StaticMutexAutoLock lock(sJihadPoolLock);
        for (size_t i = sJihadPool.size(); i-- > 0;) {
            if (sJihadPool[i].gl == aGL) {   // any size: the conversion survives a re-specification
                tex = sJihadPool[i].tex;
                sJihadPool.erase(sJihadPool.begin() + i);
                break;
            }
        }
        if (tex) sJihadPoolHits++; else sJihadPoolMisses++;
        const uint32_t n = sJihadPoolHits + sJihadPoolMisses;
        if (n <= 4 || (n % 50) == 0) {
            JIHAD_GPU_LOG("[jihad-gpu] texture pool: %u taken from the pool, %u new (%u converted, %.1f ms each), %u kept, %u deleted (pool full), %zu in the pool\n",
                    sJihadPoolHits, sJihadPoolMisses, sJihadConverted, sJihadConverted ? sJihadConvertMs / sJihadConverted : 0.0,
                    sJihadPoolKept, sJihadPoolFull, sJihadPool.size());
        }
    }
    return tex;
}

void JihadDropPooledTextures(GLContext* aGL, bool aDelete)
{
    std::vector<GLuint> mine;
    {
        mozilla::StaticMutexAutoLock lock(sJihadPoolLock);
        for (size_t i = 0; i < sJihadPool.size();) {
            if (sJihadPool[i].gl == aGL) {
                mine.push_back(sJihadPool[i].tex);
                sJihadPool.erase(sJihadPool.begin() + i);
            } else {
                ++i;
            }
        }
    }
    if (aDelete && !mine.empty() && aGL && aGL->MakeCurrent()) {
        aGL->fDeleteTextures((GLsizei)mine.size(), mine.data());
    }
    if (sJihadConvertGL == aGL) {
        if (aDelete && sJihadConvertFb && aGL && aGL->MakeCurrent()) {
            aGL->fDeleteFramebuffers(1, &sJihadConvertFb);
        }
        sJihadConvertGL = nullptr;
        sJihadConvertFb = 0;
    }
}

// ---- Jihad T-502: fresh texture names per upload, retired names deleted when the GPU is idle --
namespace {
mozilla::StaticMutex sJihadRetireLock;
std::vector<std::pair<GLContext*, GLuint>> sJihadRetired;
}

void JihadRetireTexture(GLContext* aGL, GLuint aTexture)
{
    mozilla::StaticMutexAutoLock lock(sJihadRetireLock);
    sJihadRetired.push_back(std::make_pair(aGL, aTexture));
}

void JihadFlushRetiredTextures(GLContext* aGL)
{
    std::vector<GLuint> mine;
    {
        mozilla::StaticMutexAutoLock lock(sJihadRetireLock);
        for (size_t i = 0; i < sJihadRetired.size();) {
            if (sJihadRetired[i].first == aGL) {
                mine.push_back(sJihadRetired[i].second);
                sJihadRetired.erase(sJihadRetired.begin() + i);
            } else {
                ++i;
            }
        }
    }
    if (mine.empty() || !aGL || !aGL->MakeCurrent()) {
        return;
    }
    const mozilla::TimeStamp t0 = mozilla::TimeStamp::Now();
    aGL->fDeleteTextures((GLsizei)mine.size(), mine.data());
    static uint32_t sN = 0;
    const double ms = (mozilla::TimeStamp::Now() - t0).ToMilliseconds();
    if (sN++ < 10 || ms >= 3.0) {
        if (sN < 200) JIHAD_GPU_LOG("[jihad-gpu] retired textures: %u deleted in %.1f ms\n", (unsigned)mine.size(), ms);
    }
}

void
BasicTextureImage::JihadSwapTextures()
{
    // Off unless asked for (JIHAD_GPU_FRESH_TEX=1): measured 2026-10-17 (run 86) the first upload
    // into a fresh name waits just as the re-upload did (21 ms a tile), so the driver waits for
    // the pending GPU work, not for the name; the fresh names only added allocation churn.
    static const bool sFresh = getenv("JIHAD_GPU_COMPOSITOR") && getenv("JIHAD_GPU_FRESH_TEX");
    if (!sFresh) {
        return;
    }
    if (mTextureState == Created) {
        return;   // never drawn with: the current name is as fresh as a new one
    }
    JihadRetireTexture(mGLContext, mTexture);
    mGLContext->fGenTextures(1, &mTexture);
    ScopedBindTexture bind(mGLContext, mTexture);
    mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, mTexFilter);
    mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, mTexFilter);
    mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_S, mWrapMode);
    mGLContext->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_T, mWrapMode);
    mTextureState = Created;
}

void
BasicTextureImage::BindTexture(GLenum aTextureUnit)
{
    mGLContext->fActiveTexture(aTextureUnit);
    mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, mTexture);
    mGLContext->fActiveTexture(LOCAL_GL_TEXTURE0);
}

bool
BasicTextureImage::DirectUpdate(gfx::DataSourceSurface* aSurf, const nsIntRegion& aRegion, const gfx::IntPoint& aFrom /* = gfx::IntPoint(0, 0) */)
{
    JihadSwapTextures();   // Jihad T-502: the name the last composite is not reading
    nsIntRegion region;
    if (mTextureState == Valid) {
        region = aRegion;
    } else {
        region = nsIntRegion(IntRect(0, 0, mSize.width, mSize.height));
    }
    bool needInit = mTextureState == Created;
    size_t uploadSize;

    mTextureFormat =
        UploadSurfaceToTexture(mGLContext,
                               aSurf,
                               region,
                               mTexture,
                               mSize,
                               &uploadSize,
                               needInit,
                               aFrom);

    if (uploadSize > 0) {
        UpdateUploadSize(uploadSize);
    }
    mTextureState = Valid;
    return true;
}

void
BasicTextureImage::Resize(const gfx::IntSize& aSize)
{
    mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, mTexture);

    // This matches the logic in UploadImageDataToTexture so that
    // we avoid mixing formats.
    GLenum format;
    GLenum type;
    GLenum internalFormat = LOCAL_GL_RGBA;
    if (mGLContext->GetPreferredARGB32Format() == LOCAL_GL_BGRA) {
        format = LOCAL_GL_BGRA;
        if (mGLContext->IsGLES()) {
            // Jihad: BGRA8888 on GLES takes UNSIGNED_BYTE and the same internal format
            type = LOCAL_GL_UNSIGNED_BYTE;
            internalFormat = LOCAL_GL_BGRA;
        } else {
            type = LOCAL_GL_UNSIGNED_INT_8_8_8_8_REV;
        }
    } else {
        format = LOCAL_GL_RGBA;
        type = LOCAL_GL_UNSIGNED_BYTE;
    }

    mGLContext->fTexImage2D(LOCAL_GL_TEXTURE_2D,
                            0,
                            internalFormat,
                            aSize.width,
                            aSize.height,
                            0,
                            format,
                            type,
                            nullptr);

    mTextureState = Allocated;
    mSize = aSize;
}

gfx::IntSize TextureImage::GetSize() const {
  return mSize;
}

TextureImage::TextureImage(const gfx::IntSize& aSize,
             GLenum aWrapMode, ContentType aContentType,
             Flags aFlags)
    : mSize(aSize)
    , mWrapMode(aWrapMode)
    , mContentType(aContentType)
    , mTextureFormat(gfx::SurfaceFormat::UNKNOWN)
    , mSamplingFilter(SamplingFilter::GOOD)
    , mFlags(aFlags)
    , mUploadSize(0)
{}

BasicTextureImage::BasicTextureImage(GLuint aTexture,
                  const gfx::IntSize& aSize,
                  GLenum aWrapMode,
                  ContentType aContentType,
                  GLContext* aContext,
                  TextureImage::Flags aFlags)
  : TextureImage(aSize, aWrapMode, aContentType, aFlags)
  , mTexture(aTexture)
  , mTextureState(Created)
  , mGLContext(aContext)
{
    mTexFilter = (aFlags & TextureImage::UseNearestFilter) ? LOCAL_GL_NEAREST : LOCAL_GL_LINEAR;
}

static bool
WantsSmallTiles(GLContext* gl)
{
    // We can't use small tiles on the SGX 540, because of races in texture upload.
    if (gl->WorkAroundDriverBugs() &&
        gl->Renderer() == GLRenderer::SGX540)
        return false;

    // We should use small tiles for good performance if we can't use
    // glTexSubImage2D() for some reason.
    if (!CanUploadSubTextures(gl))
        return true;

    // Don't use small tiles otherwise. (If we implement incremental texture upload,
    // then we will want to revisit this.)
    return false;
}

TiledTextureImage::TiledTextureImage(GLContext* aGL,
                                     gfx::IntSize aSize,
                                     TextureImage::ContentType aContentType,
                                     TextureImage::Flags aFlags,
                                     TextureImage::ImageFormat aImageFormat)
    : TextureImage(aSize, LOCAL_GL_CLAMP_TO_EDGE, aContentType, aFlags)
    , mCurrentImage(0)
    , mIterationCallback(nullptr)
    , mIterationCallbackData(nullptr)
    , mRows(0)
    , mColumns(0)
    , mGL(aGL)
    , mTextureState(Created)
    , mImageFormat(aImageFormat)
{
    if (!(aFlags & TextureImage::DisallowBigImage) && WantsSmallTiles(mGL)) {
      mTileSize = 256;
    } else {
      mGL->fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, (GLint*) &mTileSize);
    }
    if (aSize.width != 0 && aSize.height != 0) {
        Resize(aSize);
    }
}

TiledTextureImage::~TiledTextureImage()
{
}

bool
TiledTextureImage::DirectUpdate(gfx::DataSourceSurface* aSurf, const nsIntRegion& aRegion, const gfx::IntPoint& aFrom /* = gfx::IntPoint(0, 0) */)
{
    if (mSize.width == 0 || mSize.height == 0) {
        return true;
    }

    nsIntRegion region;

    if (mTextureState != Valid) {
        IntRect bounds = IntRect(0, 0, mSize.width, mSize.height);
        region = nsIntRegion(bounds);
    } else {
        region = aRegion;
    }

    bool result = true;
    int oldCurrentImage = mCurrentImage;
    BeginBigImageIteration();
    do {
        IntRect tileRect = GetSrcTileRect();
        int xPos = tileRect.x;
        int yPos = tileRect.y;

        nsIntRegion tileRegion;
        tileRegion.And(region, tileRect); // intersect with tile

        if (tileRegion.IsEmpty())
            continue;

        tileRegion.MoveBy(-xPos, -yPos); // translate into tile local space

        result &= mImages[mCurrentImage]->
          DirectUpdate(aSurf, tileRegion, aFrom + gfx::IntPoint(xPos, yPos));

        if (mCurrentImage == mImages.Length() - 1) {
            // We know we're done, but we still need to ensure that the callback
            // gets called (e.g. to update the uploaded region).
            NextTile();
            break;
        }
        // Override a callback cancelling iteration if the texture wasn't valid.
        // We need to force the update in that situation, or we may end up
        // showing invalid/out-of-date texture data.
    } while (NextTile() || (mTextureState != Valid));
    mCurrentImage = oldCurrentImage;

    mTextureFormat = mImages[0]->GetTextureFormat();
    mTextureState = Valid;
    return result;
}

void TiledTextureImage::BeginBigImageIteration()
{
    mCurrentImage = 0;
}

bool TiledTextureImage::NextTile()
{
    bool continueIteration = true;

    if (mIterationCallback)
        continueIteration = mIterationCallback(this, mCurrentImage,
                                               mIterationCallbackData);

    if (mCurrentImage + 1 < mImages.Length()) {
        mCurrentImage++;
        return continueIteration;
    }
    return false;
}

void TiledTextureImage::SetIterationCallback(BigImageIterationCallback aCallback,
                                             void* aCallbackData)
{
    mIterationCallback = aCallback;
    mIterationCallbackData = aCallbackData;
}

gfx::IntRect TiledTextureImage::GetTileRect()
{
    if (!GetTileCount()) {
        return gfx::IntRect();
    }
    gfx::IntRect rect = mImages[mCurrentImage]->GetTileRect();
    unsigned int xPos = (mCurrentImage % mColumns) * mTileSize;
    unsigned int yPos = (mCurrentImage / mColumns) * mTileSize;
    rect.MoveBy(xPos, yPos);
    return rect;
}

gfx::IntRect TiledTextureImage::GetSrcTileRect()
{
    gfx::IntRect rect = GetTileRect();
    const bool needsYFlip = mFlags & OriginBottomLeft;
    unsigned int srcY = needsYFlip ? mSize.height - rect.height - rect.y
                                   : rect.y;
    return gfx::IntRect(rect.x, srcY, rect.width, rect.height);
}

void
TiledTextureImage::BindTexture(GLenum aTextureUnit)
{
    if (!GetTileCount()) {
        return;
    }
    mImages[mCurrentImage]->BindTexture(aTextureUnit);
}

/*
 * Resize, trying to reuse tiles. The reuse strategy is to decide on reuse per
 * column. A tile on a column is reused if it hasn't changed size, otherwise it
 * is discarded/replaced. Extra tiles on a column are pruned after iterating
 * each column, and extra rows are pruned after iteration over the entire image
 * finishes.
 */
void TiledTextureImage::Resize(const gfx::IntSize& aSize)
{
    if (mSize == aSize && mTextureState != Created) {
        return;
    }

    // calculate rows and columns, rounding up
    unsigned int columns = (aSize.width  + mTileSize - 1) / mTileSize;
    unsigned int rows = (aSize.height + mTileSize - 1) / mTileSize;

    // Iterate over old tile-store and insert/remove tiles as necessary
    int row;
    unsigned int i = 0;
    for (row = 0; row < (int)rows; row++) {
        // If we've gone beyond how many rows there were before, set mColumns to
        // zero so that we only create new tiles.
        if (row >= (int)mRows)
            mColumns = 0;

        // Similarly, if we're on the last row of old tiles and the height has
        // changed, discard all tiles in that row.
        // This will cause the pruning of columns not to work, but we don't need
        // to worry about that, as no more tiles will be reused past this point
        // anyway.
        if ((row == (int)mRows - 1) && (aSize.height != mSize.height))
            mColumns = 0;

        int col;
        for (col = 0; col < (int)columns; col++) {
            IntSize size( // use tilesize first, then the remainder
                    (col+1) * mTileSize > (unsigned int)aSize.width  ? aSize.width  % mTileSize : mTileSize,
                    (row+1) * mTileSize > (unsigned int)aSize.height ? aSize.height % mTileSize : mTileSize);

            bool replace = false;

            // Check if we can re-use old tiles.
            if (col < (int)mColumns) {
                // Reuse an existing tile. If the tile is an end-tile and the
                // width differs, replace it instead.
                if (mSize.width != aSize.width) {
                    if (col == (int)mColumns - 1) {
                        // Tile at the end of the old column, replace it with
                        // a new one.
                        replace = true;
                    } else if (col == (int)columns - 1) {
                        // Tile at the end of the new column, create a new one.
                    } else {
                        // Before the last column on both the old and new sizes,
                        // reuse existing tile.
                        i++;
                        continue;
                    }
                } else {
                    // Width hasn't changed, reuse existing tile.
                    i++;
                    continue;
                }
            }

            // Create a new tile.
            RefPtr<TextureImage> teximg =
                TileGenFunc(mGL, size, mContentType, mFlags, mImageFormat);
            if (replace)
                mImages.ReplaceElementAt(i, teximg);
            else
                mImages.InsertElementAt(i, teximg);
            i++;
        }

        // Prune any unused tiles on the end of the column.
        if (row < (int)mRows) {
            for (col = (int)mColumns - col; col > 0; col--) {
                mImages.RemoveElementAt(i);
            }
        }
    }

    // Prune any unused tiles at the end of the store.
    unsigned int length = mImages.Length();
    for (; i < length; i++)
      mImages.RemoveElementAt(mImages.Length()-1);

    // Reset tile-store properties.
    mRows = rows;
    mColumns = columns;
    mSize = aSize;
    mTextureState = Allocated;
    mCurrentImage = 0;
}

uint32_t TiledTextureImage::GetTileCount()
{
    return mImages.Length();
}

already_AddRefed<TextureImage>
CreateTiledTextureImage(GLContext* aGL,
                        const gfx::IntSize& aSize,
                        TextureImage::ContentType aContentType,
                        TextureImage::Flags aFlags,
                        TextureImage::ImageFormat aImageFormat)
{
  RefPtr<TextureImage> texImage = static_cast<TextureImage*>(
      new gl::TiledTextureImage(aGL, aSize, aContentType, aFlags, aImageFormat));
  return texImage.forget();
}


already_AddRefed<TextureImage>
CreateBasicTextureImage(GLContext* aGL,
                        const gfx::IntSize& aSize,
                        TextureImage::ContentType aContentType,
                        GLenum aWrapMode,
                        TextureImage::Flags aFlags)
{
    bool useNearestFilter = aFlags & TextureImage::UseNearestFilter;
    if (!aGL->MakeCurrent()) {
      return nullptr;
    }

    GLuint texture = JihadTakePooledTexture(aGL, aSize);   // Jihad T-504
    if (!texture) {
        aGL->fGenTextures(1, &texture);
        if (JihadPoolCap() && !getenv("JIHAD_GPU_NO_TEX_CONVERT")) {
            JihadConvertTexture(aGL, texture);
        }
    }

    ScopedBindTexture bind(aGL, texture);

    GLint texfilter = useNearestFilter ? LOCAL_GL_NEAREST : LOCAL_GL_LINEAR;
    aGL->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, texfilter);
    aGL->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, texfilter);
    aGL->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_S, aWrapMode);
    aGL->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_T, aWrapMode);

    RefPtr<BasicTextureImage> texImage =
        new BasicTextureImage(texture, aSize, aWrapMode, aContentType,
                              aGL, aFlags);
    return texImage.forget();
}

} // namespace gl
} // namespace mozilla
