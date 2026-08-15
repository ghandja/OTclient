/*
 * Copyright (c) 2010-2026 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "texture.h"

#include "drawpoolmanager.h"
#include "graphics.h"
#include "image.h"
#include "textureatlas.h"
#include "texturemanager.h"
#include "framework/core/eventdispatcher.h"
#include <framework/core/graphicalapplication.h>
#include <framework/platform/platformwindow.h>
#include <framework/util/stats.h>
#include <mutex>
#include <vector>

 // UINT16_MAX = just to avoid conflicts with GL generated ID.
static std::atomic_uint32_t UID(UINT16_MAX);

// --- Batched GL texture deletion queue ---------------------------------------------------------
// WHY we defer at all: ~Texture can run on any thread (GC of things runs on the map thread,
// asynchronous texture loading on the task pool), while glDeleteTextures may only be called
// where an active GL context exists, i.e. on the main thread.
//
// WHY batched: previously EVERY dying texture pushed its OWN event into
// g_mainDispatcher (shared_ptr<Event> + std::function + event name = several allocations per
// texture). GarbageCollection::thingType() can unload 500 objects every 2 s, and each of them
// has a separate texture per animation phase - that is thousands of small allocations and thousands
// of glDeleteTextures calls per GC pass. Now we collect just the identifiers in a single vector
// and once per frame delete them with ONE glDeleteTextures call. This also shortens the window in
// which old and new textures live in the driver simultaneously.
namespace
{
    struct PendingTextureDeletion
    {
        uint32_t id;
        bool smooth;
    };

    std::mutex g_pendingDeletionMutex;
    std::vector<PendingTextureDeletion> g_pendingDeletions;

    // Diagnostics: how many GL textures were actually deleted and in how many batches. Without
    // this there is no way to check whether batched deletion runs at all during gameplay (on the
    // login screen things do not get unloaded, so the queue is nearly empty).
    std::atomic_size_t g_deletedTextures{ 0 };
    std::atomic_size_t g_deletionBatches{ 0 };
}

size_t Texture::getDeletedCount() { return g_deletedTextures.load(std::memory_order_relaxed); }
size_t Texture::getDeletionBatchCount() { return g_deletionBatches.load(std::memory_order_relaxed); }

Texture::Texture() : m_uniqueId(UID.fetch_add(1)) {
    generateHash();
    g_stats.addTexture();
}
Texture::Texture(const Size& size) : m_uniqueId(UID.fetch_add(1))
{
    generateHash();
    g_stats.addTexture();
    if (!setupSize(size))
        return;

    // Pure Vulkan mode: zero GL calls (GLEW functions are nullptrs). The object lives as
    // size metadata - it is used by pool framebuffers, which nobody draws in this mode.
    if (!g_window.hasGLContext())
        return;

    createTexture();
    bind();
    setupPixels(0, size, nullptr, 4);
    setupWrap();
    setupFilters();
}

Texture::Texture(const ImagePtr& image, const bool buildMipmaps, const bool compress) : m_uniqueId(UID.fetch_add(1))
{
    generateHash();
    g_stats.addTexture();

    setProp(Prop::compress, compress);
    setProp(Prop::buildMipmaps, buildMipmaps);
    m_image = image;
    setupSize(image->getSize());
}

Texture::~Texture()
{
#ifndef NDEBUG
    assert(!g_app.isTerminated());
#endif
    if (g_graphics.ok() && m_id != 0) {
        // Just the identifier goes into the queue - the real glDeleteTextures is done by
        // flushDeletedTextures() on the main thread, with one call for the whole batch.
        std::scoped_lock lock(g_pendingDeletionMutex);
        g_pendingDeletions.emplace_back(m_id, isSmooth());
    }
    g_stats.removeTexture();
}

void Texture::flushDeletedTextures()
{
    // Called from the main thread (GraphicalApplication::mainPoll), right after g_mainDispatcher.poll() -
    // that is EXACTLY the same spot in the frame where the old deletion events used to execute.
    // The behavior relative to drawing is therefore identical to before.
    static std::vector<PendingTextureDeletion> batch; // reused buffer - zero allocations in steady state
    static std::vector<uint32_t> ids;

    {
        std::scoped_lock lock(g_pendingDeletionMutex);
        if (g_pendingDeletions.empty())
            return;

        batch.clear();
        batch.swap(g_pendingDeletions); // after the swap the global queue is empty and ready for new entries
    }

    if (!g_graphics.ok()) {
        batch.clear();
        return;
    }

    ids.clear();
    ids.reserve(batch.size());

    for (const auto& [id, smooth] : batch) {
        // The atlas indexes its regions by the source texture's identifier, so it must release
        // them BEFORE the identifier returns to the GL pool - otherwise a new texture with the
        // same id would inherit someone else's region and show the wrong graphic. TextureAtlas::removeTexture()
        // has no lock of its own, and atlases are modified during flush on the render thread,
        // i.e. the same thread as this function - which is why this is a safe place.
        g_drawPool.removeTextureFromAtlas(id, smooth);
        ids.push_back(id);
    }

    glDeleteTextures(static_cast<GLsizei>(ids.size()), ids.data());

    g_deletedTextures.fetch_add(ids.size(), std::memory_order_relaxed);
    g_deletionBatches.fetch_add(1, std::memory_order_relaxed);

    batch.clear();
}

void Texture::create()
{
    // The pixel copy may have been freed by GarbageCollection::pendingImages() - this happens
    // exclusively for textures that never made it into GL (m_id == 0) and thus were never
    // drawn. Since we know the source file, we load it back at exactly the moment of the first
    // draw. Thanks to this, freeing the pixels CANNOT end up as a blank graphic.
    if (!m_image && m_id == 0 && !m_source.empty())
        m_image = Image::load(m_source);

    if (m_image) {
        createTexture();
        uploadPixels(m_image, getProp(buildMipmaps), getProp(compress));
        m_image = nullptr;
    }
}

void Texture::updateImage(const ImagePtr& image) { m_image = image; setupSize(image->getSize()); }

void Texture::updatePixels(uint8_t* pixels, const int level, const int channels, const bool compress) {
    bind();

    // glTexImage2D reallocates the whole texture on every call, and LightView refreshes its own
    // every frame. When the size and format have not changed, overwriting the contents is enough.
    if (level == 0 && !compress && m_storageChannels == channels) {
        GLenum format = 0;
        switch (channels) {
            case 4: format = GL_RGBA; break;
            case 3: format = GL_RGB; break;
            case 2: format = GL_LUMINANCE_ALPHA; break;
            case 1: format = GL_LUMINANCE; break;
            default: break;
        }

        if (format != 0) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_size.width(), m_size.height(), format, GL_UNSIGNED_BYTE, pixels);
            return;
        }
    }

    setupPixels(level, m_size, pixels, channels, compress);
}
void Texture::uploadPixels(const ImagePtr& image, const bool buildMipmaps, const bool compress)
{
    if (!setupSize(image->getSize()))
        return;

    bind();

    uint_fast8_t level = 0;
    do {
        setupPixels(level++, image->getSize(), image->getPixelData(), image->getBpp(), compress);
    } while (buildMipmaps && image->nextMipmap());
    if (buildMipmaps) setProp(Prop::buildMipmaps, true);

    setupWrap();
    setupFilters();
}

void Texture::bind() { if (m_id) glBindTexture(GL_TEXTURE_2D, m_id); }

void Texture::buildHardwareMipmaps()
{
    if (getProp(hasMipMaps))
        return;

#ifndef OPENGL_ES
    if (!glGenerateMipmap)
        return;
#endif

    setProp(hasMipMaps, true);

    bind();
    setupFilters();
    glGenerateMipmap(GL_TEXTURE_2D);
}

void Texture::enableMipmaps()
{
    setProp(Prop::buildMipmaps, true); // create()/uploadPixels() will upload the CPU mip chain
    setProp(Prop::hasMipMaps, true);   // setupFilters() will pick a *_MIPMAP_* min filter
    if (m_id) {                        // already created: re-apply the filter now
        bind();
        setupFilters();
    }
}

void Texture::setSmooth(const bool smooth)
{
    if (smooth == getProp(Prop::smooth))
        return;

    setProp(Prop::smooth, smooth);

    if (!m_id) return;

    if (!canCacheInAtlas()) {
        bind();
        setupFilters();
    } else
        g_drawPool.removeTextureFromAtlas(m_id, !smooth);
}

void Texture::allowAtlasCache() {
    bool smooth = isSmooth();
    if (smooth) setSmooth(false);
    setProp(Prop::_allowAtlasCache, true);
    setSmooth(smooth);
}

void Texture::setRepeat(const bool repeat)
{
    if (getProp(Prop::repeat) == repeat)
        return;

    setProp(Prop::repeat, repeat);

    if (!m_id) return;

    bind();
    setupWrap();
}

void Texture::setUpsideDown(const bool upsideDown)
{
    if (getProp(Prop::upsideDown) == upsideDown)
        return;

    setProp(Prop::upsideDown, upsideDown);
    setupTranformMatrix();
}

void Texture::createTexture()
{
    if (g_graphics.ok() && m_id != 0)
        glDeleteTextures(1, &m_id);

    glGenTextures(1, &m_id);
    assert(m_id != 0);

    generateHash();
}

bool Texture::setupSize(const Size& size)
{
    if (m_size == size)
        return true;

    // the size is changing, so the existing allocation no longer fits - the next refresh
    // must go through the full glTexImage2D path again
    m_storageChannels = -1;

    // checks texture max size
    if (std::max<int>(size.width(), size.height()) > g_graphics.getMaxTextureSize()) {
        g_logger.error(
            "loading texture with size {}x{} failed, "
            "the maximum size allowed by the graphics card is {}x{}, "
            "to prevent crashes the texture will be displayed as a blank texture",
            size.width(), size.height(), g_graphics.getMaxTextureSize(), g_graphics.getMaxTextureSize()
        );
        return false;
    }

    m_size = size;

    setupTranformMatrix();

    return true;
}

void Texture::setupWrap() const
{
    const GLint texParam = getProp(repeat) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, texParam);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, texParam);
}

void Texture::setupFilters() const
{
    if (!m_id) return;

    GLenum minFilter;
    GLenum magFilter;
    if (getProp(smooth)) {
        minFilter = getProp(hasMipMaps) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
        magFilter = GL_LINEAR;
    } else {
        minFilter = getProp(hasMipMaps) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        magFilter = GL_NEAREST;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
}

void Texture::setupTranformMatrix()
{
    m_transformMatrixId = g_textures.getMatrixId(m_size, getProp(upsideDown));
}

const AtlasRegion* Texture::getAtlasRegion() const {
    if (g_drawPool.isValid() && g_drawPool.getAtlas()) {
        if (const auto region = m_atlas[g_drawPool.getAtlas()->getType()]) {
            return region->isEnabled() ? region : nullptr;
        }
    }

    return nullptr;
}

void Texture::setupPixels(const int level, const Size& size, const uint8_t* pixels, const int channels, const bool
#ifndef OPENGL_ES
                          compress
#endif
) const
{
    GLenum format = 0;
    GLenum internalFormat = GL_R8;
    switch (channels) {
        case 4:
            format = GL_RGBA;
            internalFormat = GL_RGBA;
            break;
        case 3:
            format = GL_RGB;
            internalFormat = GL_RGB;
            break;
        case 2:
            format = GL_LUMINANCE_ALPHA;
            break;
        case 1:
            format = GL_LUMINANCE;
            break;
    }

#ifdef OPENGL_ES
    //TODO
#else
    if (compress)
        internalFormat = GL_COMPRESSED_RGBA;
#endif

    glTexImage2D(GL_TEXTURE_2D, level, internalFormat, size.width(), size.height(), 0, format, GL_UNSIGNED_BYTE, pixels);

    // remember the format of the full allocation so subsequent refreshes can go through glTexSubImage2D
    if (level == 0 && size == m_size) {
#ifndef OPENGL_ES
        m_storageChannels = compress ? -1 : channels;
#else
        m_storageChannels = channels;
#endif
    }
}
