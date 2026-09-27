// Трансляция GLSL -> MSL для шейдерного диалекта движка. См. ShaderTranslate.h.
#include "ShaderTranslate.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace crossrender::mtlgl {
namespace {

using TokenMap = std::unordered_map<std::string, std::string>;

// Замена по целым словам; всё остальное в строке сохраняется.
std::string ReplaceWord(const std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    const auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    while (pos <= text.size()) {
        const size_t hit = text.find(from, pos);
        if (hit == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        const bool leftOk = hit == 0 || !isWord(text[hit - 1]);
        const size_t right = hit + from.size();
        const bool rightOk = right >= text.size() || !isWord(text[right]);
        if (leftOk && rightOk) {
            out.append(text, pos, hit - pos);
            out.append(to);
            pos = right;
        } else {
            out.append(text, pos, right - pos);
            pos = right;
        }
    }
    return out;
}

// Тип GLSL -> тип MSL (перезапись токенов по границам слов).
std::string RewriteTypes(const std::string& text) {
    static const TokenMap kTypes = {
        {"vec2", "float2"}, {"vec3", "float3"}, {"vec4", "float4"},
        {"ivec2", "int2"}, {"ivec3", "int3"}, {"ivec4", "int4"},
        {"uvec2", "uint2"}, {"uvec3", "uint3"}, {"uvec4", "uint4"},
        {"bvec2", "bool2"}, {"bvec3", "bool3"}, {"bvec4", "bool4"},
        {"mat2", "float2x2"}, {"mat3", "float3x3"}, {"mat4", "float4x4"},
    };
    std::string out = text;
    for (const auto& [from, to] : kTypes) out = ReplaceWord(out, from, to);
    return out;
}

// Встроенные функции GLSL без точного одноимённика в MSL.
std::string RewriteBuiltins(const std::string& text) {
    std::string out = ReplaceWord(text, "inversesqrt", "rsqrt");
    out = ReplaceWord(out, "mod", "fmod");
    out = ReplaceWord(out, "discard;", "discard_fragment();");
    out = ReplaceWord(out, "discard ;", "discard_fragment();");
    return out;
}

// Устаревший GLSL (attribute/varying/texture2D/gl_FragColor) -> современный
// диалект, который ожидает остальной конвертер. Дёшево и идемпотентно на современных исходниках.
std::string NormalizeLegacy(const std::string& glsl, ShaderStage stage) {
    std::string out = glsl;
    if (stage == ShaderStage::Vertex) {
        out = ReplaceWord(out, "attribute", "in");
        out = ReplaceWord(out, "varying", "out");
    } else {
        out = ReplaceWord(out, "varying", "in");
    }
    out = ReplaceWord(out, "texture2D(", "texture(");
    out = ReplaceWord(out, "texture2D (", "texture(");
    return out;
}

// "vec4 name[8]" -> type="vec4", name="name", array=8 (обрабатывает и простой вид).
bool SplitDeclaration(const std::string& decl, std::string* type, std::string* name, int* array) {
    std::istringstream stream(decl);
    std::string first, second;
    if (!(stream >> first)) return false;
    *array = 0;
    if (stream >> second) {
        *type = first;
        // second несёт имя, опционально "name[N]".
        const size_t bracket = second.find('[');
        if (bracket != std::string::npos) {
            *name = second.substr(0, bracket);
            const size_t close = second.find(']', bracket);
            if (close != std::string::npos) {
                *array = std::atoi(second.substr(bracket + 1, close - bracket - 1).c_str());
            }
        } else {
            *name = second;
        }
        return !name->empty();
    }
    return false;
}

int BaseUniformSizeBytes(const std::string& glslType) {
    // Размеры в духе std140; каждый uniform живёт в собственном выровненном
    // чанке, поэтому о правилах паддинга vec3/mat заботятся только шаги массивов.
    if (glslType == "float" || glslType == "int" || glslType == "uint" || glslType == "bool") return 4;
    if (glslType == "vec2" || glslType == "ivec2" || glslType == "uvec2" || glslType == "bvec2") return 8;
    if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3" || glslType == "bvec3") return 12;
    if (glslType == "vec4" || glslType == "ivec4" || glslType == "uvec4" || glslType == "bvec4") return 16;
    if (glslType == "mat2") return 32;
    if (glslType == "mat3") return 48;
    if (glslType == "mat4") return 64;
    return 16;
}

int Std140Stride(const std::string& glslType) {
    if (glslType == "vec3" || glslType == "ivec3" || glslType == "uvec3" || glslType == "bvec3") return 16;
    const int base = BaseUniformSizeBytes(glslType);
    return base;
}

// Захватывает сбалансированный блок {...}, начинающийся с openBrace.
std::string CaptureBlock(const std::string& text, size_t openBrace) {
    int depth = 0;
    for (size_t i = openBrace; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        if (text[i] == '}') {
            --depth;
            if (depth == 0) return text.substr(openBrace + 1, i - openBrace - 1);
        }
    }
    return {};
}

struct Declarations {
    std::vector<MSLAttribute> attributes;
    std::vector<std::string> varyingOutNames, varyingOutTypes;    // vertex -> struct
    std::vector<std::string> varyingInNames, varyingInTypes;      // fragment <- struct
    std::string fragmentOutputName, fragmentOutputType = "vec4";
    bool hasFragmentOutput = false;
};

}  // namespace

// Перезаписывает texelFetch(tex, coord, lod) -> tex.read((uint2)(coord)) для 2D
// текстур (lod должен быть 0 - единственный случай, который использует движок).
std::string RewriteTexelFetch(const std::string& text) {
    std::string out = text;
    size_t pos = 0;
    while (true) {
        const size_t hit = out.find("texelFetch(", pos);
        if (hit == std::string::npos) break;
        // Разделяем три аргумента по запятым нулевой глубины.
        size_t i = hit + 11;
        int depth = 1;
        int arg = 0;
        std::string args[3];
        size_t end = std::string::npos;
        for (; i < out.size(); ++i) {
            const char c = out[i];
            if (c == '(') { ++depth; args[arg] += c; }
            else if (c == ')') {
                if (depth == 1) { end = i; break; }
                --depth; args[arg] += c;
            }
            else if (c == ',' && depth == 1) {
                if (++arg > 2) { end = i; break; }
            }
            else if (arg < 3) args[arg] += c;
        }
        if (end == std::string::npos || arg < 2) { pos = hit + 11; continue; }
        const std::string tex = args[0];
        const std::string coord = args[1];
        std::string replacement = tex + ".read((uint2)(" + coord + "))";
        out = out.substr(0, hit) + replacement + out.substr(i + 1);
        pos = hit + replacement.size();
    }
    return out;
}

// Добавляет `, extra` к каждому вызову указанных функций (скан по сбалансированным скобкам).
std::string AppendCallArg(const std::string& text, const std::string& name,
                          const std::string& extra) {
    std::string out = text;
    const std::string callHead = name + "(";
    size_t pos = 0;
    while (true) {
        const size_t hit = out.find(callHead, pos);
        if (hit == std::string::npos) break;
        if (hit > 0) {
            const char prev = out[hit - 1];
            if (std::isalnum((unsigned char)prev) || prev == '_') { pos = hit + name.size(); continue; }
        }
        size_t i = hit + callHead.size();
        int depth = 1;
        for (; i < out.size(); ++i) {
            if (out[i] == '(') ++depth;
            if (out[i] == ')') { --depth; break; }
        }
        if (i >= out.size()) break;
        out.insert(i, ", " + extra);
        pos = i + extra.size() + 2;
    }
    return out;
}

int GLSLUniformSizeBytes(const std::string& type, int arraySize) {
    if (arraySize > 0) return Std140Stride(type) * arraySize;
    return BaseUniformSizeBytes(type);
}

MSLTranslation TranslateGLSLToMSL(const std::string& glsl, ShaderStage stage) {
    MSLTranslation result;
    result.stage = stage;

    std::string src = NormalizeLegacy(glsl, stage);

    Declarations decls;
    std::string preambleCode;  // вспомогательные функции и т.п.
    std::string mainBody;
    bool inMain = false, mainFound = false;

    size_t pos = 0;
    while (pos <= src.size()) {
        const size_t lineStart = pos;
        const size_t eol = src.find('\n', pos);
        std::string line = src.substr(lineStart, eol == std::string::npos ? std::string::npos
                                                                          : eol - lineStart);
        pos = eol == std::string::npos ? src.size() + 1 : eol + 1;

        // Обрезка.
        const size_t first = line.find_first_not_of(" \t\r");
        std::string trimmed = first == std::string::npos ? "" : line.substr(first);
        trimmed = trimmed.substr(0, trimmed.find_last_not_of(" \t\r") + 1);

        if (!inMain) {
            // Строки преамбулы, которые MSL-стороне не нужны.
            if (trimmed.empty() || trimmed.rfind("#version", 0) == 0 ||
                trimmed.rfind("precision", 0) == 0 || trimmed.rfind("#extension", 0) == 0 ||
                trimmed.rfind("#pragma", 0) == 0 || trimmed.rfind("#define ENG_GLES", 0) == 0) {
                continue;
            }
            if (trimmed.rfind("layout(", 0) == 0 && trimmed.find(") in ") != std::string::npos &&
                stage == ShaderStage::Vertex) {
                // layout(location = N) in TYPE name;
                const size_t close = trimmed.find(')', 7);
                const size_t eq = trimmed.find('=', 7);
                const int location = std::atoi(trimmed.substr(eq + 1, close - eq - 1).c_str());
                std::string rest = trimmed.substr(close + 2);
                rest = rest.substr(0, rest.find(';'));
                if (rest.rfind("in ", 0) == 0) rest = rest.substr(3);
                std::string type, name;
                int array = 0;
                if (SplitDeclaration(rest, &type, &name, &array) && array == 0) {
                    decls.attributes.push_back({name, type, location});
                }
                continue;
            }
            auto startsWith = [&](const char* p) { return trimmed.rfind(p, 0) == 0; };
            auto stripSemi = [&](std::string s) {
                const size_t semi = s.find(';');
                return semi == std::string::npos ? s : s.substr(0, semi);
            };
            if (startsWith("in ") && stage == ShaderStage::Vertex) {
                // Устаревший атрибут без явной локации (attribute /
                // голое "in"): назначаем следующий свободный слот.
                std::string type, name;
                int array = 0;
                if (SplitDeclaration(stripSemi(trimmed.substr(3)), &type, &name, &array) &&
                    array == 0) {
                    int location = 0;
                    bool taken = true;
                    while (taken) {
                        taken = false;
                        for (const MSLAttribute& a : decls.attributes) {
                            if (a.location == location) {
                                taken = true;
                                ++location;
                            }
                        }
                    }
                    decls.attributes.push_back({name, type, location});
                }
                continue;
            }
            if (startsWith("in ") && stage == ShaderStage::Fragment) {
                std::string type, name;
                int array = 0;
                if (SplitDeclaration(stripSemi(trimmed.substr(3)), &type, &name, &array)) {
                    decls.varyingInNames.push_back(name);
                    decls.varyingInTypes.push_back(type);
                }
                continue;
            }
            if (startsWith("out ")) {
                std::string type, name;
                int array = 0;
                if (SplitDeclaration(stripSemi(trimmed.substr(4)), &type, &name, &array)) {
                    if (stage == ShaderStage::Vertex) {
                        decls.varyingOutNames.push_back(name);
                        decls.varyingOutTypes.push_back(type);
                    } else if (!decls.hasFragmentOutput) {
                        decls.hasFragmentOutput = true;
                        decls.fragmentOutputName = name;
                        decls.fragmentOutputType = type;
                    }
                }
                continue;
            }
            if (startsWith("uniform ")) {
                std::string rest = stripSemi(trimmed.substr(8));
                std::string type, name;
                int array = 0;
                if (SplitDeclaration(rest, &type, &name, &array)) {
                    MSLUniform u;
                    u.name = name;
                    u.glslType = type;
                    u.sampler = type.rfind("sampler", 0) == 0;
                    u.arraySize = array;
                    u.bytes = u.sampler ? 0 : GLSLUniformSizeBytes(type, array);
                    result.uniforms.push_back(u);
                }
                continue;
            }
            if (trimmed.find("void main()") != std::string::npos) {
                const size_t brace = src.find('{', lineStart);
                if (brace == std::string::npos) {
                    result.error = "main() has no body";
                    return result;
                }
                mainBody = CaptureBlock(src, brace);
                inMain = true;
                mainFound = true;
                // Перескакиваем за захваченный блок.
                size_t depth = 0;
                size_t i = brace;
                for (; i < src.size(); ++i) {
                    if (src[i] == '{') ++depth;
                    if (src[i] == '}') {
                        --depth;
                        if (depth == 0) break;
                    }
                }
                pos = i + 1;
                continue;
            }
            // Код хелперов: сохраняем, перезаписи типов/встроенных применяются позже.
            preambleCode += line + "\n";
            continue;
        }
    }

    if (!mainFound) {
        result.error = "no main() found";
        return result;
    }

    // Назначение привязок: текстуры получают последовательные [[texture(i)]].
    // Все не-сэмплерные uniform-ы упаковываются в ОДИН constant-буфер на стадию
    // (единственный аргумент [[buffer(16)]]), что держит нас далеко ниже лимита
    // индексов буферов Metal даже для шейдеров с десятками uniform-ов.
    constexpr int kUniformBufferSlot = 16;
    int textureSlot = 0;
    int blockOffset = 0;
    for (MSLUniform& u : result.uniforms) {
        if (u.sampler) {
            u.binding = textureSlot++;
            continue;
        }
        const int align = u.glslType == "vec3" || u.glslType.rfind("mat", 0) == 0 ? 16
                          : std::min(GLSLUniformSizeBytes(u.glslType, 0), 16);
        blockOffset = (blockOffset + align - 1) / align * align;
        u.offset = blockOffset;
        blockOffset += GLSLUniformSizeBytes(u.glslType, u.arraySize);
        u.binding = kUniformBufferSlot;
    }
    result.uniformsBlockBytes = blockOffset;

    // ---- перезаписи тела ----------------------------------------------------
    std::string body = mainBody;
    body = RewriteTypes(body);
    body = RewriteBuiltins(body);
    preambleCode = RewriteTypes(preambleCode);
    preambleCode = RewriteBuiltins(preambleCode);
    preambleCode = RewriteTexelFetch(preambleCode);

    // texture(sampler, ...) -> sampler.sample(sampler_Sm, ...)
    for (const MSLUniform& u : result.uniforms) {
        if (!u.sampler) continue;
        const std::string from = "texture(" + u.name + ",";
        const std::string to = u.name + ".sample(" + u.name + "_Sm,";
        size_t hit;
        while ((hit = body.find(from)) != std::string::npos) body.replace(hit, from.size(), to);
        const std::string fromSp = "texture(" + u.name + " ,";
        while ((hit = body.find(fromSp)) != std::string::npos) body.replace(hit, fromSp.size(), to);
        while ((hit = preambleCode.find(from)) != std::string::npos) preambleCode.replace(hit, from.size(), to);
        while ((hit = preambleCode.find(fromSp)) != std::string::npos) preambleCode.replace(hit, fromSp.size(), to);
    }
    body = RewriteTexelFetch(body);

    const bool usesFragCoord = stage == ShaderStage::Fragment &&
                               ReplaceWord(body, "gl_FragCoord", "") != body;
    if (usesFragCoord) body = ReplaceWord(body, "gl_FragCoord", "_fragCoord");

    if (stage == ShaderStage::Fragment) {
        // Голый ранний return всё равно должен вернуть выходной цвет.
        body = ReplaceWord(body, "return;", "return fragColor;");
    }

    if (stage == ShaderStage::Vertex) {
        body = ReplaceWord(body, "gl_Position", "out.position");
        for (const MSLAttribute& a : decls.attributes) body = ReplaceWord(body, a.name, "in_." + a.name);
        for (size_t i = 0; i < decls.varyingOutNames.size(); ++i) {
            body = ReplaceWord(body, decls.varyingOutNames[i], "out." + decls.varyingOutNames[i]);
        }
    } else {
        for (size_t i = 0; i < decls.varyingInNames.size(); ++i) {
            body = ReplaceWord(body, decls.varyingInNames[i], "in_." + decls.varyingInNames[i]);
        }
    }
    // Не-сэмплерные uniform-ы живут внутри упакованного аргумента-блока; снабжаем
    // их префиксом и в теле main, И в функциях-хелперах (хелперы получают блок как
    // дополнительный хвостовой параметр, см. проход хелперов ниже).
    for (const MSLUniform& u : result.uniforms) {
        if (!u.sampler) {
            body = ReplaceWord(body, u.name, "_u." + u.name);
            preambleCode = ReplaceWord(preambleCode, u.name, "_u." + u.name);
        }
    }
    // GL рендерит в NDC с y вверх; в Metal y смотрит вниз. Отразить один раз на
    // вершину, чтобы транслированная геометрия совпадала со GL-сборкой.
    if (stage == ShaderStage::Vertex) {
        // NOTE: отражения y NDC нет - NDC Metal совпадает с GL для этого конвейера.
    }

    // ---- проход хелперов ------------------------------------------------------
    const char* blockName = stage == ShaderStage::Vertex ? "UniformsVS" : "UniformsFS";
    const std::string blockParam = std::string("constant ") + blockName + "& _u";
    // Хелперы (функции преамбулы), ссылающиеся на `_u.`, получают uniform-блок
    // как хвостовой параметр; их места вызова (в main и в других хелперах)
    // добавляют `, _u`. Итерируем несколько раз, чтобы покрыть вызовы хелпер-из-хелпера.
    {
        std::vector<std::string> helperNames;
        {
            std::istringstream ls(preambleCode);
            std::string line;
            while (std::getline(ls, line)) {
                const size_t op = line.find('(');
                if (op == std::string::npos) continue;
                // Сбалансированный скан до соответствующей ')' ...
                size_t cl = op;
                int pdepth = 0;
                for (size_t i2 = op; i2 < line.size(); ++i2) {
                    if (line[i2] == '(') ++pdepth;
                    if (line[i2] == ')') { --pdepth; if (pdepth == 0) { cl = i2; break; } }
                }
                // ... и собираем только ОПРЕДЕЛЕНИЯ: за ')' должен следовать '{'.
                size_t j = cl + 1;
                while (j < line.size() && line[j] == ' ') ++j;
                if (j >= line.size() || line[j] != '{') continue;
                size_t start = op;
                while (start > 0 && (std::isalnum((unsigned char)line[start - 1]) || line[start - 1] == '_')) --start;
                if (op > start) helperNames.push_back(line.substr(start, op - start));
            }
        }
        // определения: добавляем параметр-блок
        for (const std::string& n : helperNames) {
            const std::string sigHead = n + "(";
            const size_t hit = preambleCode.find(sigHead);
            if (hit == std::string::npos) continue;
            size_t i = hit + sigHead.size();
            int depth = 1;
            while (i < preambleCode.size() && depth > 0) {
                if (preambleCode[i] == '(') ++depth;
                if (preambleCode[i] == ')') --depth;
                if (depth > 0) ++i;
            }
            preambleCode.insert(i, ", " + blockParam);
        }
        // места вызова: пропускаем определения (за которыми следует '{')
        for (const std::string& n : helperNames) {
            const std::string callHead = n + "(";
            size_t pos2 = 0;
            while (true) {
                const size_t hit = preambleCode.find(callHead, pos2);
                if (hit == std::string::npos) break;
                size_t i = hit + callHead.size();
                int depth = 1;
                while (i < preambleCode.size() && depth > 0) {
                    if (preambleCode[i] == '(') ++depth;
                    if (preambleCode[i] == ')') --depth;
                    if (depth > 0) ++i;
                }
                size_t j = i + 1;
                while (j < preambleCode.size() && (preambleCode[j] == ' ' || preambleCode[j] == '\n')) ++j;
                if (j < preambleCode.size() && preambleCode[j] == '{') {
                    pos2 = i + 1;  // определение, пропускаем
                    continue;
                }
                preambleCode.insert(i, ", _u");
                pos2 = i + 5;
            }
        }
        // та же перезапись мест вызова внутри main
        for (const std::string& n : helperNames) {
            const std::string callHead = n + "(";
            size_t pos2 = 0;
            while (true) {
                const size_t hit = body.find(callHead, pos2);
                if (hit == std::string::npos) break;
                size_t i = hit + callHead.size();
                int depth = 1;
                while (i < body.size() && depth > 0) {
                    if (body[i] == '(') ++depth;
                    if (body[i] == ')') --depth;
                    if (depth > 0) ++i;
                }
                body.insert(i, ", _u");
                pos2 = i + 5;
            }
        }
    }


    // ---- генерация ---------------------------------------------------------
    std::ostringstream msl;
    msl << "// Generated by the crossrender Metal compatibility layer (GLSL -> MSL).\n"
        << "#include <metal_stdlib>\nusing namespace metal;\n\n";

    const auto emitStruct = [&](const char* name, bool position, const std::vector<std::string>& names,
                                const std::vector<std::string>& types, bool attributes) {
        msl << "struct " << name << " {\n";
        if (position) msl << "    float4 position [[position]];\n";
        for (size_t i = 0; i < names.size(); ++i) {
            int location = -1;
            if (attributes) {
                for (const MSLAttribute& a : decls.attributes) {
                    if (a.name == names[i]) location = a.location;
                }
            }
            msl << "    " << RewriteTypes(types[i]) << " " << names[i];
            if (attributes) msl << " [[attribute(" << location << ")]]";
            msl << ";\n";
        }
        msl << "};\n\n";
    };

    const char* inStructName = stage == ShaderStage::Vertex ? "VSIn" : "FSIn";
    const char* outStructName = stage == ShaderStage::Vertex ? "VSOut" : "FSOut";

    if (stage == ShaderStage::Vertex && !decls.attributes.empty()) {
        std::vector<std::string> types, names;
        for (const MSLAttribute& a : decls.attributes) {
            names.push_back(a.name);
            types.push_back(a.glslType);
        }
        emitStruct(inStructName, false, names, types, true);
    }
    if (stage == ShaderStage::Vertex) {
        emitStruct(outStructName, true, decls.varyingOutNames, decls.varyingOutTypes, false);
    } else {
        emitStruct(inStructName, false, decls.varyingInNames, decls.varyingInTypes, false);
    }

    // Структура упакованного uniform-блока (не-сэмплерные uniform-ы, смещения в духе std140).
    // Генерируется до функций-хелперов: хелперы, ссылающиеся на uniform-ы, получают
    // блок как хвостовой параметр `constant Uniforms..& _u`.
    int hasBlock = 0;
    for (const MSLUniform& u : result.uniforms) {
        if (!u.sampler) hasBlock = 1;
    }
    if (hasBlock) {
        msl << "struct " << blockName << " {\n";
        for (const MSLUniform& u : result.uniforms) {
            if (u.sampler) continue;
            msl << "    " << RewriteTypes(u.glslType) << " " << u.name;
            if (u.arraySize > 0) msl << "[" << u.arraySize << "]";
            msl << ";  // offset " << u.offset << "\n";
        }
        msl << "};\n\n";
    }

    if (!preambleCode.empty()) {
        msl << preambleCode << "\n";
    }

    // Сигнатура.
    std::ostringstream args;
    if ((stage == ShaderStage::Vertex && !decls.attributes.empty()) ||
        (stage == ShaderStage::Fragment && !decls.varyingInNames.empty())) {
        args << inStructName << " in_ [[stage_in]]";
    }
    if (usesFragCoord) {
        if (args.str().size()) args << ", ";
        args << "float4 _fragCoord [[position]]";
    }
    if (hasBlock) {
        if (args.str().size()) args << ", ";
        args << blockParam << " [[buffer(16)]]";
    }

    for (const MSLUniform& u : result.uniforms) {
        if (!u.sampler) continue;
        if (args.str().size()) args << ", ";
        const char* texType = u.glslType == "samplerCube" ? "texturecube<float>" : "texture2d<float>";
        args << texType << " " << u.name << " [[texture(" << u.binding << ")]]";
        args << ", sampler " << u.name << "_Sm [[sampler(" << u.binding << ")]]";
    }

    if (stage == ShaderStage::Vertex) {
        msl << "vertex " << outStructName << " main0(" << args.str() << ") {\n"
            << "    " << outStructName << " out;\n"
            << body << "\n"
            << "    return out;\n}\n";
    } else {
        msl << "fragment float4 main0(" << args.str() << ") {\n";
        if (decls.hasFragmentOutput) {
            msl << "    " << RewriteTypes(decls.fragmentOutputType) << " "
                << decls.fragmentOutputName << " = float4(0.0);\n";
        }
        msl << body << "\n";
        if (decls.hasFragmentOutput) {
            msl << "    return " << decls.fragmentOutputName << ";\n";
        } else {
            msl << "    return float4(0.0);\n";
        }
        msl << "}\n";
    }

    result.attributes = std::move(decls.attributes);
    result.msl = msl.str();
    result.ok = true;
    return result;
}

}  // namespace crossrender::mtlgl
