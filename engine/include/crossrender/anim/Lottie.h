//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: проигрыватель векторной анимации Lottie (bodymovin JSON) без внешних библиотек.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Json.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Renderer2D.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

class Font;

// Универсальное анимируемое свойство: либо статичное значение, либо ключевые кадры.
struct LottieProperty {
    enum class Kind : u8 { Scalar, Vec2, Vec3, Color, Path, Gradient };
    Kind kind = Kind::Scalar;
    // Статичное значение
    f32 scalar = 0;
    Vec2 vec2;
    Vec3 vec3;
    Color color;
    std::vector<Vec2> pathPoints;
    bool animated = false;
    // Ключевые кадры (время в секундах, уже поделено на частоту кадров).
    struct Key {
        f32 time = 0;
        f32 value = 0;
        Vec2 v2;
        Vec3 v3;
        Color col;
        std::vector<Vec2> pts;
        std::string easingIn, easingOut;  // "linear" или "x1,y1,x2,y2"
        bool hold = false;
    };
    std::vector<Key> keys;
    // Пространственные bezier-касательные для ключей позиции.
    struct SpatialTangent {
        Vec2 inTangent, outTangent;
    };
    std::vector<SpatialTangent> spatial;

    [[nodiscard]] f32 EvaluateScalar(f32 time) const;
    [[nodiscard]] Vec2 EvaluateVec2(f32 time) const;
    [[nodiscard]] Color EvaluateColor(f32 time) const;
    [[nodiscard]] std::vector<Vec2> EvaluatePath(f32 time) const;
};

struct LottieTransform {
    LottieProperty anchor;
    LottieProperty position;
    LottieProperty scale;      // проценты
    LottieProperty rotation;   // градусы
    LottieProperty opacity;    // 0..100
    LottieProperty skew, skewAxis;
    // Вычисляется в заданный момент времени.
    struct Result {
        Vec2 anchor{0, 0};
        Vec2 position{0, 0};
        Vec2 scale{100, 100};
        f32 rotation = 0;
        f32 opacity = 100;
        f32 skew = 0, skewAxis = 0;
        Mat4 Matrix() const;
    };
    [[nodiscard]] Result Evaluate(f32 time) const;
    [[nodiscard]] Mat4 Matrix(f32 time) const;
};

enum class LottieLayerType : u8 { Precomp, Solid, Image, Null, Shape, Text, Audio, Unknown };

struct LottieShape {
    enum class Type : u8 {
        Group, Rectangle, Ellipse, Path, PolyStar, Fill, Stroke, GradientFill, GradientStroke,
        Transform, Merge, Trim, Repeater, RoundedCorners, Unknown
    };
    Type type = Type::Unknown;
    std::string name;
    bool hidden = false;
    LottieTransform transform;
    // Геометрия
    LottieProperty size, position, roundness, direction, innerRadius, outerRadius, points, rotation,
        starType;
    LottieProperty start, end, offset;  // trim
    LottieProperty copies;              // repeater
    LottieProperty pathData;
    // Стиль
    LottieProperty colour, strokeWidth, strokeOpacity, fillOpacity, opacity;
    LottieProperty dashLength, dashGap;
    int fillRule = 1;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    f32 miterLimit = 4;
    // Градиенты
    struct GradientStop {
        f32 offset = 0;
        Color color;
    };
    std::vector<GradientStop> gradientStops;
    LottieProperty gradientStart, gradientEnd;
    int gradientType = 1;  // 1 линейный, 2 радиальный
    std::vector<std::unique_ptr<LottieShape>> items;  // для групп
    // Состояние trim path (применяется плеером)
    bool trimEnabled = false;
};

struct LottieMask {
    LottieProperty path;
    int mode = 'a';  // a=добавление, s=вычитание, i=пересечение, n=нет
    bool inverted = false;
    f32 opacity = 100;
};

struct LottieLayer {
    int index = 0;
    int parent = -1;
    std::string name;
    LottieLayerType type = LottieLayerType::Shape;
    f32 startTime = 0, inTime = 0, outTime = 0;
    f32 timeStretch = 1.0f;
    int precompIndex = -1;
    bool hidden = false;
    f32 width = 0, height = 0;      // solid
    Color solidColor = Color::White;
    std::string imageName;          // слой изображения: ссылка на ассет
    int imageAssetIndex = -1;
    LottieTransform transform;
    std::vector<LottieShape> shapes;
    std::vector<LottieMask> masks;
    int matteLayer = -1;            // источник track matte
    int matteMode = 0;              // 0 нет, 1 alpha, 2 инвертированный alpha, 3 luma, 4 инвертированный luma
    bool hasMask = false;
    // Текстовый слой
    std::string text;
    std::string fontFamily;
    f32 fontSize = 24;
    Color textColor = Color::White;
    f32 textTracking = 0, textLineHeight = 0;
    TextAlign textAlign = TextAlign::Left;
    int justification = 0;
    bool strokeOverFill = false;
    Color textStrokeColor = Color::Black;
    f32 textStrokeWidth = 0;
    // Псевдо-3D флаги (3D-слои Lottie проецируются как 2D)
    bool is3D = false;
    Vec3 positionZ;
};

struct LottieAsset {
    int id = -1;
    std::string name;
    int width = 0, height = 0;
    bool isPrecomp = false;
    std::vector<LottieLayer> layers;
    std::string imagePath;
    std::vector<u8> imageData;   // байты изображения, декодированные из base64 (при встраивании)
};

// ---------------------------------------------------------------------------
// LottieAnimation: неизменяемая разобранная композиция
// ---------------------------------------------------------------------------
class LottieAnimation {
public:
    LottieAnimation();
    ~LottieAnimation();
    LottieAnimation(LottieAnimation&&) noexcept;
    LottieAnimation& operator=(LottieAnimation&&) noexcept;

    bool LoadFromFile(const std::string& path, std::string* error = nullptr);
    bool LoadFromJson(const std::string& json, std::string* error = nullptr);
    bool Parse(const JsonValue& root, std::string* error = nullptr);
    void Destroy();

    [[nodiscard]] bool Valid() const { return valid_; }
    [[nodiscard]] const std::string& Name() const { return name_; }
    [[nodiscard]] f32 Duration() const { return duration_; }   // секунды
    [[nodiscard]] f32 FrameRate() const { return frameRate_; }
    [[nodiscard]] int TotalFrames() const { return totalFrames_; }
    [[nodiscard]] int Width() const { return width_; }
    [[nodiscard]] int Height() const { return height_; }
    [[nodiscard]] int LayerCount() const { return static_cast<int>(layers_.size()); }
    [[nodiscard]] const std::vector<LottieLayer>& Layers() const { return layers_; }
    [[nodiscard]] const std::vector<LottieAsset>& Assets() const { return assets_; }
    [[nodiscard]] const JsonValue& Raw() const { return raw_; }
    [[nodiscard]] usize MemoryUsage() const;

    // ---- состояние плеера во время выполнения ----------------------------
    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop) { loop_ = loop; }
    [[nodiscard]] bool Looping() const { return loop_; }
    void SetFrame(f32 frame);
    void SetTime(f32 seconds);
    void SetSpeed(f32 speed) { speed_ = speed; }
    [[nodiscard]] f32 Speed() const { return speed_; }
    void SetSegment(f32 startFrame, f32 endFrame);
    [[nodiscard]] f32 CurrentFrame() const { return currentFrame_; }
    [[nodiscard]] f32 CurrentTime() const { return currentFrame_ / (frameRate_ > 0 ? frameRate_ : 1.0f); }
    [[nodiscard]] f32 Progress() const;
    [[nodiscard]] bool Playing() const { return playing_; }
    [[nodiscard]] bool Finished() const { return finished_; }
    [[nodiscard]] int LoopCount() const { return loopCount_; }
    void Advance(f32 dt);
    void Update(f32 dt) { Advance(dt); }  // устаревший псевдоним

    // Отрисовывает текущий фрейм в `dst` через Renderer2D.
    // `alpha` умножает прозрачность всех слоёв; `tint` умножает цвета заливки.
    void Render(Renderer2D& r, const Rect& dst, f32 alpha = 1.0f,
                const Color& tint = Color::White) const;
    // Отрисовывает с трансформацией в `position` и масштабом до `size`.
    void RenderAt(Renderer2D& r, const Vec2& position, const Vec2& size, f32 rotation = 0,
                  f32 alpha = 1.0f) const;

    // Регистрирует загрузчик изображений, чтобы слои изображений могли подгружать внешние ассеты.
    using ImageLoader = std::function<const Texture*(const std::string& name)>;
    void SetImageLoader(ImageLoader loader) { imageLoader_ = std::move(loader); }
    void SetFont(Font* font) { font_ = font; }
    [[nodiscard]] Font* GetFont() const { return font_; }

    // Число глифов/фигур, отрисованных при последнем вызове Render (профилирование).
    struct RenderStats {
        int layersDrawn = 0;
        int shapesDrawn = 0;
        int masksApplied = 0;
        int imagesDrawn = 0;
        int textDrawn = 0;
    };
    [[nodiscard]] const RenderStats& LastRenderStats() const { return stats_; }

    // Проверяет корректность разбора (используется тестами).
    struct ValidationResult {
        bool ok = true;
        std::vector<std::string> errors;
    };
    [[nodiscard]] ValidationResult Validate() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    JsonValue raw_;
    bool valid_ = false;
    std::string name_;
    int width_ = 0, height_ = 0;
    f32 frameRate_ = 60, duration_ = 0;
    int totalFrames_ = 0;
    std::vector<LottieLayer> layers_;
    std::vector<LottieAsset> assets_;
    f32 currentFrame_ = 0;
    f32 speed_ = 1.0f;
    f32 segStart_ = 0, segEnd_ = -1;
    bool playing_ = false, loop_ = true, finished_ = false;
    int loopCount_ = 0;
    ImageLoader imageLoader_;
    Font* font_ = nullptr;
    mutable RenderStats stats_{};
};

// ---------------------------------------------------------------------------
// Библиотека Lottie: кэш + процедурная генерация анимаций
// ---------------------------------------------------------------------------
class LottieLibrary {
public:
    static LottieLibrary& Get();
    // Загружает и кэширует по пути.
    LottieAnimation* Load(const std::string& path);
    // Возвращает процедурно сгенерированную анимацию (записывается в папку user,
    // чтобы пример работал без поставки бинарных ассетов).
    LottieAnimation* LoadOrGenerate(const std::string& name);
    void Clear();

private:
    LottieLibrary() = default;
    std::vector<std::unique_ptr<LottieAnimation>> owned_;
    std::unordered_map<std::string, LottieAnimation*> cache_;
};

// Генерирует совместимый с bodymovin JSON для встроенных демо-анимаций.
// Имена: "loading", "success", "heart", "checkmark", "pulse", "card-flip"
std::string GenerateLottieJson(const std::string& name);

}  // namespace crossrender
