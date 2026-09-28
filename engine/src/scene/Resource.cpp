#include "crossrender/Resource.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

namespace crossrender {

Texture* ResourceCache::Texture_(const std::string& path, bool srgb) {
    std::string key = path + (srgb ? "|srgb" : "");
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second.value.get();
    Entry<Texture> e;
    e.path = path;
    e.value.reset(new Texture());
    if (!e.value->LoadFromFile(path, srgb)) {
        ENG_LOGE("res", "texture '%s' failed to load", path.c_str());
        return nullptr;
    }
    e.value->SetDebugName(path);
    e.timestamp = FS().FileTime(path);
    Texture* raw = e.value.get();
    textures_[key] = std::move(e);
    return raw;
}

Font* ResourceCache::Font_(const std::string& path, const FontDesc& desc) {
    char key[512];
    std::snprintf(key, sizeof(key), "%s|%.1f|%d|%d", path.c_str(), desc.pixelHeight,
                  desc.sdf ? 1 : 0, static_cast<int>(desc.atlasSize));
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second.value.get();
    Entry<Font> e;
    e.path = path;
    e.value.reset(new Font());
    if (!e.value->LoadFromFile(path, desc)) {
        ENG_LOGE("res", "font '%s' failed to load", path.c_str());
        return nullptr;
    }
    e.timestamp = FS().FileTime(path);
    Font* raw = e.value.get();
    fonts_[key] = std::move(e);
    return raw;
}

Model* ResourceCache::Model_(const std::string& path) {
    auto it = models_.find(path);
    if (it != models_.end()) return it->second.value.get();
    Entry<Model> e;
    e.path = path;
    e.value.reset(new Model());
    if (!e.value->Load(path)) {
        ENG_LOGE("res", "model '%s' failed to load", path.c_str());
        return nullptr;
    }
    e.value->UploadToGpu();
    e.timestamp = FS().FileTime(path);
    Model* raw = e.value.get();
    models_[path] = std::move(e);
    return raw;
}

AudioClip* ResourceCache::Audio_(const std::string& path) {
    auto it = audio_.find(path);
    if (it != audio_.end()) return it->second.value.get();
    Entry<AudioClip> e;
    e.path = path;
    e.value.reset(new AudioClip());
    if (!e.value->LoadFromFile(path)) {
        ENG_LOGE("res", "audio '%s' failed to load", path.c_str());
        return nullptr;
    }
    e.timestamp = FS().FileTime(path);
    AudioClip* raw = e.value.get();
    audio_[path] = std::move(e);
    return raw;
}

Shader* ResourceCache::Shader_(const std::string& basePath) {
    auto it = shaders_.find(basePath);
    if (it != shaders_.end()) return it->second.value.get();
    Entry<Shader> e;
    e.path = basePath;
    e.value.reset(new Shader());
    if (!e.value->Load(basePath)) {
        ENG_LOGE("res", "shader '%s' failed to load", basePath.c_str());
        return nullptr;
    }
    Shader* raw = e.value.get();
    shaders_[basePath] = std::move(e);
    return raw;
}

Shader* ResourceCache::ShaderFromSource(const std::string& key, const char* vert, const char* frag) {
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return it->second.value.get();
    Entry<Shader> e;
    e.path = key;
    e.value.reset(new Shader());
    if (!e.value->Build(vert, frag, key)) {
        ENG_LOGE("res", "inline shader '%s' failed to build", key.c_str());
        return nullptr;
    }
    Shader* raw = e.value.get();
    shaders_[key] = std::move(e);
    return raw;
}

Texture* ResourceCache::WhiteTexture() {
    if (!white_) {
        white_.reset(new Texture());
        white_->CreateSolid(Color::White);
        white_->SetDebugName("white");
    }
    return white_.get();
}

Texture* ResourceCache::BlackTexture() {
    if (!black_) {
        black_.reset(new Texture());
        black_->CreateSolid(Color::Black);
        black_->SetDebugName("black");
    }
    return black_.get();
}

Texture* ResourceCache::NormalFlatTexture() {
    if (!normalFlat_) {
        normalFlat_.reset(new Texture());
        const u8 px[4] = {128, 128, 255, 255};
        normalFlat_->Create(1, 1, PixelFormat::RGBA8, px, TextureFilter::Nearest,
                            TextureWrap::ClampToEdge, false);
        normalFlat_->SetDebugName("normal-flat");
    }
    return normalFlat_.get();
}

int ResourceCache::ReloadChanged() {
    int reloaded = 0;
    for (auto& kv : textures_) {
        Entry<Texture>& e = kv.second;
        const i64 t = FS().FileTime(e.path);
        const bool srgb = kv.first.size() >= 5 && kv.first.compare(kv.first.size() - 5, 5, "|srgb") == 0;
        if (t != 0 && e.timestamp != 0 && t != e.timestamp && e.value->LoadFromFile(e.path, srgb)) {
            e.timestamp = t;
            ++reloaded;
        }
    }
    for (auto& kv : fonts_) {
        Entry<Font>& e = kv.second;
        const i64 t = FS().FileTime(e.path);
        if (t != 0 && e.timestamp != 0 && t != e.timestamp && e.value->LoadFromFile(e.path, e.value->Desc())) {
            e.timestamp = t;
            ++reloaded;
        }
    }
    for (auto& kv : models_) {
        Entry<Model>& e = kv.second;
        const i64 t = FS().FileTime(e.path);
        if (t != 0 && e.timestamp != 0 && t != e.timestamp && e.value->Load(e.path)) {
            e.value->UploadToGpu();
            e.timestamp = t;
            ++reloaded;
        }
    }
    for (auto& kv : audio_) {
        Entry<AudioClip>& e = kv.second;
        const i64 t = FS().FileTime(e.path);
        if (t != 0 && e.timestamp != 0 && t != e.timestamp && e.value->LoadFromFile(e.path)) {
            e.timestamp = t;
            ++reloaded;
        }
    }
    if (reloaded > 0) ENG_LOGI("res", "%d resources changed on disk", reloaded);
    return reloaded;
}

void ResourceCache::Clear() {
    textures_.clear();
    fonts_.clear();
    models_.clear();
    audio_.clear();
    shaders_.clear();
}

void ResourceCache::Shutdown() {
    Clear();
    white_.reset();
    black_.reset();
    normalFlat_.reset();
}

usize ResourceCache::Count() const {
    return textures_.size() + fonts_.size() + models_.size() + audio_.size() + shaders_.size();
}

}  // namespace crossrender
