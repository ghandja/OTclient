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

#pragma once

#include "declarations.h"
#include <framework/core/timer.h>

class Texture
{
public:
    Texture();
    Texture(const Size& size);
    Texture(const ImagePtr& image, bool buildMipmaps = false, bool compress = false);
    virtual ~Texture();

    virtual void create();
    void uploadPixels(const ImagePtr& image, bool buildMipmaps = false, bool compress = false);
    void updateImage(const ImagePtr& image);
    void updatePixels(uint8_t* pixels, int level = 0, int channels = 4, bool compress = false);

    virtual void buildHardwareMipmaps();

    // Enable trilinear/mipmap minification for this texture. Unlike buildHardwareMipmaps()
    // it does not need the GL texture to exist yet: it just flags buildMipmaps + hasMipMaps
    // so that the deferred create()/uploadPixels() (on the render thread) uploads the CPU
    // mip chain and setupFilters() selects a *_MIPMAP_* filter. Safe to call right after
    // construction, before the texture is created.
    void enableMipmaps();

    virtual void setSmooth(bool smooth);
    virtual void setRepeat(bool repeat);
    void setUpsideDown(bool upsideDown);
    void setTime(const ticks_t time) { m_time = time; }

    const Size& getSize() const { return m_size; }
    auto getTransformMatrixId() const { return m_transformMatrixId; }

    auto getAtlasRegion(Fw::TextureAtlasType type) const { return m_atlas[type]; }
    const AtlasRegion* getAtlasRegion() const;

    ticks_t getTime() const { return m_time; }
    uint32_t getId() const { return m_id; }
    uint32_t getUniqueId() const { return m_uniqueId; }
    size_t hash() const { return m_hash; }

    int getWidth() const { return m_size.width(); }
    int getHeight() const { return m_size.height(); }

    virtual bool isAnimatedTexture() const { return false; }
    bool isEmpty() const { return m_id == 0; }
    bool hasRepeat() const { return getProp(repeat); }
    bool hasMipmaps() const { return getProp(hasMipMaps); }
    bool isSmooth() const { return getProp(smooth); }
    bool canCacheInAtlas() const { return getProp(Prop::_allowAtlasCache); }
    bool setupSize(const Size& size);

    virtual void allowAtlasCache();

    // Batched GL texture deletion. Call EXCLUSIVELY from the main thread (that is where the
    // OpenGL context is active). For details see the comment at the definition in texture.cpp.
    static void flushDeletedTextures();

    // Batched deletion diagnostics: total number of deleted GL textures and number of batches.
    // The ratio shows how many glDeleteTextures calls (and dispatcher events) we save.
    static size_t getDeletedCount();
    static size_t getDeletionBatchCount();

protected:
    void bind();
    void setupWrap() const;
    void setupFilters() const;
    void createTexture();
    void setupTranformMatrix();
    void setupPixels(int level, const Size& size, const uint8_t* pixels, int channels = 4, bool compress = false) const;
    void generateHash() { m_hash = stdext::hash_int(m_id > 0 ? m_id : m_uniqueId); }

    const uint32_t m_uniqueId;

    std::array<AtlasRegion*, Fw::TextureAtlasType::LAST> m_atlas{ };

    uint32_t m_id{ 0 };
    ticks_t m_time{ 0 };
    size_t m_hash{ 0 };

    Size m_size;
    Timer m_lastTimeUsage;

    // how many channels the texture's already-allocated memory has (-1 = nothing uploaded yet).
    // Lets us swap the contents via glTexSubImage2D instead of reallocating the whole texture.
    mutable int m_storageChannels{ -1 };

    uint16_t m_transformMatrixId{ 0 };

    ImagePtr m_image;

    // Path of the file this texture was created from (set by TextureManager when loading from disk).
    // Thanks to it, GC can free the pixel copy of a texture that was never drawn,
    // and create() can restore it from disk at exactly the moment of the first draw.
    // Empty = the texture came from memory (base64, HTTP, QR, framebuffer) and MUST NOT be cleared.
    std::string m_source;

public:
    // whether the texture still holds a CPU-side pixel copy (not yet uploaded to the GPU)
    bool hasPendingImage() const { return m_image != nullptr; }

    // whether we can restore this texture from disk after freeing the pixel copy
    bool isReloadableFromFile() const { return !m_source.empty(); }

protected:

    enum Prop : uint16_t
    {
        hasMipMaps = 1 << 0,
        smooth = 1 << 1,
        upsideDown = 1 << 2,
        repeat = 1 << 3,
        compress = 1 << 4,
        buildMipmaps = 1 << 5,
        _allowAtlasCache = 1 << 6
    };

    uint16_t m_props{ 0 };
    void setProp(const Prop prop, const bool v) { if (v) m_props |= prop; else m_props &= ~prop; }
    bool getProp(const Prop prop) const { return m_props & prop; };

    friend class GarbageCollection;
    friend class TextureManager;
    friend class TextureAtlas;
    // Vulkan renderer stage 4: the feeder reads m_image/m_source to put the pixels into the atlas.
    friend class VkDrawFeeder;
};
