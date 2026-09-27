//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: слой совместимости с Metal: соответствие вызовов gl:: и их эквивалентов в Metal.
//
#pragma once

#include "crossrender/core/Base.h"

#include <cstdint>

namespace crossrender::mtlgl {

// ---------------------------------------------------------------------------
// Форматы текстур: GL (internalFormat, format, type) -> MGPixelFormat.
// ---------------------------------------------------------------------------
struct GLFormatKey {
    uint32_t internalFormat;
    uint32_t format;
    uint32_t type;
};

enum class MGPixelFormat : uint8_t {
    Invalid,
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    BGRA8Unorm,
    R16Float,
    R32Float,
    RGBA16Float,
    RGBA32Float,
    Depth32Float,
    RGB8Unorm,  // аппроксимируется как RGBA8Unorm на Metal
};

// Сначала точные совпадения троек, затем разумные аппроксимации (RGB8 -> RGBA8).
[[nodiscard]] MGPixelFormat GLFormatToMTL(uint32_t internalFormat, uint32_t format,
                                           uint32_t type);

// ---------------------------------------------------------------------------
// Форматы вершинных атрибутов: (GL-тип, размер, normalized) -> MGVertexFormat.
// ---------------------------------------------------------------------------
enum class MGVertexFormat : uint8_t {
    Invalid,
    Float,
    Float2,
    Float3,
    Float4,
    Half2,
    Half4,
    UChar2Norm,
    UChar4Norm,
    UInt,
    UInt4,
};

MGVertexFormat GLAttribFormatToMTL(uint32_t type, int32_t size, uint32_t normalized);

// ---------------------------------------------------------------------------
// Словарь отрисовки и render pass.
// ---------------------------------------------------------------------------
enum class MGPrimitiveType : uint8_t { Point, Line, LineStrip, Triangle, TriangleStrip };
enum class MGIndexType : uint8_t { UInt16, UInt32 };
enum class MGCullMode : uint8_t { None, Back, Front };
enum class MGWinding : uint8_t { Clockwise, CounterClockwise };

MGPrimitiveType GLPrimitiveToMTL(uint32_t mode);
MGIndexType GLIndexTypeToMTL(uint32_t type);  // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT

// ---------------------------------------------------------------------------
// Факторы и операции блендинга (GL-перечисления -> Metal).
// ---------------------------------------------------------------------------
enum class MGBlendFactor : uint8_t {
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationColor,
    OneMinusDestinationColor,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    SourceAlphaSaturated,
    ConstantColor,
    OneMinusConstantColor,
};
enum class MGBlendOperation : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

MGBlendFactor GLBlendFactorToMTL(uint32_t factor);
MGBlendOperation GLBlendOpToMTL(uint32_t op);

// Функции сравнения (тесты depth / stencil): GL-перечисление -> функция сравнения Metal.
enum class MGCompareFunction : uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};
MGCompareFunction GLCompareToMTL(uint32_t func);

// Предикат проверки поддержки формата (слой использует его, чтобы честно сообщать о покрытии).
[[nodiscard]] bool MTLFormatSupported(MGPixelFormat format);

}  // namespace crossrender::mtlgl
