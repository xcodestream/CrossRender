//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: атласы спрайтов: дескрипторы, упаковка на страницы и отрисовка.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace crossrender {

// ---------------------------------------------------------------------------
// Одна область страницы атласа
// ---------------------------------------------------------------------------
struct AtlasRegion {
    std::string name;
    // Нормализованный прямоугольник внутри изображения страницы, начало слева сверху, y вниз.
    Rect uv;
    // Тот же прямоугольник в пикселях страницы.
    Rect frame;
    // Якорь внутри области (0,0 = левый верх, 1,1 = правый низ).
    Vec2 pivot{0.5f, 0.5f};
    // Пакер сохранил эту область повёрнутой на 90 градусов по часовой стрелке.
    bool rotated = false;
    // Исходное изображение было обрезано перед упаковкой.
    bool trimmed = false;
    // Размер необрезанного исходного изображения.
    Vec2 sourceSize{0, 0};
    // Где обрезанный прямоугольник находился внутри необрезанного исходника.
    Rect spriteSourceSize;
    // На какой странице атласа лежит область.
    int page = 0;
    // Длительность кадра Aseprite в миллисекундах (0, если формат её не хранит).
    f32 durationMs = 0.0f;
    // Опциональный полигон попаданий, нормализованный к области (пуст = весь прямоугольник).
    std::vector<Vec2> polygon;

    [[nodiscard]] bool Valid() const { return uv.w > 0.0f && uv.h > 0.0f && frame.w > 0.0f; }
    // Размер области в пикселях страницы с учётом поворота при упаковке.
    [[nodiscard]] Vec2 Size() const {
        return rotated ? Vec2{frame.h, frame.w} : Vec2{frame.w, frame.h};
    }
    // Необрезанный размер; если обрезки не было — размер после упаковки.
    [[nodiscard]] Vec2 OriginalSize() const {
        return sourceSize.x > 0.0f ? sourceSize : Size();
    }
};

enum class AtlasFormat : u8 {
    Unknown = 0,
    EngineJson,
    TexturePackerHash,
    TexturePackerArray,
    Aseprite,
    SparrowXml,
    LibGdx,
    PlainImage,
    Count,
};

// Именованная анимация: список индексов областей и длительности кадров.
struct AtlasAnimation {
    std::string name;
    std::vector<int> frames;
    std::vector<f32> durations;   // миллисекунды, параллельно `frames`
    bool loop = true;
    f32 TotalDuration() const;
    // Индекс кадра в момент `timeSeconds` с учётом длительностей кадров.
    int FrameAt(f32 timeSeconds) const;
};

struct AtlasPageDesc {
    std::string image;      // как записано в дескрипторе (обычно относительный)
    std::string resolved;   // фактический загруженный абсолютный путь
    int width = 0;
    int height = 0;
    Texture texture;
};

class SpriteAtlas {
public:
    SpriteAtlas();
    ~SpriteAtlas();
    SpriteAtlas(const SpriteAtlas&) = delete;
    SpriteAtlas& operator=(const SpriteAtlas&) = delete;
    SpriteAtlas(SpriteAtlas&&) noexcept;
    SpriteAtlas& operator=(SpriteAtlas&&) noexcept;

    // ---- загрузка ----------------------------------------------------------
// Загружает дескриптор и изображения его страниц. Относительные пути
// изображений разрешаются относительно каталога дескриптора. Возвращает
// false и заполняет Warnings(), если дескриптор отсутствует или не
// разбирается; дескриптор без изображений всё равно загружает свои области
// (отрисовка пропускается, Valid() равен true — данные доступны для изучения).
    bool LoadFromFile(const std::string& descriptorPath, bool srgb = false);
    // То же, из памяти. `baseDir` служит для разрешения относительных путей изображений.
    bool LoadFromMemory(const std::string& descriptor, const std::string& baseDir,
                        const std::string& debugName = "<memory>", bool srgb = false);
    // Считает целое изображение одной областью с именем по имени файла без расширения.
    bool LoadPlainImage(const std::string& imagePath, bool srgb = false);
    void Clear();

// Записывает дескриптор (JSON движка) в `path`. Изображения страниц пишутся
// рядом, если задан `writePageImages` и доступны пиксели на CPU.
    bool SaveToFile(const std::string& path, bool writePageImages = false) const;

    static AtlasFormat DetectFormat(const std::string& descriptor);
    static const char* FormatName(AtlasFormat f);

    // ---- сборка / упаковка ----------------------------------------------
    struct PackOptions {
        int maxSize = 2048;       // предел стороны страницы
        int padding = 2;          // пиксели между областями
        bool powerOfTwo = true;
        bool allowRotate = false; // упаковывать с поворотом, если экономит место
        bool trim = false;        // обрезать полностью прозрачные края
        Color background{0, 0, 0, 0};
    };
    // Упаковывает отдельные изображения в новый атлас (упаковщик shelf/skyline).
    bool BuildFromFiles(const std::vector<std::string>& imagePaths, const PackOptions& opts);
    bool BuildFromFiles(const std::vector<std::string>& imagePaths) {
        return BuildFromFiles(imagePaths, PackOptions{});
    }
    // Добавляет область вручную (так делают генератор ассетов и тесты).
    void AddRegion(const AtlasRegion& region);
// Объявляет изображение страницы без дескриптора (например, когда
// дескриптор только называет страницу, а изображение грузит вызывающий).
    bool SetPageImage(int page, const std::string& imagePath, bool srgb = false);
    // Сортирует области по имени (иначе порядок упаковки — порядок вставки).
    void SortRegionsByName();

    // ---- запросы ----------------------------------------------------------
    [[nodiscard]] bool Valid() const { return !regions_.empty() && !pages_.empty(); }
    [[nodiscard]] int RegionCount() const { return static_cast<int>(regions_.size()); }
    [[nodiscard]] int PageCount() const { return static_cast<int>(pages_.size()); }
    [[nodiscard]] const std::vector<AtlasRegion>& Regions() const { return regions_; }
    [[nodiscard]] const AtlasRegion& RegionAt(int index) const;
    [[nodiscard]] int IndexOf(const std::string& name) const;
    [[nodiscard]] bool Has(const std::string& name) const { return IndexOf(name) >= 0; }
    [[nodiscard]] const AtlasRegion* Find(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> Names() const;
    // Имена с префиксом `prefix` в естественном порядке ("run_2" перед "run_10").
    [[nodiscard]] std::vector<std::string> NamesWithPrefix(const std::string& prefix) const;
    [[nodiscard]] const std::vector<std::string>& Warnings() const { return warnings_; }
    [[nodiscard]] std::string SourcePath() const { return source_; }
    [[nodiscard]] AtlasFormat Format() const { return format_; }
    [[nodiscard]] const AtlasPageDesc& Page(int index) const;
    [[nodiscard]] const Texture& PageTexture(int index) const;
    // Текстура страницы, на которой лежит область (следует `region.page`).
    [[nodiscard]] const Texture& TextureFor(const AtlasRegion& region) const;

    // ---- отрисовка ----------------------------------------------------------
    // Рисует `name` в `dst` (область растягивается до нужного размера).
    bool Draw(Renderer2D& r2d, const std::string& name, const Rect& dst,
              const Color& tint = Color::White) const;
// Рисует `name` в естественном размере, позиционируя по `position` и pivot
// области (при pivot (0.5,0.5) её центр совпадает с `position`).
    bool DrawAnchored(Renderer2D& r2d, const std::string& name, Vec2 position,
                      const Color& tint = Color::White, f32 scale = 1.0f) const;
    // Рисует уже найденную область, при необходимости зеркально.
    bool DrawRegion(Renderer2D& r2d, int regionIndex, const Rect& dst, const Color& tint,
                    bool flipX = false, bool flipY = false) const;
    // Nine-patch: область нарезается с отступом `border` пикселей от каждого края.
    bool DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 border,
                       const Color& tint = Color::White) const;
    bool DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 left,
                       f32 top, f32 right, f32 bottom, const Color& tint = Color::White) const;
    // Плиточная заливка одной областью (для паттернов земли/фона).
    bool DrawTiled(Renderer2D& r2d, const std::string& name, const Rect& dst,
                   const Color& tint = Color::White) const;

    // ---- анимации -------------------------------------------------------
    [[nodiscard]] int AddAnimation(const AtlasAnimation& anim);
    [[nodiscard]] const AtlasAnimation* FindAnimation(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> AnimationNames() const;
// Группирует области с именами "<base><sep><number>" в анимации. Возвращает
// число созданных анимаций.
    int BuildAnimationsFromPrefixes(const std::string& separator = "_");
    [[nodiscard]] int AnimationFrameAt(const std::string& name, f32 timeSeconds) const;
    bool DrawAnimation(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 timeSeconds,
                       const Color& tint = Color::White) const;
    bool DrawAnimationFrame(Renderer2D& r2d, const std::string& name, int frame, const Rect& dst,
                            const Color& tint = Color::White) const;

    // ---- помощники на CPU (тесты, инструментарий, авторинг атласов) ---------------
    struct Stats {
        int regions = 0;
        int pages = 0;
        int totalPixels = 0;      // сумма площадей областей
        int pagePixels = 0;
        f32 occupancy = 0.0f;     // totalPixels / pagePixels
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    // Пиксели страницы на CPU, когда атлас собран (а не загружен) из изображений.
    [[nodiscard]] bool PagePixels(int page, std::vector<u8>* rgba, int* width, int* height) const;
    // Записывает страницу в PNG (требуется PagePixels()).
    bool SavePageImage(const std::string& pngPath, int page = 0) const;

private:
    void RecomputeStats();
    bool LoadPagesAndRegions(const std::string& descriptorText, const std::string& baseDir,
                             bool srgb);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<AtlasRegion> regions_;
    std::vector<AtlasPageDesc> pages_;
    std::unordered_map<std::string, int> lookup_;
    std::vector<AtlasAnimation> animations_;
    std::vector<std::string> warnings_;
    std::string source_;
    AtlasFormat format_ = AtlasFormat::Unknown;
    Stats stats_{};
};

}  // namespace crossrender
