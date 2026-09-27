//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: стек пост-обработки: цепочка полноэкранных фильтров, накладываемых на кадр.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/RenderTarget.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

class Renderer2D;
class Shader;

enum class FilterType : u8 {
    Bloom = 0,
    Blur,               // сепарабельное блочное размытие, `radius` px
    GaussianBlur,       // сепарабельное гауссово размытие, `radius` px + `sigma`
    RadialBlur,         // размытие от точки `center`, сила `amount`
    ZoomBlur,
    MotionBlur,         // линейное размытие вдоль `direction`, сила `amount`
    Sharpen,
    EdgeDetect,         // Sobel
    Emboss,
    Pixelate,
    Posterize,
    Halftone,
    Dither,             // Байер / blue-noise
    Scanlines,
    Crt,                // кривизна + строки развёртки + маска
    Vignette,
    FilmGrain,
    ChromaticAberration,
    BarrelDistort,      // подушкообразное при amount < 0
    WaveDistort,        // синусоидальная рябь
    Glitch,             // блочное смещение + RGB-сплит
    Kaleidoscope,
    Fisheye,
    Swirl,
    ColorGrade,         // lift / gamma / gain + насыщенность + температура
    HueShift,
    Invert,
    Threshold,
    Sepia,
    Bleed,              // смаз с учётом краёв (дешёвая аппроксимация)
    Feedback,           // подмешивает предыдущий кадр (эхо/шлейфы)
    Count,
};

// Один блок параметров на все фильтры; каждый фильтр читает нужные ему поля
// и игнорирует остальные (описание по каждому фильтру в FilterChain.cpp).
struct FilterParams {
    f32 amount = 1.0f;          // общая сила / степень смешения
    f32 radius = 4.0f;          // радиус размытия в пикселях
    f32 sigma = 2.0f;
    Vec2 center{0.5f, 0.5f};    // нормализовано
    Vec2 direction{1.0f, 0.0f};
    f32 angle = 0.0f;           // радианы
    f32 frequency = 20.0f;      // частота искажения
    f32 amplitude = 0.01f;      // амплитуда искажения (в единицах UV)
    f32 threshold = 0.5f;
    f32 levels = 6.0f;          // уровни постеризации
    int cellSize = 6;           // ячейка пикселизации / полутонов
    f32 aspect = 1.0f;          // аспект источника для круглых искажений
    Color tint{1, 1, 1, 1};
    Color lift{0, 0, 0, 0};
    Color gain{1, 1, 1, 1};
    f32 gamma = 1.0f;
    f32 saturation = 1.0f;
    f32 temperature = 0.0f;
    f32 vignette = 0.35f;
    f32 grain = 0.04f;
    f32 scanlineStrength = 0.3f;
    f32 curvature = 0.06f;
    f32 aberration = 0.003f;
    f32 bloomThreshold = 1.0f;
    bool useThreshold = false;
    // Фильтр Feedback
    f32 feedback = 0.85f;
    // Калейдоскоп
    int segments = 6;
};

struct FilterInstance {
    FilterType type = FilterType::Blur;
    bool enabled = true;
    FilterParams params;
};

class FilterChain {
public:
    FilterChain();
    ~FilterChain();
    FilterChain(const FilterChain&) = delete;
    FilterChain& operator=(const FilterChain&) = delete;
// Поддержка перемещения: цепочка владеет GL-ресурсами через unique_ptr,
// поэтому перенос просто передаёт их. Нужно для возврата по значению из MakePreset().
    FilterChain(FilterChain&&) noexcept;
    FilterChain& operator=(FilterChain&&) noexcept;

    bool Init();
    void Shutdown();
    [[nodiscard]] bool Valid() const;

    void Clear() { filters_.clear(); }
    void Add(FilterType type, const FilterParams& params = {}, bool enabled = true);
    void Add(FilterInstance f) { filters_.push_back(std::move(f)); }
    void Remove(int index);
    void MoveUp(int index);
    void MoveDown(int index);
    [[nodiscard]] int Count() const { return static_cast<int>(filters_.size()); }
    [[nodiscard]] FilterInstance& At(int index) {
// Безопасный доступ: индексы вне диапазона возвращают запасной экземпляр
// вместо чтения за концом вектора.
        static FilterInstance fallback;
        if (index < 0 || index >= static_cast<int>(filters_.size())) return fallback;
        return filters_[static_cast<usize>(index)];
    }
    [[nodiscard]] const FilterInstance& At(int index) const {
        static const FilterInstance fallback;
        if (index < 0 || index >= static_cast<int>(filters_.size())) return fallback;
        return filters_[static_cast<usize>(index)];
    }
    [[nodiscard]] const std::vector<FilterInstance>& Filters() const { return filters_; }

// Применяет все включённые фильтры к `source`, записывая результат в
// `targetFbo` (0 = фреймбуфер по умолчанию).
    void Apply(const Texture& source, unsigned int targetFbo, int width, int height);
    // То же, но для render target (используется его цветовая текстура).
    void Apply(const RenderTarget& source, unsigned int targetFbo = 0);
    // Напрямую выполняет один фильтр (используется сеткой предпросмотра в демо).
    void ApplySingle(const Texture& source, const FilterInstance& filter, unsigned int targetFbo,
                     int width, int height);

// Рисует интерактивную сетку предпросмотра множества фильтров в текущую
// цель (используется демо-сценой постобработки).
    void PreviewGrid(const Texture& source, const std::vector<FilterType>& types, int columns,
                     int width, int height);

    // ---- метаданные ---------------------------------------------------------
    static const char* FilterName(FilterType t);
    static const char* FilterDescription(FilterType t);
    static std::vector<FilterType> AllFilters();
    // Блок параметров по умолчанию, с которым фильтр сразу выглядит хорошо.
    static FilterParams Defaults(FilterType t);

    // Именованные цепочки-пресеты, используемые демо.
    static FilterChain MakePreset(const std::string& name);
    static std::vector<std::string> PresetNames();

    struct Stats {
        int passes = 0;
        int pingPong = 0;
        bool usedFeedback = false;
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }

private:
    void EnsureTargets(int width, int height);
    void RunPass(const Texture& source, const FilterInstance& filter, RenderTarget& dst,
                 const Texture* history, int width, int height);
    void BlurPass(const Texture& source, RenderTarget& dst, const FilterParams& params, int width,
                  int height, bool gaussian);

    std::vector<FilterInstance> filters_;
    std::unique_ptr<RenderTarget> a_, b_;
    std::unique_ptr<RenderTarget> history_;
    std::unique_ptr<Shader> filterShader_;
    std::unique_ptr<Shader> blurShader_;
    std::unique_ptr<Shader> copyShader_;
    int width_ = 0, height_ = 0;
    bool initialized_ = false;
    Stats stats_{};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crossrender
