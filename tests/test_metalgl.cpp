// Тесты слоя совместимости с Metal: чисто C++ конверсионное ядро (таблицы
// форматов/enum GL->Metal и рантайм-транслятор шейдеров GLSL->MSL) проверяется
// на всех платформах. GL-реализации поверх Metal требуют реального железа
// и покрываются запуском движка с CR_METAL=ON на устройствах Apple.
#include "MetalTypes.h"
#include "ShaderTranslate.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/test/Test.h"

using namespace crossrender;
using namespace crossrender::gl;
using namespace crossrender::mtlgl;

namespace {

const char* kVert = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uTransform;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uTransform * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kFrag = R"GLSL(
#version 330 core
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec4 uTint;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUV) * uTint;
}
)GLSL";

}  // namespace

ENG_TEST(MetalGL, FormatTablesMapToMetal) {
    ENG_CHECK(GLFormatToMTL(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) == MGPixelFormat::RGBA8Unorm);
    ENG_CHECK(GLFormatToMTL(GL_R8, GL_RED, GL_UNSIGNED_BYTE) == MGPixelFormat::R8Unorm);
    ENG_CHECK(GLFormatToMTL(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT) == MGPixelFormat::RGBA16Float);
    ENG_CHECK(GLFormatToMTL(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT) ==
              MGPixelFormat::Depth32Float);
    // Незаданный по размеру RGBA8-аплоад - обычный путь движка.
    ENG_CHECK(GLFormatToMTL(GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE) == MGPixelFormat::RGBA8Unorm);
    ENG_CHECK(MTLFormatSupported(MGPixelFormat::RGBA8Unorm));
    ENG_CHECK(!MTLFormatSupported(MGPixelFormat::RGB8Unorm));  // в Metal нет 24-бит

    ENG_CHECK(GLAttribFormatToMTL(GL_FLOAT, 2, GL_FALSE) == MGVertexFormat::Float2);
    ENG_CHECK(GLAttribFormatToMTL(GL_FLOAT, 4, GL_FALSE) == MGVertexFormat::Float4);
    ENG_CHECK(GLAttribFormatToMTL(GL_UNSIGNED_BYTE, 4, GL_TRUE) == MGVertexFormat::UChar4Norm);
    ENG_CHECK(GLAttribFormatToMTL(GL_FLOAT, 5, GL_FALSE) == MGVertexFormat::Invalid);

    ENG_CHECK(GLPrimitiveToMTL(GL_TRIANGLES) == MGPrimitiveType::Triangle);
    ENG_CHECK(GLPrimitiveToMTL(GL_TRIANGLE_STRIP) == MGPrimitiveType::TriangleStrip);
    ENG_CHECK(GLIndexTypeToMTL(GL_UNSIGNED_SHORT) == MGIndexType::UInt16);
    ENG_CHECK(GLBlendFactorToMTL(GL_SRC_ALPHA) == MGBlendFactor::SourceAlpha);
    ENG_CHECK(GLBlendOpToMTL(GL_FUNC_ADD) == MGBlendOperation::Add);
    ENG_CHECK(GLCompareToMTL(GL_LEQUAL) == MGCompareFunction::LessEqual);
}

ENG_TEST(MetalGL, VertexShaderTranslatesToMSL) {
    const MSLTranslation r = TranslateGLSLToMSL(kVert, ShaderStage::Vertex);
    ENG_CHECK_MSG(r.ok, r.error);
    ENG_CHECK(r.stage == ShaderStage::Vertex);
    ENG_CHECK(r.msl.find("vertex VSOut main0(") != std::string::npos);
    ENG_CHECK(r.msl.find("[[attribute(0)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("[[attribute(1)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("constant UniformsVS& _u [[buffer(16)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("float4x4 uTransform;") != std::string::npos);
    ENG_CHECK(r.msl.find("out.position = _u.uTransform") != std::string::npos);
    // Без переворота NDC по y: NDC Metal совпадает с GL для этого пайплайна.
    ENG_CHECK(r.msl.find("out.position.y = -out.position.y;") == std::string::npos);
    ENG_CHECK(r.msl.find("in_.aPos") != std::string::npos);
    ENG_CHECK(r.msl.find("return out;") != std::string::npos);
    ENG_CHECK(r.msl.find("#version") == std::string::npos);
    ENG_CHECK(r.attributes.size() == 2);
    ENG_CHECK(r.uniforms.size() == 1);
    ENG_CHECK(r.uniforms[0].bytes == 64);
}

ENG_TEST(MetalGL, FragmentShaderTranslatesToMSL) {
    const MSLTranslation r = TranslateGLSLToMSL(kFrag, ShaderStage::Fragment);
    ENG_CHECK_MSG(r.ok, r.error);
    ENG_CHECK(r.msl.find("fragment float4 main0(") != std::string::npos);
    ENG_CHECK(r.msl.find("texture2d<float> uTexture [[texture(0)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("sampler uTexture_Sm [[sampler(0)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("constant UniformsFS& _u [[buffer(16)]]") != std::string::npos);
    ENG_CHECK(r.msl.find("_u.uTint") != std::string::npos);
    ENG_CHECK(r.uniforms[1].offset >= 0);
    ENG_CHECK(r.msl.find("uTexture.sample(uTexture_Sm, in_.vUV)") != std::string::npos);
    ENG_CHECK(r.msl.find("return fragColor;") != std::string::npos);
    ENG_CHECK(r.uniforms.size() == 2);
    bool sawSampler = false;
    for (const MSLUniform& u : r.uniforms) sawSampler |= u.sampler;
    ENG_CHECK(sawSampler);
}

ENG_TEST(MetalGL, LegacyGlslIsNormalised) {
    const char* legacy = R"GLSL(
attribute vec4 aPos;
varying vec2 vUV;
void main() {
    vUV = aPos.xy;
    gl_Position = aPos;
}
)GLSL";
    const MSLTranslation r = TranslateGLSLToMSL(legacy, ShaderStage::Vertex);
    ENG_CHECK_MSG(r.ok, r.error);
    ENG_CHECK(r.msl.find("in_.aPos") != std::string::npos);
    ENG_CHECK(r.attributes.size() == 1);
}

ENG_TEST(MetalGL, UniformSizeTable) {
    ENG_CHECK(GLSLUniformSizeBytes("float", 0) == 4);
    ENG_CHECK(GLSLUniformSizeBytes("vec2", 0) == 8);
    ENG_CHECK(GLSLUniformSizeBytes("vec3", 0) == 12);
    ENG_CHECK(GLSLUniformSizeBytes("vec4", 0) == 16);
    ENG_CHECK(GLSLUniformSizeBytes("mat4", 0) == 64);
    // std140-шаг массива для vec3 равен 16.
    ENG_CHECK(GLSLUniformSizeBytes("vec3", 4) == 64);
}

ENG_TEST(MetalGL, MissingMainIsReported) {
    const MSLTranslation r = TranslateGLSLToMSL("uniform vec4 x;\n", ShaderStage::Vertex);
    ENG_CHECK(!r.ok);
    ENG_CHECK(!r.error.empty());
}
