//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: трансляция шейдеров во время выполнения: GLSL -> Metal Shading Language.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>
#include <vector>

namespace crossrender::mtlgl {

enum class ShaderStage : uint8_t { Vertex, Fragment };

struct MSLUniform {
    std::string name;
    std::string glslType;   // как записано в GLSL, напр. "vec4", "mat4", "sampler2D"
    bool sampler = false;   // uniform-сэмплеры становятся аргументами texture+sampler
    int arraySize = 0;      // 0 = не массив
    int binding = 0;        // слот [[buffer(k)]] или [[texture(i)]]/[[sampler(i)]]
    int bytes = 0;          // размер в байтах в staging (0 для сэмплеров)
    int offset = 0;         // смещение в байтах внутри uniform-блока стадии
};

struct MSLAttribute {
    std::string name;
    std::string glslType;
    int location = 0;
};

struct MSLTranslation {
    bool ok = false;
    std::string msl;                // транслированный исходник; точка входа "main0"
    std::string error;              // диагностика при !ok
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<MSLUniform> uniforms;
    std::vector<MSLAttribute> attributes;  // только вершинная стадия
    int uniformsBlockBytes = 0;  // размер uniform-блока стадии (0 = нет)
};

// Транслирует одну стадию шейдера. Принимает GLSL движка: современный in/out
// с явным layout(location) для атрибутов, отдельные uniform-ы, один
// фрагментный выход и опционально преамбулу `#version`/precision (она
// игнорируется - метаданные привязок берутся из объявлений).
MSLTranslation TranslateGLSLToMSL(const std::string& glsl, ShaderStage stage);

// Размер в байтах staged (не-сэмплерного) uniform-а данного GLSL-типа
// с размерами в духе std140. Массивы умножаются на число элементов.
int GLSLUniformSizeBytes(const std::string& type, int arraySize);

}  // namespace crossrender::mtlgl
