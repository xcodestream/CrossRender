//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: текстуры: форматы пикселей, загрузка, режимы фильтрации и смешивания.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <string>
#include <vector>

namespace crossrender {

enum class PixelFormat : u8 {
    Unknown = 0,
    R8,
    RG8,
    RGB8,
    RGBA8,
    SRGBA8,
    R16F,
    RG16F,
    RGB16F,
    RGBA16F,
    R32F,
    RGBA32F,
    Depth16,
    Depth24,
    Depth24Stencil8,
    Depth32F,
    RGB10A2,
};

// Возвращает число байт на пиксель для несжатых форматов.
int PixelFormatSize(PixelFormat f);
bool PixelFormatIsDepth(PixelFormat f);
bool PixelFormatIsFloat(PixelFormat f);

enum class TextureFilter : u8 { Nearest, Linear, NearestMipmapNearest, LinearMipmapLinear };
enum class TextureWrap : u8 { Repeat, ClampToEdge, MirroredRepeat, ClampToBorder };

enum class BlendMode : u8 {
    None,          // без смешивания
    Alpha,         // стандартное src-alpha / one-minus-src-alpha (straight alpha)
    Premultiplied, // one / one-minus-src-alpha
    Additive,      // src-alpha / one
    Multiply,      // dst-color / zero
    Screen,        // one / one-minus-src-color
    Min,
    Max,
    Opaque,
};

struct BlendState {
    BlendMode mode = BlendMode::Alpha;
    bool operator==(const BlendState& o) const { return mode == o.mode; }
};

// GPU-текстура (2D, 3D или кубмап в зависимости от `depth`/`cube`).
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(Texture&& o) noexcept;
    Texture& operator=(Texture&& o) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Создаёт пустую (неинициализированную) текстуру.
    bool Create(int width, int height, PixelFormat format, const void* pixels = nullptr,
                TextureFilter filter = TextureFilter::Linear, TextureWrap wrap = TextureWrap::ClampToEdge,
                bool mipmaps = false);
    // Создаёт 3D-текстуру (используется воксельным рендерером).
    bool Create3D(int width, int height, int depth, PixelFormat format, const void* pixels = nullptr,
                  TextureFilter filter = TextureFilter::Nearest, TextureWrap wrap = TextureWrap::ClampToEdge);
    // Создаёт кубмап из 6 граней (порядок: +X -X +Y -Y +Z -Z).
    bool CreateCubemap(int size, PixelFormat format, const void* const faces[6],
                       TextureFilter filter = TextureFilter::Linear);
    // Загружает PNG/JPG/TGA/BMP/HDR с диска (stb_image).
    bool LoadFromFile(const std::string& path, bool srgb = false, bool mipmaps = true);
    // Загружает файл изображения из памяти.
    bool LoadFromMemory(const void* data, usize size, bool srgb = false, bool mipmaps = true);
    // Создаёт текстуру 1x1 из цвета (удобно для значений по умолчанию).
    bool CreateSolid(const Color& c);
    // Создаёт отладочную текстуру-шахматку.
    bool CreateCheckerboard(int size = 64, Color a = Color::FromRGB(0x333333),
                            Color b = Color::FromRGB(0x777777));
    void Destroy();

    void Update(const void* pixels, int x = 0, int y = 0, int w = -1, int h = -1);
    void Update3D(const void* pixels, int x, int y, int z, int w, int h, int d);
    void GenerateMipmaps();
    void SetFilter(TextureFilter f);
    void SetWrap(TextureWrap w);
    void SetAnisotropy(f32 level);

    [[nodiscard]] bool Valid() const { return id_ != 0; }
    [[nodiscard]] unsigned int Id() const { return id_; }
    [[nodiscard]] int Width() const { return width_; }
    [[nodiscard]] int Height() const { return height_; }
    [[nodiscard]] int Depth() const { return depth_; }
    [[nodiscard]] PixelFormat Format() const { return format_; }
    [[nodiscard]] bool IsCubemap() const { return cube_; }
    // Для атласов шрифтов: запоминает диапазон пикселей, используемый SDF-шрифтами.
    void SetSdfParams(f32 spread, f32 size) { sdfSpread_ = spread; sdfSize_ = size; }
    [[nodiscard]] f32 SdfSpread() const { return sdfSpread_; }
    [[nodiscard]] f32 SdfSize() const { return sdfSize_; }
    [[nodiscard]] const std::string& DebugName() const { return name_; }
    void SetDebugName(std::string n) { name_ = std::move(n); }

    // Данные изображения на CPU (заполняются только LoadFromFile по запросу).
    struct ImageData {
        int width = 0, height = 0, channels = 0;
        std::vector<u8> pixels;
    };
    // Декодирует файл изображения в память CPU, не затрагивая GPU.
    static bool DecodeImage(const void* data, usize size, ImageData* out, bool flipVertically = true);
    static bool DecodeImageFile(const std::string& path, ImageData* out, bool flipVertically = true);
    static bool EncodePng(const std::string& path, int w, int h, int channels, const void* pixels);

private:
    unsigned int id_ = 0;
    int width_ = 0, height_ = 0, depth_ = 1;
    PixelFormat format_ = PixelFormat::Unknown;
    bool cube_ = false;
    f32 sdfSpread_ = 0, sdfSize_ = 0;
    std::string name_;
};

}  // namespace crossrender
