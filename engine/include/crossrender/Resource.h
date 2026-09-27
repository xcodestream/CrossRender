//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: кэш ресурсов: единственный владелец загружаемых с диска текстур, мешей и шрифтов.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/audio/Audio.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/assets/Model.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace crossrender {

class ResourceCache {
public:
    ResourceCache() = default;
    ~ResourceCache() = default;

    // Все геттеры возвращают стабильный указатель, которым владеет кэш (не null,
    // если ресурс загрузился; nullptr при ошибке).
    Texture* Texture_(const std::string& path, bool srgb = false);
    Font* Font_(const std::string& path, const FontDesc& desc = {});
    Model* Model_(const std::string& path);
    AudioClip* Audio_(const std::string& path);
    // Шейдер из "<base>.vert"/"<base>.frag".
    Shader* Shader_(const std::string& basePath);
    // Шейдер из строк исходного кода (кэшируется по синтетическому ключу).
    Shader* ShaderFromSource(const std::string& key, const char* vert, const char* frag);
    // Процедурная однотонная текстура, используемая отладочной отрисовкой.
    Texture* WhiteTexture();
    Texture* BlackTexture();
    Texture* NormalFlatTexture();

    void SetAssetRoot(const std::string& root) { assetRoot_ = root; }
    [[nodiscard]] const std::string& AssetRoot() const { return assetRoot_; }
    // Перезагружает все файлы с изменившейся меткой времени (только десктоп).
    int ReloadChanged();
    void Clear();
    void Shutdown();
    [[nodiscard]] usize Count() const;

private:
    template <typename T>
    struct Entry {
        std::unique_ptr<T> value;
        i64 timestamp = 0;
        std::string path;
    };

    std::string assetRoot_;
    std::unordered_map<std::string, Entry<Texture>> textures_;
    std::unordered_map<std::string, Entry<Font>> fonts_;
    std::unordered_map<std::string, Entry<Model>> models_;
    std::unordered_map<std::string, Entry<AudioClip>> audio_;
    std::unordered_map<std::string, Entry<Shader>> shaders_;
    std::unique_ptr<Texture> white_, black_, normalFlat_;
};

}  // namespace crossrender
