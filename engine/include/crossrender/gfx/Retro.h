//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: ретро-режимы вывода: пиксель-арт виртуальный экран и ASCII-рендер.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/RenderTarget.h"

#include <string>
#include <vector>

namespace crossrender {

class Renderer2D;
class Font;

enum class RetroMode : u8 {
    Off,    // напрямую во фреймбуфер
    Pixel,  // низкое разрешение, целочисленный масштаб, квантование по палитре
    Ascii,  // низкое разрешение, преобразуется в сетку символов
};

// Классические аппаратные палитры (пиксельный режим и редактор палитр).
enum class RetroPalette : u8 {
    None,        // без квантования (true colour)
    Nes,         // NES/Famicom, 54 доступных цвета
    GameBoy,     // 4 оттенка зелёного
    GameBoyPocket,
    Cga16,       // CGA/EGA, 16 цветов
    Ega64,       // EGA, 64 цвета
    C64,         // Commodore 64, 16 цветов
    Pico8,       // PICO-8, 16 цветов
    Amstrad32,   // Amstrad CPC, 27 цветов
    ZxSpectrum,  // ZX Spectrum, 15 цветов
    Mono,        // 2 цвета
    VirtualBoy,  // 4 оттенка красного
    Count
};

enum class AsciiCharset : u8 {
    Ramp10,    // " .:-=+*#%@"
    Ramp70,    // полная шкала из 70 уровней
    Blocks,    // символы псевдографики Unicode
    Braille,   // точечные узоры Брайля (суб-ячейки 2x4)
    Custom,    // пользовательская шкала
    Count,
};

struct RetroSettings {
    RetroMode mode = RetroMode::Off;

    // ---- виртуальное разрешение ------------------------------------------------
    int virtualWidth = 320;
    int virtualHeight = 180;
// Целочисленное масштабирование сохраняет чёткость пикселей; при false
// изображение растягивается на окно с сохранением пропорций.
    bool integerScale = true;
    bool showOverscan = false;   // рисовать область за пределами виртуального изображения
    Color letterbox{0.02f, 0.02f, 0.03f, 1.0f};

    // ---- пиксельный режим --------------------------------------------------------
    RetroPalette palette = RetroPalette::None;
    bool dither = false;             // упорядоченный дизеринг по Байеру при квантовании
    int ditherMatrix = 4;            // 2, 4 или 8
    f32 ditherStrength = 1.0f;
    f32 colorDepth = 0.0f;           // 0 = только палитра, иначе 2..6 бит/канал
    bool scanlines = false;
    f32 scanlineStrength = 0.30f;
    f32 scanlineCount = 0.0f;        // 0 = вычислить из virtualHeight
    bool crtCurvature = false;
    f32 curvature = 0.06f;
    bool crtMask = false;            // RGB-маска aperture grille
    f32 crtMaskStrength = 0.25f;
    f32 bloom = 0.0f;                // простое 4-сэмпловое свечение
    f32 brightness = 1.0f;
    f32 contrast = 1.0f;
    f32 saturation = 1.0f;

    // ---- ascii-режим --------------------------------------------------------
    int asciiCols = 100;
    int asciiRows = 40;              // 0 = вычислить из пропорций
    AsciiCharset charset = AsciiCharset::Ramp10;
    std::string customRamp;          // UTF-8, от тёмного к светлому (charset == Custom)
    bool asciiColor = true;          // раскрашивать глифы по исходному пикселю
    bool asciiInvert = false;
    Color asciiInk{0.85f, 0.95f, 0.85f, 1.0f};
    Color asciiPaper{0.02f, 0.03f, 0.02f, 1.0f};
    f32 asciiGamma = 1.0f;
    f32 asciiContrast = 1.0f;
    f32 asciiBrightness = 0.0f;
    f32 asciiCellAspect = 0.5f;      // ширина / высота глифа
    bool asciiScanlines = false;
    f32 asciiScanlineStrength = 0.35f;
    bool asciiShowGrid = false;
    bool asciiBackgroundFill = true; // закрашивать фон цветом бумаги позади глифов
};

// ---------------------------------------------------------------------------
// RetroDisplay
// ---------------------------------------------------------------------------
class RetroDisplay {
public:
    RetroDisplay();
    ~RetroDisplay();
    RetroDisplay(const RetroDisplay&) = delete;
    RetroDisplay& operator=(const RetroDisplay&) = delete;

    bool Init();
    void Shutdown();
    // Пересоздаёт виртуальную цель при изменении фреймбуфера или настроек.
    void Resize(int fbWidth, int fbHeight, const RetroSettings& settings);
    [[nodiscard]] bool Valid() const;

// Привязывает виртуальную цель и подготавливает к ней 2D-рендерер. Возвращает
// логический прямоугольник экрана, относительно которого раскладывать сцену.
    Rect BeginFrame(Renderer2D& r2d, const RetroSettings& settings, f32 dpiScale);
    // Выводит виртуальное изображение в текущий привязанный фреймбуфер.
    void EndFrame(Renderer2D& r2d, const RetroSettings& settings, int fbWidth, int fbHeight);

// Виртуальная (низкого разрешения) цветовая цель — например, для эффектов,
// которым нужно сэмплировать уже нарисованное.
    [[nodiscard]] RenderTarget* VirtualTarget() { return virtual_.get(); }
    [[nodiscard]] const RenderTarget* VirtualTarget() const { return virtual_.get(); }
    [[nodiscard]] int VirtualWidth() const { return virtualW_; }
    [[nodiscard]] int VirtualHeight() const { return virtualH_; }

    // Куда ложится виртуальное изображение внутри фреймбуфера (в пикселях фреймбуфера).
    [[nodiscard]] Rect ViewportRect(int fbWidth, int fbHeight, const RetroSettings& s) const;
    // Переводит точку из координат фреймбуфера в виртуальные пиксели.
    [[nodiscard]] Vec2 MapToVirtual(Vec2 point, int fbWidth, int fbHeight, const RetroSettings& s) const;
    // Переводит виртуальные пиксели в координаты фреймбуфера.
    [[nodiscard]] Vec2 MapFromVirtual(Vec2 point, int fbWidth, int fbHeight, const RetroSettings& s) const;

    // Строит ASCII-атлас глифов из `font` (идемпотентно для пары шрифт+ячейка).
    bool BuildAsciiAtlas(Font* font, int cellW, int cellH);
    [[nodiscard]] bool AsciiAtlasReady() const { return asciiAtlas_.Valid(); }
    [[nodiscard]] const Texture& AsciiAtlas() const { return asciiAtlas_; }
    // Число ячеек глифов в сетке атласа (столбцы, строки).
    [[nodiscard]] int AsciiCellsX() const { return asciiCellsX_; }
    [[nodiscard]] int AsciiCellsY() const { return asciiCellsY_; }
    // Шкала для набора символов как кодпоинты UTF-8 (от тёмного к светлому).
    static std::vector<u32> CharsetRamp(AsciiCharset charset, const std::string& custom);

    // ---- палитры ----------------------------------------------------------
    static int PaletteSize(RetroPalette p);
    static Color PaletteColor(RetroPalette p, int index);
    // Ближайший цвет палитры (евклидово в RGB, с весами для восприятия).
    static Color Quantize(RetroPalette p, const Color& c);
    static const char* PaletteName(RetroPalette p);
    // Значение матрицы порогов Байера в [0,1) для пикселя (дизеринг).
    static f32 BayerThreshold(int x, int y, int matrixSize);

// Помощники на стороне CPU (используются тестами и headless-инструментами).
// Квантизация + дизеринг изображения RGBA8 на месте.
    static void ApplyPalette(u8* rgba, int width, int height, const RetroSettings& s);
    // Преобразует изображение RGBA8 в сетку яркости ASCII + цвета по ячейкам.
    struct AsciiGrid {
        int cols = 0, rows = 0;
        std::vector<u8> levels;    // 0..255 на ячейку
        std::vector<Color> colors; // цвет ячейки
        [[nodiscard]] int Index(int x, int y) const { return y * cols + x; }
    };
    static AsciiGrid BuildAsciiGrid(const u8* rgba, int width, int height, const RetroSettings& s);

    struct Stats {
        int virtualWidth = 0, virtualHeight = 0;
        int scale = 1;
        int paletteColors = 0;
        int asciiCols = 0, asciiRows = 0;
        bool usedAscii = false;
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }

private:
    std::unique_ptr<RenderTarget> virtual_;
    Texture asciiAtlas_;
    int asciiCellW_ = 0, asciiCellH_ = 0;
    int asciiCellsX_ = 0, asciiCellsY_ = 0;
    Font* asciiFont_ = nullptr;
    int asciiRampCount_ = 0;
    int virtualW_ = 0, virtualH_ = 0;
    int fbW_ = 0, fbH_ = 0;
    bool initialized_ = false;
    Stats stats_{};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Небольшой помощник демо-сцены, редактирующий 1D/2D-палитры: циклически
// переключает дизеринг, палитру и виртуальное разрешение без состояния движка.
struct RetroPreset {
    const char* name;
    RetroSettings settings;
};
std::vector<RetroPreset> BuiltinRetroPresets();

}  // namespace crossrender
