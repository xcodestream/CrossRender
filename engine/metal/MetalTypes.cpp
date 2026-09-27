// Таблицы преобразования GL -> Metal. См. MetalTypes.h.
#include "MetalTypes.h"

#include "crossrender/gfx/GL.h"

namespace crossrender::mtlgl {

MGPixelFormat GLFormatToMTL(uint32_t internalFormat, uint32_t format, uint32_t type) {
    using gl::GLenum;
    switch (static_cast<uint32_t>(internalFormat)) {
        case gl::GL_R8: return MGPixelFormat::R8Unorm;
        case gl::GL_RG8: return MGPixelFormat::RG8Unorm;
        case gl::GL_RGBA8: return MGPixelFormat::RGBA8Unorm;
        case gl::GL_SRGB8_ALPHA8: return MGPixelFormat::RGBA8Unorm;
        case gl::GL_R16F: return MGPixelFormat::R16Float;
        case gl::GL_R32F: return MGPixelFormat::R32Float;
        case gl::GL_RGBA16F: return MGPixelFormat::RGBA16Float;
        case gl::GL_RGBA32F: return MGPixelFormat::RGBA32Float;
        case gl::GL_DEPTH_COMPONENT32F: return MGPixelFormat::Depth32Float;
        default: break;
    }
    // Безразмерные internal-форматы: классификация по (format, type).
    if (internalFormat == 0 || internalFormat == format || internalFormat == gl::GL_RGBA) {
        if (format == gl::GL_RGBA && type == gl::GL_UNSIGNED_BYTE) return MGPixelFormat::RGBA8Unorm;
        if (format == gl::GL_BGRA && type == gl::GL_UNSIGNED_BYTE) return MGPixelFormat::BGRA8Unorm;
        if (format == gl::GL_RGBA && type == gl::GL_HALF_FLOAT) return MGPixelFormat::RGBA16Float;
        if (format == gl::GL_RGBA && type == gl::GL_FLOAT) return MGPixelFormat::RGBA32Float;
        if (format == gl::GL_DEPTH_COMPONENT && type == gl::GL_FLOAT)
            return MGPixelFormat::Depth32Float;
    }
    if (internalFormat == gl::GL_RGB || format == gl::GL_RGB) return MGPixelFormat::RGB8Unorm;
    return MGPixelFormat::Invalid;
}

MGVertexFormat GLAttribFormatToMTL(uint32_t type, int32_t size, uint32_t normalized) {
    using gl::GLenum;
    if (type == gl::GL_FLOAT) {
        switch (size) {
            case 1: return MGVertexFormat::Float;
            case 2: return MGVertexFormat::Float2;
            case 3: return MGVertexFormat::Float3;
            case 4: return MGVertexFormat::Float4;
            default: return MGVertexFormat::Invalid;
        }
    }
    if (type == gl::GL_HALF_FLOAT) {
        if (size == 2) return MGVertexFormat::Half2;
        if (size == 4) return MGVertexFormat::Half4;
        return MGVertexFormat::Invalid;
    }
    if (type == gl::GL_UNSIGNED_BYTE && normalized) {
        if (size == 2) return MGVertexFormat::UChar2Norm;
        if (size == 4) return MGVertexFormat::UChar4Norm;
        return MGVertexFormat::Invalid;
    }
    if (type == gl::GL_UNSIGNED_INT && !normalized) {
        if (size == 1) return MGVertexFormat::UInt;
        if (size == 4) return MGVertexFormat::UInt4;
        return MGVertexFormat::Invalid;
    }
    return MGVertexFormat::Invalid;
}

MGPrimitiveType GLPrimitiveToMTL(uint32_t mode) {
    using gl::GLenum;
    switch (mode) {
        case gl::GL_POINTS: return MGPrimitiveType::Point;
        case gl::GL_LINES: return MGPrimitiveType::Line;
        case gl::GL_LINE_STRIP: return MGPrimitiveType::LineStrip;
        case gl::GL_TRIANGLE_STRIP: return MGPrimitiveType::TriangleStrip;
        case gl::GL_TRIANGLES:
        default: return MGPrimitiveType::Triangle;
    }
}

MGIndexType GLIndexTypeToMTL(uint32_t type) {
    return type == gl::GL_UNSIGNED_SHORT ? MGIndexType::UInt16 : MGIndexType::UInt32;
}

MGBlendFactor GLBlendFactorToMTL(uint32_t factor) {
    using gl::GLenum;
    switch (factor) {
        case gl::GL_ZERO: return MGBlendFactor::Zero;
        case gl::GL_ONE: return MGBlendFactor::One;
        case gl::GL_SRC_COLOR: return MGBlendFactor::SourceColor;
        case gl::GL_ONE_MINUS_SRC_COLOR: return MGBlendFactor::OneMinusSourceColor;
        case gl::GL_SRC_ALPHA: return MGBlendFactor::SourceAlpha;
        case gl::GL_ONE_MINUS_SRC_ALPHA: return MGBlendFactor::OneMinusSourceAlpha;
        case gl::GL_DST_COLOR: return MGBlendFactor::DestinationColor;
        case gl::GL_ONE_MINUS_DST_COLOR: return MGBlendFactor::OneMinusDestinationColor;
        case gl::GL_DST_ALPHA: return MGBlendFactor::DestinationAlpha;
        case gl::GL_ONE_MINUS_DST_ALPHA: return MGBlendFactor::OneMinusDestinationAlpha;
        case gl::GL_SRC_ALPHA_SATURATE: return MGBlendFactor::SourceAlphaSaturated;
        case gl::GL_CONSTANT_COLOR: return MGBlendFactor::ConstantColor;
        case gl::GL_ONE_MINUS_CONSTANT_COLOR: return MGBlendFactor::OneMinusConstantColor;
        default: return MGBlendFactor::One;
    }
}

MGBlendOperation GLBlendOpToMTL(uint32_t op) {
    using gl::GLenum;
    switch (op) {
        case gl::GL_FUNC_SUBTRACT: return MGBlendOperation::Subtract;
        case gl::GL_FUNC_REVERSE_SUBTRACT: return MGBlendOperation::ReverseSubtract;
        case gl::GL_MIN: return MGBlendOperation::Min;
        case gl::GL_MAX: return MGBlendOperation::Max;
        case gl::GL_FUNC_ADD:
        default: return MGBlendOperation::Add;
    }
}

MGCompareFunction GLCompareToMTL(uint32_t func) {
    using gl::GLenum;
    switch (func) {
        case gl::GL_NEVER: return MGCompareFunction::Never;
        case gl::GL_LESS: return MGCompareFunction::Less;
        case gl::GL_EQUAL: return MGCompareFunction::Equal;
        case gl::GL_LEQUAL: return MGCompareFunction::LessEqual;
        case gl::GL_GREATER: return MGCompareFunction::Greater;
        case gl::GL_NOTEQUAL: return MGCompareFunction::NotEqual;
        case gl::GL_GEQUAL: return MGCompareFunction::GreaterEqual;
        case gl::GL_ALWAYS:
        default: return MGCompareFunction::Always;
    }
}

bool MTLFormatSupported(MGPixelFormat format) {
    switch (format) {
        case MGPixelFormat::RGB8Unorm: return false;  // в Metal нет 24-битного цвета
        default: return format != MGPixelFormat::Invalid;
    }
}

}  // namespace crossrender::mtlgl
