// SpriteAtlas — загрузка дескрипторов (engine JSON, TexturePacker, Aseprite,
// Sparrow XML, libGDX), shelf-упаковщик и помощники отрисовки во время выполнения.
//
// Публичный контракт живёт в crossrender/gfx/SpriteAtlas.h; ничто здесь не должно
// его менять. Всё ориентировано на CPU: разбор дескриптора никогда не трогает GPU,
// а отсутствие изображения страницы — предупреждение, а не ошибка.
#include "crossrender/gfx/SpriteAtlas.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Json.h"

#include "SpriteAtlasInternal.h"

#include <map>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Небольшие строковые / текстовые помощники
// ---------------------------------------------------------------------------
constexpr usize kNpos = static_cast<usize>(-1);

void AddWarning(std::vector<std::string>& warns, const std::string& msg) {
    warns.push_back(msg);
    ENG_LOGW("atlas", "%s", msg.c_str());
}

std::string FormatMsg(const char* fmt, ...)
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string FormatMsg(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    usize b = 0;
    usize e = s.size();
    // Срезаем UTF-8 BOM, чтобы детектор JSON видел настоящий первый символ.
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        b = 3;
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool ContainsI(const std::string& haystack, const char* needle) {
    return ToLower(haystack).find(ToLower(std::string(needle))) != std::string::npos;
}

bool HasImageExtension(const std::string& path) {
    const std::string ext = ToLower(PathExt(path));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
           ext == ".gif" || ext == ".webp" || ext == ".hdr" || ext == ".psd" || ext == ".qoi";
}

// Имя файла без каталога и без распознанного расширения изображения.
std::string StemOf(const std::string& path) {
    std::string base = PathBase(path);
    const usize dot = base.find_last_of('.');
    if (dot != kNpos && dot > 0 && HasImageExtension(base)) base = base.substr(0, dot);
    return base;
}

std::string FirstLine(const std::string& text) {
    const usize nl = text.find('\n');
    std::string line = nl == kNpos ? text : text.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return Trim(line);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    usize start = 0;
    for (;;) {
        const usize nl = text.find('\n', start);
        std::string line = text.substr(start, nl == kNpos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == kNpos) break;
        start = nl + 1;
    }
    return lines;
}

// Разбирает "1, 2, 3" (или разделённые пробелами) в числа float.
std::vector<f32> ParseNumberList(const std::string& s) {
    std::vector<f32> out;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        char* end = nullptr;
        const f32 v = std::strtof(s.c_str() + i, &end);
        if (end == s.c_str() + i) break;
        out.push_back(v);
        i = static_cast<usize>(end - s.c_str());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Модель разобранного дескриптора (не зависит от формата)
// ---------------------------------------------------------------------------
struct ParsedRegion {
    std::string name;
    Rect frame;
    Vec2 pivot{0.5f, 0.5f};
    bool rotated = false;
    bool trimmed = false;
    Vec2 sourceSize{0, 0};
    Rect spriteSourceSize;
    int page = 0;
    f32 durationMs = 0.0f;
    std::vector<Vec2> polygon;

    [[nodiscard]] Vec2 DisplaySize() const {
        return rotated ? Vec2{frame.h, frame.w} : Vec2{frame.w, frame.h};
    }
};

struct ParsedPage {
    std::string image;
    int width = 0;
    int height = 0;
};

struct ParsedAnim {
    std::string name;
    std::vector<int> frameIndices;        // прямые индексы (Aseprite frameTags)
    std::vector<std::string> frameNames;  // разрешаются по имени (engine JSON)
    std::vector<f32> durations;
    bool loop = true;
};

struct ParsedAtlas {
    std::vector<ParsedRegion> regions;
    std::vector<ParsedPage> pages;
    std::vector<ParsedAnim> anims;
    bool ok = false;
};

// Полигоны TexturePacker / Aseprite заданы в исходных пикселях, а движок
// хранит их нормализованными. Значения больше 1 считаются пикселями.
void NormalizePolygon(std::vector<Vec2>* poly, const Vec2& size) {
    if (!poly || poly->empty()) return;
    f32 maxV = 0.0f;
    for (const Vec2& p : *poly) maxV = MaxT(maxV, MaxT(std::fabs(p.x), std::fabs(p.y)));
    if (maxV <= 1.0f + 1e-4f) return;
    const f32 w = size.x > 0.0f ? size.x : 1.0f;
    const f32 h = size.y > 0.0f ? size.y : 1.0f;
    for (Vec2& p : *poly) {
        p.x /= w;
        p.y /= h;
    }
}

// ---------------------------------------------------------------------------
// JSON-записи кадров
// ---------------------------------------------------------------------------
void ReadFrameFields(const JsonValue& fv, ParsedRegion* r) {
    const JsonValue* frame = fv.Find("frame");
    if (frame && frame->IsObject()) {
        r->frame = Rect{frame->GetFloat("x"), frame->GetFloat("y"), frame->GetFloat("w"),
                        frame->GetFloat("h")};
    } else if (frame && frame->IsString()) {
        const std::vector<f32> v = ParseNumberList(frame->AsString());
        if (v.size() >= 4) r->frame = Rect{v[0], v[1], v[2], v[3]};
    } else {
        // Плоские ключи x/y/w/h (некоторые экспортёры используют это для array-раскладки).
        r->frame = Rect{fv.GetFloat("x"), fv.GetFloat("y"), fv.GetFloat("w"), fv.GetFloat("h")};
    }

    const JsonValue* srcSize = fv.Find("sourceSize");
    if (srcSize && srcSize->IsObject()) {
        r->sourceSize = {srcSize->GetFloat("w"), srcSize->GetFloat("h")};
    } else if (srcSize && srcSize->IsString()) {
        const std::vector<f32> v = ParseNumberList(srcSize->AsString());
        if (v.size() >= 2) r->sourceSize = {v[0], v[1]};
    }

    const JsonValue* sss = fv.Find("spriteSourceSize");
    if (sss && sss->IsObject()) {
        r->spriteSourceSize =
            Rect{sss->GetFloat("x"), sss->GetFloat("y"), sss->GetFloat("w"), sss->GetFloat("h")};
    } else if (sss && sss->IsString()) {
        const std::vector<f32> v = ParseNumberList(sss->AsString());
        if (v.size() >= 4) r->spriteSourceSize = Rect{v[0], v[1], v[2], v[3]};
    }

    r->rotated = fv.GetBool("rotated", false);
    if (r->sourceSize.x <= 0.0f || r->sourceSize.y <= 0.0f) r->sourceSize = r->DisplaySize();
    if (r->spriteSourceSize.w <= 0.0f || r->spriteSourceSize.h <= 0.0f)
        r->spriteSourceSize = Rect{0, 0, r->DisplaySize().x, r->DisplaySize().y};

    const JsonValue* trimmed = fv.Find("trimmed");
    if (trimmed && trimmed->IsBool()) {
        r->trimmed = trimmed->AsBool();
    } else {
        r->trimmed = r->spriteSourceSize.x != 0.0f || r->spriteSourceSize.y != 0.0f ||
                     r->spriteSourceSize.w != r->frame.w || r->spriteSourceSize.h != r->frame.h;
    }

    const JsonValue* pivot = fv.Find("pivot");
    if (pivot && pivot->IsObject())
        r->pivot = {pivot->GetFloat("x", 0.5f), pivot->GetFloat("y", 0.5f)};

    r->durationMs = fv.GetFloat("duration", 0.0f);
    r->page = fv.GetInt("page", 0);

    const JsonValue* poly = fv.Find("polygon");
    if (poly && poly->IsArray()) {
        for (usize i = 0; i < poly->Size(); ++i) {
            const JsonValue& p = (*poly)[i];
            if (p.IsObject()) {
                r->polygon.push_back({p.GetFloat("x"), p.GetFloat("y")});
            } else if (p.IsArray() && p.Size() >= 2) {
                r->polygon.push_back({p[0].AsFloat(), p[1].AsFloat()});
            }
        }
        NormalizePolygon(&r->polygon, r->DisplaySize());
    }
}

// ---------------------------------------------------------------------------
// Sparrow / Starling XML (самописный сканер, без XML-библиотеки)
// ---------------------------------------------------------------------------
std::string XmlAttr(const std::string& tag, const char* name) {
    const usize n = std::strlen(name);
    usize i = 0;
    while (i < tag.size()) {
        const usize p = tag.find(name, i);
        if (p == kNpos) return {};
        const bool leftOk =
            p == 0 || !(std::isalnum(static_cast<unsigned char>(tag[p - 1])) || tag[p - 1] == '_' ||
                        tag[p - 1] == '-' || tag[p - 1] == ':');
        const usize after = p + n;
        const bool rightOk =
            after >= tag.size() ||
            !(std::isalnum(static_cast<unsigned char>(tag[after])) || tag[after] == '_' ||
              tag[after] == '-' || tag[after] == ':');
        if (!leftOk || !rightOk) {
            i = p + 1;
            continue;
        }
        usize eq = after;
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) ++eq;
        if (eq >= tag.size() || tag[eq] != '=') {
            i = p + 1;
            continue;
        }
        ++eq;
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) ++eq;
        if (eq >= tag.size()) return {};
        const char quote = tag[eq];
        if (quote == '"' || quote == '\'') {
            const usize close = tag.find(quote, eq + 1);
            if (close == kNpos) return {};
            return tag.substr(eq + 1, close - eq - 1);
        }
        usize close = eq;
        while (close < tag.size() && !std::isspace(static_cast<unsigned char>(tag[close])) &&
               tag[close] != '>' && tag[close] != '/')
            ++close;
        return tag.substr(eq, close - eq);
    }
    return {};
}

f32 XmlAttrF(const std::string& tag, const char* name, f32 def = 0.0f) {
    const std::string v = XmlAttr(tag, name);
    if (v.empty()) return def;
    char* end = nullptr;
    const f32 f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? def : f;
}

bool ParseSparrowXml(const std::string& text, ParsedAtlas& out, std::vector<std::string>& warns) {
    const std::string lower = ToLower(text);
    // <TextureAtlas imagePath="atlas.png" width="512" height="512">
    const usize atlas = lower.find("<textureatlas");
    if (atlas != kNpos) {
        const usize end = text.find('>', atlas);
        if (end != kNpos) {
            const std::string tag = text.substr(atlas, end - atlas);
            ParsedPage page;
            page.image = XmlAttr(tag, "imagePath");
            if (page.image.empty()) page.image = XmlAttr(tag, "image");
            page.width = static_cast<int>(XmlAttrF(tag, "width"));
            page.height = static_cast<int>(XmlAttrF(tag, "height"));
            if (!page.image.empty() || page.width > 0 || page.height > 0) out.pages.push_back(page);
        }
    }

    usize pos = 0;
    for (;;) {
        const usize tagStart = lower.find("<subtexture", pos);
        if (tagStart == kNpos) break;
        const usize tagEnd = text.find('>', tagStart);
        if (tagEnd == kNpos) break;
        const std::string tag = text.substr(tagStart, tagEnd - tagStart);
        pos = tagEnd + 1;

        ParsedRegion r;
        r.name = XmlAttr(tag, "name");
        if (r.name.empty()) continue;
        r.frame = Rect{XmlAttrF(tag, "x"), XmlAttrF(tag, "y"), XmlAttrF(tag, "width"),
                       XmlAttrF(tag, "height")};
        r.rotated = ToLower(XmlAttr(tag, "rotated")) == "true";
        // Sparrow записывает смещения обрезанного прямоугольника как *отрицательные*
        // frameX/frameY, а необрезанный размер — как frameWidth/frameHeight.
        const bool hasFrame =
            !XmlAttr(tag, "frameWidth").empty() || !XmlAttr(tag, "frameHeight").empty();
        const f32 fx = XmlAttrF(tag, "frameX", 0.0f);
        const f32 fy = XmlAttrF(tag, "frameY", 0.0f);
        f32 fw = XmlAttrF(tag, "frameWidth", r.frame.w);
        f32 fh = XmlAttrF(tag, "frameHeight", r.frame.h);
        if (fw <= 0.0f) fw = r.frame.w;
        if (fh <= 0.0f) fh = r.frame.h;
        r.sourceSize = {fw, fh};
        // `width`/`height` — это вырез страницы, поэтому у повёрнутого SubTexture
        // отображаемый (неповернутый) обрезанный размер хранится транспонированным.
        // frameX/frameY остаются в display-пространстве, как в других форматах.
        const f32 dw = r.rotated ? r.frame.h : r.frame.w;
        const f32 dh = r.rotated ? r.frame.w : r.frame.h;
        r.spriteSourceSize = Rect{-fx, -fy, dw, dh};
        r.trimmed = hasFrame && (fx != 0.0f || fy != 0.0f || fw != dw || fh != dh);
        r.pivot = {XmlAttrF(tag, "pivotX", 0.5f), XmlAttrF(tag, "pivotY", 0.5f)};
        r.durationMs = XmlAttrF(tag, "duration", 0.0f);
        out.regions.push_back(std::move(r));
    }
    if (out.regions.empty()) AddWarning(warns, "atlas: Sparrow XML contained no <SubTexture> entries");
    return !out.regions.empty();
}

// ---------------------------------------------------------------------------
// Текстовый формат libGDX .atlas
// ---------------------------------------------------------------------------
bool ParseLibGdx(const std::string& text, ParsedAtlas& out, std::vector<std::string>& warns) {
    const std::vector<std::string> lines = SplitLines(text);
    int page = -1;
    bool inRegion = false;
    bool sawHeader = false;
    ParsedRegion cur;
    int curIndex = -1;
    int curOffsetX = 0;
    int curOffsetY = 0;
    bool curHasOrig = false;
    bool curHasOffset = false;
    bool offsetFromBottom = false;
    // Собственный упаковщик libGDX пишет `size`/`bounds` как *неповернутый* размер
    // региона и транспонирует вырез страницы для повёрнутых регионов
    // (TexturePacker.writeRect пишет "size" = regionWidth/regionHeight), тогда как
    // самописные экспортёры пишут сам вырез. Принимаются оба; см. `sizeIsCut` в flush().
    f32 xyX = 0.0f, xyY = 0.0f, sizeW = 0.0f, sizeH = 0.0f;
    bool hasXY = false, hasSize = false;

    auto flush = [&]() {
        if (!inRegion) return;
        ParsedRegion r = cur;
        if (curIndex >= 0) r.name = cur.name + "_" + std::to_string(curIndex);
        const f32 dw = hasSize ? sizeW : r.frame.w;
        const f32 dh = hasSize ? sizeH : r.frame.h;
        const f32 px = hasXY ? xyX : r.frame.x;
        const f32 py = hasXY ? xyY : r.frame.y;
        // Обрезанный регион не может быть больше исходного ни по одной оси, поэтому
        // `size`, превышающий `orig`, обязан быть повёрнутым вырезом страницы.
        const bool sizeIsCut = r.rotated && hasSize && curHasOrig &&
                               (dw > r.sourceSize.x + 0.5f || dh > r.sourceSize.y + 0.5f);
        const f32 displayW = (r.rotated && sizeIsCut) ? dh : dw;
        const f32 displayH = (r.rotated && sizeIsCut) ? dw : dh;
        // `frame` — всегда прямоугольник, фактически хранящийся на странице:
        // транспонированный, когда упаковщик повернул спрайт, а `size` был неповернутым размером.
        r.frame = (r.rotated && !sizeIsCut) ? Rect{px, py, dh, dw} : Rect{px, py, dw, dh};
        if (r.sourceSize.x <= 0.0f || r.sourceSize.y <= 0.0f) r.sourceSize = {displayW, displayH};
        f32 offsetY = static_cast<f32>(curOffsetY);
        if (offsetFromBottom) offsetY = r.sourceSize.y - displayH - offsetY;
        r.spriteSourceSize = {static_cast<f32>(curOffsetX), offsetY, displayW, displayH};
        r.trimmed = (curHasOffset && (curOffsetX != 0 || offsetY != 0.0f)) ||
                    (curHasOrig && (r.sourceSize.x != displayW || r.sourceSize.y != displayH));
        r.page = page < 0 ? 0 : page;
        out.regions.push_back(std::move(r));
        inRegion = false;
        cur = ParsedRegion{};
        curIndex = -1;
        curOffsetX = curOffsetY = 0;
        curHasOrig = curHasOffset = false;
        offsetFromBottom = false;
        xyX = xyY = sizeW = sizeH = 0.0f;
        hasXY = hasSize = false;
    };

    for (const std::string& raw : lines) {
        if (Trim(raw).empty()) continue;
        const bool indented = raw[0] == ' ' || raw[0] == '\t';
        const std::string line = Trim(raw);
        if (!indented && line.find(':') == std::string::npos) {
            const bool header = !sawHeader || HasImageExtension(line);
            if (header) {
                flush();
                ParsedPage p;
                p.image = line;
                out.pages.push_back(p);
                page = static_cast<int>(out.pages.size()) - 1;
                sawHeader = true;
                inRegion = false;
            } else {
                flush();
                cur = ParsedRegion{};
                cur.name = line;
                inRegion = true;
            }
            continue;
        }
        const usize colon = line.find(':');
        if (colon == kNpos) continue;
        const std::string key = ToLower(Trim(line.substr(0, colon)));
        const std::string val = Trim(line.substr(colon + 1));
        if (!inRegion) {
            if (key == "size" && page >= 0) {
                const std::vector<f32> v = ParseNumberList(val);
                if (v.size() >= 2) {
                    out.pages[page].width = static_cast<int>(v[0]);
                    out.pages[page].height = static_cast<int>(v[1]);
                }
            }
            continue;  // format / filter / repeat / pma и неизвестные ключи страницы
        }
        if (key == "rotate") {
            cur.rotated = ToLower(val) == "true";
        } else if (key == "xy") {
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 2) {
                xyX = v[0];
                xyY = v[1];
                hasXY = true;
            }
        } else if (key == "size") {
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 2) {
                sizeW = v[0];
                sizeH = v[1];
                hasSize = true;
            }
        } else if (key == "bounds") {
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 4) {
                xyX = v[0];
                xyY = v[1];
                sizeW = v[2];
                sizeH = v[3];
                hasXY = hasSize = true;
            }
        } else if (key == "orig") {
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 2) {
                cur.sourceSize = {v[0], v[1]};
                curHasOrig = true;
            }
        } else if (key == "offset") {
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 2) {
                curOffsetX = static_cast<int>(v[0]);
                curOffsetY = static_cast<int>(v[1]);
                curHasOffset = true;
            }
        } else if (key == "offsets") {
            // Новая раскладка libGDX: offsetX, offsetY (отсчитываются от низа),
            // originalWidth, originalHeight.
            const std::vector<f32> v = ParseNumberList(val);
            if (v.size() >= 4) {
                curOffsetX = static_cast<int>(v[0]);
                curOffsetY = static_cast<int>(v[1]);
                cur.sourceSize = {v[2], v[3]};
                curHasOffset = true;
                curHasOrig = true;
                offsetFromBottom = true;
            }
        } else if (key == "index") {
            curIndex = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        }
        // Неизвестные ключи намеренно игнорируются.
    }
    flush();
    if (out.regions.empty()) AddWarning(warns, "atlas: libGDX descriptor contained no regions");
    return !out.regions.empty();
}

// ---------------------------------------------------------------------------
// Обходчик JSON-дескриптора (engine + TexturePacker + Aseprite имеют общую раскладку)
// ---------------------------------------------------------------------------
ParsedAtlas ParseDescriptor(const std::string& text, AtlasFormat fmt,
                            std::vector<std::string>& warns) {
    ParsedAtlas out;
    if (fmt == AtlasFormat::SparrowXml) {
        out.ok = ParseSparrowXml(text, out, warns);
        return out;
    }
    if (fmt == AtlasFormat::LibGdx) {
        out.ok = ParseLibGdx(text, out, warns);
        return out;
    }

    std::string err;
    JsonValue root = JsonValue::Parse(text, &err);
    if (!root.IsObject()) {
        AddWarning(warns,
                   "atlas: JSON parse failed: " + (err.empty() ? std::string("bad document") : err));
        return out;
    }
    const JsonValue* meta = root.Find("meta");

    // Страницы: либо явный массив "pages" (многостраничный engine), либо meta.
    if (const JsonValue* pages = root.Find("pages"); pages && pages->IsArray()) {
        for (usize i = 0; i < pages->Size(); ++i) {
            const JsonValue& pv = (*pages)[i];
            ParsedPage p;
            p.image = pv.GetString("image", pv.GetString("file", ""));
            p.width = pv.GetInt("w", pv.GetInt("width", 0));
            p.height = pv.GetInt("h", pv.GetInt("height", 0));
            out.pages.push_back(std::move(p));
        }
    }
    if (out.pages.empty() && meta && meta->IsObject()) {
        ParsedPage p;
        p.image = meta->GetString("image");
        if (const JsonValue* sz = meta->Find("size"); sz && sz->IsObject()) {
            p.width = sz->GetInt("w");
            p.height = sz->GetInt("h");
        }
        if (!p.image.empty() || p.width > 0 || p.height > 0) out.pages.push_back(std::move(p));
    }

    // Кадры: объект с ключами-именами (hash) или массив записей (array-раскладки).
    const JsonValue* frames = root.Find("frames");
    if (frames && frames->IsObject()) {
        for (const auto& kv : frames->Object()) {
            ParsedRegion r;
            r.name = kv.first;
            ReadFrameFields(kv.second, &r);
            out.regions.push_back(std::move(r));
        }
    } else if (frames && frames->IsArray()) {
        for (usize i = 0; i < frames->Size(); ++i) {
            const JsonValue& fv = (*frames)[i];
            ParsedRegion r;
            const std::string file = fv.GetString("filename", fv.GetString("name", ""));
            r.name = file.empty() ? ("frame_" + std::to_string(i)) : StemOf(file);
            ReadFrameFields(fv, &r);
            out.regions.push_back(std::move(r));
        }
    } else {
        AddWarning(warns, "atlas: descriptor has no 'frames' member");
    }

    // Анимации engine JSON.
    const JsonValue* anims = root.Find("animations");
    if (anims && anims->IsArray()) {
        for (usize i = 0; i < anims->Size(); ++i) {
            const JsonValue& av = (*anims)[i];
            ParsedAnim a;
            a.name = av.GetString("name");
            if (a.name.empty()) continue;
            if (const JsonValue* fr = av.Find("frames"); fr && fr->IsArray()) {
                for (usize k = 0; k < fr->Size(); ++k) a.frameNames.push_back((*fr)[k].AsString());
            }
            if (const JsonValue* du = av.Find("durations"); du && du->IsArray()) {
                for (usize k = 0; k < du->Size(); ++k)
                    a.durations.push_back((*du)[k].AsFloat(100.0f));
            }
            a.loop = av.GetBool("loop", true);
            if (!a.frameNames.empty()) out.anims.push_back(std::move(a));
        }
    }

    // Теги кадров Aseprite становятся анимациями; `repeat` масштабирует длительности.
    if (meta && meta->IsObject()) {
        if (const JsonValue* tags = meta->Find("frameTags"); tags && tags->IsArray()) {
            for (usize i = 0; i < tags->Size(); ++i) {
                const JsonValue& tv = (*tags)[i];
                ParsedAnim a;
                a.name = tv.GetString("name");
                if (a.name.empty()) continue;
                int from = tv.GetInt("from", 0);
                int to = tv.GetInt("to", -1);
                if (to < from) std::swap(from, to);
                for (int f = from; f <= to; ++f) {
                    if (f < 0 || f >= static_cast<int>(out.regions.size())) continue;
                    a.frameIndices.push_back(f);
                }
                if (ToLower(tv.GetString("direction", "forward")) == "reverse")
                    std::reverse(a.frameIndices.begin(), a.frameIndices.end());
                f32 repeat = tv.GetFloat("repeat", 1.0f);
                if (repeat < 1.0f) repeat = 1.0f;
                for (int f : a.frameIndices) {
                    const f32 d = out.regions[static_cast<usize>(f)].durationMs;
                    a.durations.push_back((d > 0.0f ? d : 100.0f) * repeat);
                }
                a.loop = true;
                if (!a.frameIndices.empty()) out.anims.push_back(std::move(a));
            }
        }
    }

    out.ok = !out.regions.empty();
    if (!out.ok) AddWarning(warns, "atlas: descriptor contained no frames");
    return out;
}

// ---------------------------------------------------------------------------
// Помощники упаковки
// ---------------------------------------------------------------------------
struct PackItem {
    std::string path;
    std::string name;
    int w = 0;  // упакованный размер (после обрезки)
    int h = 0;
    int originalW = 0;
    int originalH = 0;
    int trimX = 0;
    int trimY = 0;
    bool trimmed = false;
    std::vector<u8> rgba;  // RGBA8 сверху вниз, уже обрезано
};

struct Placement {
    int item = 0;
    int page = 0;
    int x = 0;
    int y = 0;
    bool rotated = false;
};

// Огибающий прямоугольник непрозрачных пикселей; всё изображение, если оно полностью прозрачно.
bool ComputeAlphaBounds(const Texture::ImageData& img, int* x0, int* y0, int* x1, int* y1) {
    if (img.width <= 0 || img.height <= 0) return false;
    int minX = img.width, minY = img.height, maxX = -1, maxY = -1;
    const usize stride = static_cast<usize>(img.width) * 4;
    for (int y = 0; y < img.height; ++y) {
        const u8* row = img.pixels.data() + static_cast<usize>(y) * stride;
        for (int x = 0; x < img.width; ++x) {
            if (row[static_cast<usize>(x) * 4 + 3] == 0) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < minX || maxY < minY) {
        *x0 = 0;
        *y0 = 0;
        *x1 = img.width;
        *y1 = img.height;
        return false;
    }
    *x0 = minX;
    *y0 = minY;
    *x1 = maxX + 1;
    *y1 = maxY + 1;
    return true;
}

u8 ColorByte(f32 v) { return static_cast<u8>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// ---------------------------------------------------------------------------
// Помощники отрисовки
// ---------------------------------------------------------------------------
// UV-прямоугольник (нормализованный, в пространстве страницы), покрывающий `logical` внутри `r`.
Rect UvForLogical(const AtlasRegion& r, const Rect& logical) {
    const Vec2 size = r.Size();
    if (size.x <= 0.0f || size.y <= 0.0f) return r.uv;
    const Rect stored = atlas_detail::StoredRectForLogical(logical, size, r.rotated);
    const f32 sw = r.rotated ? size.y : size.x;  // ширина хранимого (страничного) прямоугольника в пикселях
    const f32 sh = r.rotated ? size.x : size.y;  // высота хранимого (страничного) прямоугольника в пикселях
    return Rect{r.uv.x + (stored.x / sw) * r.uv.w, r.uv.y + (stored.y / sh) * r.uv.h,
                (stored.w / sw) * r.uv.w, (stored.h / sh) * r.uv.h};
}

// Рисует под-прямоугольник `logical` региона (0,0 .. Size()) в `dst`.
// Зеркалирование выражается обращением сэмплируемого UV-прямоугольника, а повёрнутый
// регион выпрямляется четвертью оборота трансформации рендерера (так что каждый пиксель
// по-прежнему проходит через Renderer2D::Image).
void DrawRegionQuad(Renderer2D& r2d, const Texture& tex, const AtlasRegion& region,
                    const Rect& logical, const Rect& dst, const Color& tint, bool flipX, bool flipY) {
    if (dst.w <= 0.0f || dst.h <= 0.0f) return;
    Rect src = UvForLogical(region, logical);
    // Логическое зеркало при повороте отображается на перпендикулярную хранимую ось.
    const bool flipU = region.rotated ? flipY : flipX;
    const bool flipV = region.rotated ? flipX : flipY;
    if (flipU) {
        src.x += src.w;
        src.w = -src.w;
    }
    if (flipV) {
        src.y += src.h;
        src.h = -src.h;
    }
    if (!region.rotated) {
        r2d.Image(tex, dst, src, tint);
        return;
    }
    r2d.Save();
    r2d.Translate(dst.x + dst.w * 0.5f, dst.y + dst.h * 0.5f);
    r2d.Rotate(atlas_detail::UprightRotation());
    r2d.Image(tex, Rect{-dst.h * 0.5f, -dst.w * 0.5f, dst.h, dst.w}, src, tint);
    r2d.Restore();
}

void RebuildLookup(std::vector<AtlasRegion>& regions, std::unordered_map<std::string, int>& lookup) {
    lookup.clear();
    for (usize i = 0; i < regions.size(); ++i) lookup[regions[i].name] = static_cast<int>(i);
}

// Длительность кадра в миллисекундах (единица, которую использует AtlasAnimation).
f32 FrameDurationMs(const AtlasAnimation& a, usize i) {
    const f32 ms = i < a.durations.size() ? a.durations[i] : 100.0f;
    return MaxT(ms, 0.0f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Хранение страниц на CPU
// ---------------------------------------------------------------------------
struct SpriteAtlas::Impl {
    struct CpuPage {
        int width = 0;
        int height = 0;
        std::vector<u8> rgba;  // RGBA8 сверху вниз
    };
    std::vector<CpuPage> cpu;
};

// ---------------------------------------------------------------------------
// Время жизни
// ---------------------------------------------------------------------------
SpriteAtlas::SpriteAtlas() : impl_(new Impl()) {}

SpriteAtlas::~SpriteAtlas() = default;

SpriteAtlas::SpriteAtlas(SpriteAtlas&& o) noexcept
    : impl_(std::move(o.impl_)),
      regions_(std::move(o.regions_)),
      pages_(std::move(o.pages_)),
      lookup_(std::move(o.lookup_)),
      animations_(std::move(o.animations_)),
      warnings_(std::move(o.warnings_)),
      source_(std::move(o.source_)),
      format_(o.format_),
      stats_(o.stats_) {
    if (!impl_) impl_.reset(new Impl());
    o.format_ = AtlasFormat::Unknown;
    o.stats_ = Stats{};
}

SpriteAtlas& SpriteAtlas::operator=(SpriteAtlas&& o) noexcept {
    if (this == &o) return *this;
    impl_ = std::move(o.impl_);
    regions_ = std::move(o.regions_);
    pages_ = std::move(o.pages_);
    lookup_ = std::move(o.lookup_);
    animations_ = std::move(o.animations_);
    warnings_ = std::move(o.warnings_);
    source_ = std::move(o.source_);
    format_ = o.format_;
    stats_ = o.stats_;
    if (!impl_) impl_.reset(new Impl());
    o.format_ = AtlasFormat::Unknown;
    o.stats_ = Stats{};
    return *this;
}

void SpriteAtlas::Clear() {
    regions_.clear();
    pages_.clear();
    lookup_.clear();
    animations_.clear();
    warnings_.clear();
    source_.clear();
    format_ = AtlasFormat::Unknown;
    stats_ = Stats{};
    if (impl_) impl_->cpu.clear();
}

// ---------------------------------------------------------------------------
// Определение формата
// ---------------------------------------------------------------------------
namespace {
bool LooksLikeLibGdx(const std::string& text) {
    if (text.find("xy:") == std::string::npos || text.find("size:") == std::string::npos)
        return false;
    if (text.find("rotate:") == std::string::npos && text.find('\n') == std::string::npos)
        return false;
    const std::string first = FirstLine(text);
    if (first.empty()) return false;
    if (first[0] == '<' || first[0] == '{' || first[0] == '[') return false;
    if (first.find(':') != std::string::npos) return false;
    return true;
}
}  // namespace

AtlasFormat SpriteAtlas::DetectFormat(const std::string& descriptor) {
    const std::string t = Trim(descriptor);
    if (t.empty()) return AtlasFormat::Unknown;

    if (t[0] == '<') {
        return ContainsI(t, "<textureatlas") ? AtlasFormat::SparrowXml : AtlasFormat::Unknown;
    }

    if (t[0] == '{' || t[0] == '[') {
        std::string err;
        JsonValue root = JsonValue::Parse(t, &err);
        if (!root.IsObject()) return AtlasFormat::Unknown;
        const JsonValue* meta = root.Find("meta");
        const std::string app = ToLower(meta ? meta->GetString("app") : std::string());
        const JsonValue* frames = root.Find("frames");
        const bool framesArray = frames && frames->IsArray();
        const bool framesObject = frames && frames->IsObject();

        if (app.find("aseprite") != std::string::npos) return AtlasFormat::Aseprite;
        if (meta && meta->Find("frameTags")) return AtlasFormat::Aseprite;
        if (app.find("texturepacker") != std::string::npos)
            return framesArray ? AtlasFormat::TexturePackerArray : AtlasFormat::TexturePackerHash;
        if (app == "gameengine") return AtlasFormat::EngineJson;
        // Раскладка engine — это TexturePacker hash плюс "pages"/"animations".
        if (root.Find("animations") || root.Find("pages")) return AtlasFormat::EngineJson;
        if (framesArray) {
            // Массивы Aseprite содержат длительность на кадр; у TexturePacker её нет.
            if (frames->Size() > 0 && (*frames)[0].Find("duration")) return AtlasFormat::Aseprite;
            return AtlasFormat::TexturePackerArray;
        }
        if (framesObject) return AtlasFormat::TexturePackerHash;
        return AtlasFormat::Unknown;
    }

    if (LooksLikeLibGdx(t)) return AtlasFormat::LibGdx;
    // Текстовый файл с одним путём к изображению — «голый» дескриптор изображения.
    if (t.find('\n') == std::string::npos && HasImageExtension(t)) return AtlasFormat::PlainImage;
    return AtlasFormat::Unknown;
}

const char* SpriteAtlas::FormatName(AtlasFormat f) {
    switch (f) {
        case AtlasFormat::Unknown: return "Unknown";
        case AtlasFormat::EngineJson: return "EngineJson";
        case AtlasFormat::TexturePackerHash: return "TexturePackerHash";
        case AtlasFormat::TexturePackerArray: return "TexturePackerArray";
        case AtlasFormat::Aseprite: return "Aseprite";
        case AtlasFormat::SparrowXml: return "SparrowXml";
        case AtlasFormat::LibGdx: return "LibGdx";
        case AtlasFormat::PlainImage: return "PlainImage";
        case AtlasFormat::Count: break;
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Загрузка
// ---------------------------------------------------------------------------
bool SpriteAtlas::LoadPagesAndRegions(const std::string& descriptorText, const std::string& baseDir,
                                      bool srgb) {
    ParsedAtlas parsed = ParseDescriptor(descriptorText, format_, warnings_);
    if (parsed.regions.empty()) {
        if (parsed.ok) AddWarning(warnings_, "atlas: descriptor yielded no regions");
        return false;
    }

    // У каждого региона должен быть слот страницы, на которой он живёт.
    int maxPage = 0;
    for (const ParsedRegion& r : parsed.regions) maxPage = MaxT(maxPage, r.page);
    while (static_cast<int>(parsed.pages.size()) <= maxPage) parsed.pages.push_back(ParsedPage{});

    for (usize i = 0; i < parsed.pages.size(); ++i) {
        AtlasPageDesc page;
        page.image = parsed.pages[i].image;
        page.width = parsed.pages[i].width;
        page.height = parsed.pages[i].height;
        if (!page.image.empty()) {
            std::string resolved = page.image;
            if (!FileExists(resolved) && !baseDir.empty()) resolved = PathJoin(baseDir, page.image);
            page.resolved = resolved;
            if (!FileExists(resolved)) {
                AddWarning(warnings_, "atlas: page image missing: " + resolved +
                                          " (regions kept; drawing disabled)");
            } else if (!page.texture.LoadFromFile(resolved, srgb, false)) {
                // Файл есть: либо это не изображение, либо пиксели декодировались,
                // но не смогли загрузиться (нет текущего GL-контекста).
                Texture::ImageData probe;
                if (Texture::DecodeImageFile(resolved, &probe, false)) {
                    AddWarning(warnings_, FormatMsg("atlas: page %d image decoded but not uploaded "
                                                    "(no GL context); drawing disabled",
                                                    static_cast<int>(i)));
                } else {
                    AddWarning(warnings_, "atlas: page image unreadable: " + resolved +
                                              " (regions kept; drawing disabled)");
                }
            }
        } else {
            AddWarning(warnings_, FormatMsg("atlas: page %d names no image", static_cast<int>(i)));
        }
        if (page.width <= 0 && page.texture.Valid()) page.width = page.texture.Width();
        if (page.height <= 0 && page.texture.Valid()) page.height = page.texture.Height();
        // `uv` не должен зависеть от того, была ли загрузка на GPU: когда дескриптор
        // не указывает размер страницы (например, Sparrow XML без width/height),
        // читаем его из самого изображения.
        if ((page.width <= 0 || page.height <= 0) && !page.resolved.empty() &&
            FileExists(page.resolved)) {
            Texture::ImageData img;
            if (Texture::DecodeImageFile(page.resolved, &img, false)) {
                if (page.width <= 0) page.width = img.width;
                if (page.height <= 0) page.height = img.height;
            }
        }
        pages_.push_back(std::move(page));
    }

    // Дескрипторы без размера откатываются к объединению своих прямоугольников кадров.
    for (usize i = 0; i < pages_.size(); ++i) {
        AtlasPageDesc& page = pages_[i];
        if (page.width > 0 && page.height > 0) continue;
        f32 mx = 0.0f, my = 0.0f;
        for (const ParsedRegion& pr : parsed.regions) {
            if (pr.page != static_cast<int>(i)) continue;
            mx = MaxT(mx, pr.frame.Right());
            my = MaxT(my, pr.frame.Bottom());
        }
        if (mx > 0.0f && page.width <= 0) page.width = static_cast<int>(std::ceil(mx));
        if (my > 0.0f && page.height <= 0) page.height = static_cast<int>(std::ceil(my));
    }

    for (const ParsedRegion& pr : parsed.regions) {
        AtlasRegion r;
        r.name = pr.name;
        r.frame = pr.frame;
        r.pivot = pr.pivot;
        r.rotated = pr.rotated;
        r.trimmed = pr.trimmed;
        r.sourceSize = pr.sourceSize;
        r.spriteSourceSize = pr.spriteSourceSize;
        r.page = Clamp(pr.page, 0, static_cast<int>(pages_.size()) - 1);
        r.durationMs = pr.durationMs;
        r.polygon = pr.polygon;
        const AtlasPageDesc& page = pages_[static_cast<usize>(r.page)];
        if (page.width > 0 && page.height > 0) {
            r.uv = Rect{r.frame.x / page.width, r.frame.y / page.height, r.frame.w / page.width,
                        r.frame.h / page.height};
        } else {
            r.uv = Rect{0, 0, 0, 0};
        }
        regions_.push_back(std::move(r));
    }
    RebuildLookup(regions_, lookup_);

    for (const ParsedAnim& pa : parsed.anims) {
        AtlasAnimation a;
        a.name = pa.name;
        a.loop = pa.loop;
        a.durations = pa.durations;
        if (!pa.frameNames.empty()) {
            for (const std::string& n : pa.frameNames) {
                const auto it = lookup_.find(n);
                if (it != lookup_.end()) a.frames.push_back(it->second);
            }
        } else {
            for (int idx : pa.frameIndices) {
                if (idx >= 0 && idx < RegionCount()) a.frames.push_back(idx);
            }
        }
        if (a.frames.empty()) continue;
        if (a.durations.size() < a.frames.size()) a.durations.resize(a.frames.size(), 100.0f);
        animations_.push_back(std::move(a));
    }

    RecomputeStats();
    if (animations_.empty()) BuildAnimationsFromPrefixes();
    return true;
}

bool SpriteAtlas::LoadPlainImage(const std::string& imagePath, bool srgb) {
    Clear();
    if (imagePath.empty()) {
        AddWarning(warnings_, "atlas: empty image path");
        return false;
    }
    Texture::ImageData img;
    if (!Texture::DecodeImageFile(imagePath, &img, false)) {
        AddWarning(warnings_, "atlas: cannot decode image " + imagePath);
        return false;
    }
    source_ = imagePath;
    format_ = AtlasFormat::PlainImage;

    AtlasPageDesc page;
    page.image = PathBase(imagePath);
    page.resolved = imagePath;
    page.width = img.width;
    page.height = img.height;
    if (!page.texture.LoadFromFile(imagePath, srgb, false))
        AddWarning(warnings_, "atlas: image decoded but not uploaded for " + imagePath +
                                  " (no GL context); drawing disabled");
    pages_.push_back(std::move(page));

    AtlasRegion r;
    r.name = StemOf(imagePath);
    if (r.name.empty()) r.name = "image";
    r.frame = Rect{0, 0, static_cast<f32>(img.width), static_cast<f32>(img.height)};
    r.uv = Rect{0, 0, 1, 1};
    r.sourceSize = {static_cast<f32>(img.width), static_cast<f32>(img.height)};
    r.spriteSourceSize = r.frame;
    regions_.push_back(r);
    RebuildLookup(regions_, lookup_);

    // Храним пиксели на CPU, чтобы работали PagePixels()/SavePageImage().
    impl_->cpu.clear();
    Impl::CpuPage cpu;
    cpu.width = img.width;
    cpu.height = img.height;
    cpu.rgba = std::move(img.pixels);
    impl_->cpu.push_back(std::move(cpu));

    RecomputeStats();
    return true;
}

bool SpriteAtlas::LoadFromFile(const std::string& descriptorPath, bool srgb) {
    Clear();
    if (descriptorPath.empty()) {
        AddWarning(warnings_, "atlas: empty descriptor path");
        return false;
    }
    if (!FileExists(descriptorPath)) {
        AddWarning(warnings_, "atlas: descriptor not found: " + descriptorPath);
        return false;
    }
    const std::string text = ReadTextFile(descriptorPath);
    source_ = descriptorPath;
    format_ = DetectFormat(text);

    if (format_ == AtlasFormat::Unknown) {
        // Не текстовый дескриптор: «голый» файл изображения — валидный атлас из одного региона.
        Texture::ImageData probe;
        if (Texture::DecodeImageFile(descriptorPath, &probe, false))
            return LoadPlainImage(descriptorPath, srgb);
        AddWarning(warnings_, "atlas: unrecognised descriptor format: " + descriptorPath);
        return false;
    }
    if (format_ == AtlasFormat::PlainImage) {
        const std::string image = Trim(text);
        if (image.empty()) {
            AddWarning(warnings_, "atlas: bare image descriptor is empty: " + descriptorPath);
            return false;
        }
        return LoadPlainImage(PathJoin(PathDir(descriptorPath), image), srgb);
    }
    return LoadPagesAndRegions(text, PathDir(descriptorPath), srgb);
}

bool SpriteAtlas::LoadFromMemory(const std::string& descriptor, const std::string& baseDir,
                                 const std::string& debugName, bool srgb) {
    Clear();
    source_ = debugName;
    format_ = DetectFormat(descriptor);
    if (format_ == AtlasFormat::PlainImage) {
        const std::string image = Trim(descriptor);
        const std::string resolved = baseDir.empty() ? image : PathJoin(baseDir, image);
        return LoadPlainImage(resolved, srgb);
    }
    if (format_ == AtlasFormat::Unknown) {
        AddWarning(warnings_, "atlas: unrecognised descriptor: " + debugName);
        return false;
    }
    return LoadPagesAndRegions(descriptor, baseDir, srgb);
}

// ---------------------------------------------------------------------------
// Сохранение (engine JSON)
// ---------------------------------------------------------------------------
bool SpriteAtlas::SaveToFile(const std::string& path, bool writePageImages) const {
    if (path.empty()) return false;

    JsonObject root;
    JsonObject meta;
    meta["app"] = JsonValue("CrossRender");
    meta["version"] = JsonValue("1");

    JsonArray pageArray;
    for (usize i = 0; i < pages_.size(); ++i) {
        const AtlasPageDesc& p = pages_[i];
        int w = p.width;
        int h = p.height;
        if (w <= 0 && p.texture.Valid()) w = p.texture.Width();
        if (h <= 0 && p.texture.Valid()) h = p.texture.Height();
        std::string image = p.image;
        if (writePageImages) {
            std::vector<u8> rgba;
            int pw = 0, ph = 0;
            if (PagePixels(static_cast<int>(i), &rgba, &pw, &ph)) {
                std::string stem = PathBase(path);
                const usize dot = stem.find_last_of('.');
                if (dot != kNpos && dot > 0) stem = stem.substr(0, dot);
                if (stem.empty()) stem = "atlas";
                const std::string file =
                    stem + (i == 0 ? std::string() : "_" + std::to_string(i)) + ".png";
                const std::string full = PathJoin(PathDir(path), file);
                if (Texture::EncodePng(full, pw, ph, 4, rgba.data())) {
                    image = file;
                    w = pw;
                    h = ph;
                } else {
                    ENG_LOGW("atlas", "could not write page image %s", full.c_str());
                }
            }
        }
        if (i == 0) {
            meta["image"] = JsonValue(image);
            JsonObject size;
            size["w"] = JsonValue(w);
            size["h"] = JsonValue(h);
            meta["size"] = JsonValue(size);
        }
        JsonObject po;
        po["image"] = JsonValue(image);
        po["w"] = JsonValue(w);
        po["h"] = JsonValue(h);
        pageArray.push_back(JsonValue(po));
    }
    if (pages_.empty()) {
        meta["image"] = JsonValue("");
        JsonObject size;
        size["w"] = JsonValue(0);
        size["h"] = JsonValue(0);
        meta["size"] = JsonValue(size);
    }
    meta["scale"] = JsonValue("1");
    root["meta"] = JsonValue(meta);
    if (pages_.size() > 1) root["pages"] = JsonValue(pageArray);

    JsonObject frames;
    for (const AtlasRegion& r : regions_) {
        JsonObject f;
        JsonObject fr;
        fr["x"] = JsonValue(r.frame.x);
        fr["y"] = JsonValue(r.frame.y);
        fr["w"] = JsonValue(r.frame.w);
        fr["h"] = JsonValue(r.frame.h);
        f["frame"] = JsonValue(fr);
        f["rotated"] = JsonValue(r.rotated);
        f["trimmed"] = JsonValue(r.trimmed);
        JsonObject sss;
        sss["x"] = JsonValue(r.spriteSourceSize.x);
        sss["y"] = JsonValue(r.spriteSourceSize.y);
        sss["w"] = JsonValue(r.spriteSourceSize.w);
        sss["h"] = JsonValue(r.spriteSourceSize.h);
        f["spriteSourceSize"] = JsonValue(sss);
        JsonObject src;
        src["w"] = JsonValue(r.sourceSize.x);
        src["h"] = JsonValue(r.sourceSize.y);
        f["sourceSize"] = JsonValue(src);
        JsonObject pivot;
        pivot["x"] = JsonValue(r.pivot.x);
        pivot["y"] = JsonValue(r.pivot.y);
        f["pivot"] = JsonValue(pivot);
        if (r.durationMs > 0.0f) f["duration"] = JsonValue(r.durationMs);
        if (r.page != 0) f["page"] = JsonValue(r.page);
        if (!r.polygon.empty()) {
            JsonArray poly;
            for (const Vec2& v : r.polygon) {
                JsonObject po;
                po["x"] = JsonValue(v.x);
                po["y"] = JsonValue(v.y);
                poly.push_back(JsonValue(po));
            }
            f["polygon"] = JsonValue(poly);
        }
        frames[r.name] = JsonValue(f);
    }
    root["frames"] = JsonValue(frames);

    if (!animations_.empty()) {
        JsonArray arr;
        for (const AtlasAnimation& a : animations_) {
            JsonObject ao;
            ao["name"] = JsonValue(a.name);
            JsonArray fr;
            for (int idx : a.frames) fr.push_back(JsonValue(RegionAt(idx).name));
            ao["frames"] = JsonValue(fr);
            JsonArray du;
            for (f32 d : a.durations) du.push_back(JsonValue(d));
            ao["durations"] = JsonValue(du);
            ao["loop"] = JsonValue(a.loop);
            arr.push_back(JsonValue(ao));
        }
        root["animations"] = JsonValue(arr);
    }

    return WriteTextFile(path, JsonValue(root).Dump(2) + "\n");
}

// ---------------------------------------------------------------------------
// Упаковка
// ---------------------------------------------------------------------------
bool SpriteAtlas::BuildFromFiles(const std::vector<std::string>& imagePaths,
                                 const PackOptions& opts) {
    Clear();
    format_ = AtlasFormat::PlainImage;
    if (imagePaths.empty()) {
        AddWarning(warnings_, "atlas: BuildFromFiles called with no images");
        return false;
    }

    const int pad = MaxT(opts.padding, 0);
    int limit = MaxT(opts.maxSize, 8);
    if (opts.powerOfTwo) {
        int p = 8;
        while (p * 2 <= limit) p *= 2;
        limit = p;
    }

    // ---- декодирование (и при желании обрезка) каждого исходного изображения -------------------
    std::vector<PackItem> items;
    items.reserve(imagePaths.size());
    std::unordered_map<std::string, int> usedNames;
    for (const std::string& path : imagePaths) {
        Texture::ImageData img;
        if (!Texture::DecodeImageFile(path, &img, false)) {
            AddWarning(warnings_, "atlas: cannot decode image " + path);
            continue;
        }
        if (img.width <= 0 || img.height <= 0 ||
            img.pixels.size() < static_cast<usize>(img.width) * img.height * 4) {
            AddWarning(warnings_, "atlas: image has no usable pixels: " + path);
            continue;
        }
        PackItem it;
        it.path = path;
        it.originalW = img.width;
        it.originalH = img.height;

        std::string stem = StemOf(path);
        if (stem.empty()) stem = "region";
        int& count = usedNames[stem];
        if (count > 0) {
            std::string unique = stem + "_" + std::to_string(count);
            while (usedNames.count(unique) != 0) unique += "_";
            AddWarning(warnings_,
                       "atlas: duplicate region name '" + stem + "', renamed to '" + unique + "'");
            usedNames[unique] = 1;
            stem = unique;
        }
        ++count;
        it.name = stem;

        int x0 = 0, y0 = 0, x1 = img.width, y1 = img.height;
        if (opts.trim) ComputeAlphaBounds(img, &x0, &y0, &x1, &y1);
        it.trimX = x0;
        it.trimY = y0;
        it.w = x1 - x0;
        it.h = y1 - y0;
        it.trimmed = x0 != 0 || y0 != 0 || it.w != img.width || it.h != img.height;
        it.rgba.resize(static_cast<usize>(it.w) * it.h * 4);
        for (int y = 0; y < it.h; ++y) {
            const u8* srcRow = img.pixels.data() + (static_cast<usize>(y + y0) * img.width + x0) * 4;
            u8* dstRow = it.rgba.data() + static_cast<usize>(y) * it.w * 4;
            std::memcpy(dstRow, srcRow, static_cast<usize>(it.w) * 4);
        }
        items.push_back(std::move(it));
    }
    if (items.empty()) {
        AddWarning(warnings_, "atlas: no images could be decoded");
        RecomputeStats();
        return false;
    }

    // ---- shelf/skyline-упаковка --------------------------------------------
    std::vector<int> order(items.size());
    for (usize i = 0; i < items.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&items](int a, int b) {
        if (items[a].h != items[b].h) return items[a].h > items[b].h;
        if (items[a].w != items[b].w) return items[a].w > items[b].w;
        return items[a].name < items[b].name;
    });

    i64 totalArea = 0;
    for (const PackItem& it : items)
        totalArea += static_cast<i64>(it.w + pad) * static_cast<i64>(it.h + pad);
    int initialEdge = 16;
    const int wantEdge = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(totalArea))));
    while (initialEdge < wantEdge && initialEdge < limit) initialEdge *= 2;
    if (!opts.powerOfTwo) initialEdge = Clamp(wantEdge, 8, limit);
    if (initialEdge > limit) initialEdge = limit;
    if (initialEdge < 8) initialEdge = 8;

    std::vector<Placement> placed;
    placed.reserve(items.size());
    std::vector<int> pageEdges;
    int page = 0;
    int edge = initialEdge;
    pageEdges.push_back(edge);
    int shelfY = 0, shelfH = 0, cursorX = 0;

    auto tryPlace = [&](int iw, int ih, int* outX, int* outY) -> bool {
        int cx = cursorX;
        int sy = shelfY;
        int sh = shelfH;
        if (cx + iw > edge) {
            sy = sy + sh + (sh > 0 ? pad : 0);
            sh = 0;
            cx = 0;
        }
        if (cx + iw > edge) return false;
        if (sy + ih > edge) return false;
        *outX = cx;
        *outY = sy;
        cursorX = cx + iw + pad;
        shelfY = sy;
        shelfH = MaxT(sh, ih);
        return true;
    };

    for (int oi : order) {
        PackItem& it = items[static_cast<usize>(oi)];
        if (it.w > limit || it.h > limit) {
            AddWarning(warnings_,
                       FormatMsg("atlas: image '%s' (%dx%d) does not fit the %d px page limit; "
                                 "skipped",
                                 it.name.c_str(), it.w, it.h, limit));
            continue;
        }
        for (;;) {
            bool rot = false;
            if (opts.allowRotate && it.w != it.h) {
                const int nextShelf = shelfY + shelfH + (shelfH > 0 ? pad : 0);
                const int remaining = edge - nextShelf;
                const bool tall = it.h > it.w;
                if (tall && ((it.h > remaining && it.w <= remaining) ||
                             (shelfH > 0 && it.w <= shelfH && it.h > shelfH)))
                    rot = true;
            }
            const int tw = rot ? it.h : it.w;
            const int th = rot ? it.w : it.h;
            int px = 0, py = 0;
            if (tryPlace(tw, th, &px, &py)) {
                placed.push_back(Placement{oi, page, px, py, rot});
                break;
            }
            if (edge < limit) {
                // Увеличиваем текущую страницу (сохраняя состояние полок) и повторяем.
                edge = MinT(limit, edge * 2);
                pageEdges[static_cast<usize>(page)] = edge;
                continue;
            }
            // Текущая страница заполнена: открываем новую.
            ++page;
            edge = initialEdge;
            pageEdges.push_back(edge);
            shelfY = shelfH = cursorX = 0;
        }
    }

    // ---- компоновка в CPU-страницы ----------------------------------------
    impl_->cpu.clear();
    impl_->cpu.resize(pageEdges.size());
    const u8 bg[4] = {ColorByte(opts.background.r), ColorByte(opts.background.g),
                      ColorByte(opts.background.b), ColorByte(opts.background.a)};
    for (usize i = 0; i < pageEdges.size(); ++i) {
        Impl::CpuPage& buf = impl_->cpu[i];
        buf.width = pageEdges[i];
        buf.height = pageEdges[i];
        buf.rgba.resize(static_cast<usize>(buf.width) * buf.height * 4);
        for (usize p = 0; p + 3 < buf.rgba.size(); p += 4) {
            buf.rgba[p + 0] = bg[0];
            buf.rgba[p + 1] = bg[1];
            buf.rgba[p + 2] = bg[2];
            buf.rgba[p + 3] = bg[3];
        }
    }
    for (const Placement& pl : placed) {
        const PackItem& it = items[static_cast<usize>(pl.item)];
        if (pl.page < 0 || static_cast<usize>(pl.page) >= impl_->cpu.size()) continue;
        Impl::CpuPage& buf = impl_->cpu[static_cast<usize>(pl.page)];
        for (int y = 0; y < it.h; ++y) {
            for (int x = 0; x < it.w; ++x) {
                int dx, dy;
                if (pl.rotated) {
                    // 90 градусов по часовой: логический (x,y) -> хранимый (h-1-y, x).
                    dx = pl.x + (it.h - 1 - y);
                    dy = pl.y + x;
                } else {
                    dx = pl.x + x;
                    dy = pl.y + y;
                }
                if (dx < 0 || dy < 0 || dx >= buf.width || dy >= buf.height) continue;
                std::memcpy(&buf.rgba[(static_cast<usize>(dy) * buf.width + dx) * 4],
                            &it.rgba[(static_cast<usize>(y) * it.w + x) * 4], 4);
            }
        }
    }

    // ---- регионы + страницы --------------------------------------------------
    for (const Placement& pl : placed) {
        const PackItem& it = items[static_cast<usize>(pl.item)];
        AtlasRegion r;
        r.name = it.name;
        const int storedW = pl.rotated ? it.h : it.w;
        const int storedH = pl.rotated ? it.w : it.h;
        r.frame = Rect{static_cast<f32>(pl.x), static_cast<f32>(pl.y), static_cast<f32>(storedW),
                       static_cast<f32>(storedH)};
        r.rotated = pl.rotated;
        r.trimmed = it.trimmed;
        r.sourceSize = {static_cast<f32>(it.originalW), static_cast<f32>(it.originalH)};
        r.spriteSourceSize = {static_cast<f32>(it.trimX), static_cast<f32>(it.trimY),
                              static_cast<f32>(it.w), static_cast<f32>(it.h)};
        r.pivot = {0.5f, 0.5f};
        r.page = pl.page;
        r.durationMs = 0.0f;
        regions_.push_back(std::move(r));
    }

    pages_.resize(impl_->cpu.size());
    for (usize i = 0; i < impl_->cpu.size(); ++i) {
        Impl::CpuPage& buf = impl_->cpu[i];
        AtlasPageDesc& pageDesc = pages_[i];
        pageDesc.image = FormatMsg("page_%d.png", static_cast<int>(i));
        pageDesc.resolved.clear();
        pageDesc.width = buf.width;
        pageDesc.height = buf.height;
        const bool ok = pageDesc.texture.Create(buf.width, buf.height, PixelFormat::RGBA8, nullptr,
                                                TextureFilter::Linear, TextureWrap::ClampToEdge,
                                                false);
        if (ok) {
            pageDesc.texture.Update(buf.rgba.data());
            pageDesc.texture.SetDebugName(FormatMsg("atlas-page-%d", static_cast<int>(i)));
        } else {
            AddWarning(warnings_, FormatMsg("atlas: page %d could not be uploaded (no GL context)",
                                            static_cast<int>(i)));
        }
    }

    for (AtlasRegion& r : regions_) {
        if (r.page < 0 || static_cast<usize>(r.page) >= pages_.size()) continue;
        const AtlasPageDesc& p = pages_[static_cast<usize>(r.page)];
        if (p.width > 0 && p.height > 0) {
            r.uv = Rect{r.frame.x / p.width, r.frame.y / p.height, r.frame.w / p.width,
                        r.frame.h / p.height};
        }
    }
    RebuildLookup(regions_, lookup_);
    RecomputeStats();
    return !regions_.empty();
}

void SpriteAtlas::AddRegion(const AtlasRegion& region) {
    regions_.push_back(region);
    lookup_[region.name] = static_cast<int>(regions_.size()) - 1;
    RecomputeStats();
}

bool SpriteAtlas::SetPageImage(int page, const std::string& imagePath, bool srgb) {
    if (page < 0) return false;
    while (static_cast<int>(pages_.size()) <= page) pages_.push_back(AtlasPageDesc{});
    AtlasPageDesc& p = pages_[static_cast<usize>(page)];
    p.image = imagePath;
    p.resolved = imagePath;
    const bool ok = p.texture.LoadFromFile(imagePath, srgb, false);
    if (!ok) AddWarning(warnings_, "atlas: cannot load page image " + imagePath);
    if (p.texture.Valid()) {
        p.width = p.texture.Width();
        p.height = p.texture.Height();
        for (AtlasRegion& r : regions_) {
            if (r.page != page) continue;
            if (p.width > 0 && p.height > 0)
                r.uv = Rect{r.frame.x / p.width, r.frame.y / p.height, r.frame.w / p.width,
                            r.frame.h / p.height};
        }
    }
    RecomputeStats();
    return ok;
}

void SpriteAtlas::SortRegionsByName() {
    std::stable_sort(regions_.begin(), regions_.end(),
                     [](const AtlasRegion& a, const AtlasRegion& b) {
                         return atlas_detail::NaturalLess(a.name, b.name);
                     });
    RebuildLookup(regions_, lookup_);
    RecomputeStats();
}

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------
const AtlasRegion& SpriteAtlas::RegionAt(int index) const {
    static const AtlasRegion kEmpty{};
    if (index < 0 || index >= RegionCount()) return kEmpty;
    return regions_[static_cast<usize>(index)];
}

int SpriteAtlas::IndexOf(const std::string& name) const {
    const auto it = lookup_.find(name);
    return it == lookup_.end() ? -1 : it->second;
}

const AtlasRegion* SpriteAtlas::Find(const std::string& name) const {
    const int i = IndexOf(name);
    return i < 0 ? nullptr : &regions_[static_cast<usize>(i)];
}

std::vector<std::string> SpriteAtlas::Names() const {
    std::vector<std::string> out;
    out.reserve(regions_.size());
    for (const AtlasRegion& r : regions_) out.push_back(r.name);
    return out;
}

std::vector<std::string> SpriteAtlas::NamesWithPrefix(const std::string& prefix) const {
    std::vector<std::string> out;
    for (const AtlasRegion& r : regions_) {
        if (r.name.compare(0, prefix.size(), prefix) == 0) out.push_back(r.name);
    }
    std::sort(out.begin(), out.end(), atlas_detail::NaturalLess);
    return out;
}

const AtlasPageDesc& SpriteAtlas::Page(int index) const {
    static const AtlasPageDesc kEmpty{};
    if (index < 0 || index >= PageCount()) return kEmpty;
    return pages_[static_cast<usize>(index)];
}

const Texture& SpriteAtlas::PageTexture(int index) const { return Page(index).texture; }

const Texture& SpriteAtlas::TextureFor(const AtlasRegion& region) const {
    return PageTexture(region.page);
}

void SpriteAtlas::RecomputeStats() {
    stats_ = Stats{};
    stats_.regions = RegionCount();
    stats_.pages = PageCount();
    for (const AtlasRegion& r : regions_) {
        const Vec2 s = r.Size();
        stats_.totalPixels += static_cast<int>(s.x * s.y);
    }
    for (const AtlasPageDesc& p : pages_) stats_.pagePixels += p.width * p.height;
    stats_.occupancy =
        stats_.pagePixels > 0
            ? static_cast<f32>(stats_.totalPixels) / static_cast<f32>(stats_.pagePixels)
            : 0.0f;
}

bool SpriteAtlas::PagePixels(int page, std::vector<u8>* rgba, int* width, int* height) const {
    if (!impl_ || page < 0 || static_cast<usize>(page) >= impl_->cpu.size()) return false;
    const Impl::CpuPage& p = impl_->cpu[static_cast<usize>(page)];
    if (p.width <= 0 || p.height <= 0 || p.rgba.empty()) return false;
    if (rgba) *rgba = p.rgba;
    if (width) *width = p.width;
    if (height) *height = p.height;
    return true;
}

bool SpriteAtlas::SavePageImage(const std::string& pngPath, int page) const {
    std::vector<u8> rgba;
    int w = 0, h = 0;
    if (!PagePixels(page, &rgba, &w, &h)) return false;
    return Texture::EncodePng(pngPath, w, h, 4, rgba.data());
}

// ---------------------------------------------------------------------------
// Отрисовка
// ---------------------------------------------------------------------------
bool SpriteAtlas::Draw(Renderer2D& r2d, const std::string& name, const Rect& dst,
                       const Color& tint) const {
    const int i = IndexOf(name);
    if (i < 0) return false;
    return DrawRegion(r2d, i, dst, tint, false, false);
}

bool SpriteAtlas::DrawRegion(Renderer2D& r2d, int regionIndex, const Rect& dst, const Color& tint,
                             bool flipX, bool flipY) const {
    if (regionIndex < 0 || regionIndex >= RegionCount()) return false;
    const AtlasRegion& r = regions_[static_cast<usize>(regionIndex)];
    const Texture& tex = TextureFor(r);
    if (!tex.Valid()) return false;
    if (dst.w <= 0.0f || dst.h <= 0.0f) return false;
    DrawRegionQuad(r2d, tex, r, Rect{0, 0, r.Size().x, r.Size().y}, dst, tint, flipX, flipY);
    return true;
}

bool SpriteAtlas::DrawAnchored(Renderer2D& r2d, const std::string& name, Vec2 position,
                               const Color& tint, f32 scale) const {
    const int i = IndexOf(name);
    if (i < 0) return false;
    const AtlasRegion& r = regions_[static_cast<usize>(i)];
    const Vec2 size = r.Size() * scale;
    const Rect dst{position.x - r.pivot.x * size.x, position.y - r.pivot.y * size.y, size.x,
                   size.y};
    return DrawRegion(r2d, i, dst, tint, false, false);
}

bool SpriteAtlas::DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst,
                                f32 border, const Color& tint) const {
    return DrawNinePatch(r2d, name, dst, border, border, border, border, tint);
}

bool SpriteAtlas::DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 left,
                                f32 top, f32 right, f32 bottom, const Color& tint) const {
    const int i = IndexOf(name);
    if (i < 0) return false;
    const AtlasRegion& r = regions_[static_cast<usize>(i)];
    const Texture& tex = TextureFor(r);
    if (!tex.Valid()) return false;
    const Vec2 size = r.Size();
    atlas_detail::NinePatchQuad quads[9];
    const int n = atlas_detail::NinePatchQuads(Rect{0, 0, size.x, size.y}, dst, left, top, right,
                                               bottom, quads);
    if (n <= 0) return false;
    for (int q = 0; q < n; ++q)
        DrawRegionQuad(r2d, tex, r, quads[q].src, quads[q].dst, tint, false, false);
    return true;
}

bool SpriteAtlas::DrawTiled(Renderer2D& r2d, const std::string& name, const Rect& dst,
                            const Color& tint) const {
    const int i = IndexOf(name);
    if (i < 0) return false;
    const AtlasRegion& r = regions_[static_cast<usize>(i)];
    const Texture& tex = TextureFor(r);
    if (!tex.Valid()) return false;
    const Vec2 size = r.Size();
    if (size.x < 1.0f || size.y < 1.0f || dst.w <= 0.0f || dst.h <= 0.0f) return false;
    const int cols = static_cast<int>(std::ceil(dst.w / size.x));
    const int rows = static_cast<int>(std::ceil(dst.h / size.y));
    if (cols <= 0 || rows <= 0) return false;
    if (static_cast<i64>(cols) * rows > 100000) return false;  // предохранитель
    bool any = false;
    for (int ry = 0; ry < rows; ++ry) {
        const f32 ty = dst.y + ry * size.y;
        const f32 th = MinT(size.y, dst.Bottom() - ty);
        for (int cx = 0; cx < cols; ++cx) {
            const f32 tx = dst.x + cx * size.x;
            const f32 tw = MinT(size.x, dst.Right() - tx);
            if (tw <= 0.0f || th <= 0.0f) continue;
            DrawRegionQuad(r2d, tex, r, Rect{0, 0, tw, th}, Rect{tx, ty, tw, th}, tint, false, false);
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------
// Анимации
// ---------------------------------------------------------------------------
f32 AtlasAnimation::TotalDuration() const {
    if (frames.empty()) return 0.0f;
    f32 total = 0.0f;
    for (usize i = 0; i < frames.size(); ++i) total += FrameDurationMs(*this, i);
    return total;
}

int AtlasAnimation::FrameAt(f32 timeSeconds) const {
    const usize n = frames.size();
    if (n == 0) return -1;
    const f32 total = TotalDuration();  // миллисекунды
    if (total <= 0.0f) return 0;
    f32 t = timeSeconds * 1000.0f;  // секунды -> миллисекунды
    if (t < 0.0f) t = 0.0f;
    if (loop) {
        t = std::fmod(t, total);
        if (t < 0.0f) t += total;
    } else if (t >= total) {
        return static_cast<int>(n) - 1;
    }
    f32 acc = 0.0f;
    for (usize i = 0; i < n; ++i) {
        acc += FrameDurationMs(*this, i);
        if (t < acc) return static_cast<int>(i);
    }
    return static_cast<int>(n) - 1;
}

int SpriteAtlas::AddAnimation(const AtlasAnimation& anim) {
    for (usize i = 0; i < animations_.size(); ++i) {
        if (animations_[i].name == anim.name) {
            animations_[i] = anim;
            return static_cast<int>(i);
        }
    }
    animations_.push_back(anim);
    return static_cast<int>(animations_.size()) - 1;
}

const AtlasAnimation* SpriteAtlas::FindAnimation(const std::string& name) const {
    for (const AtlasAnimation& a : animations_) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::vector<std::string> SpriteAtlas::AnimationNames() const {
    std::vector<std::string> out;
    out.reserve(animations_.size());
    for (const AtlasAnimation& a : animations_) out.push_back(a.name);
    std::sort(out.begin(), out.end(), atlas_detail::NaturalLess);
    return out;
}

int SpriteAtlas::BuildAnimationsFromPrefixes(const std::string& separator) {
    if (separator.empty()) return 0;
    struct Entry {
        long long number = 0;
        std::string name;
    };
    std::map<std::string, std::vector<Entry>> groups;
    for (const AtlasRegion& r : regions_) {
        const usize p = r.name.rfind(separator);
        if (p == std::string::npos) continue;
        const std::string base = r.name.substr(0, p);
        const std::string suffix = r.name.substr(p + separator.size());
        if (base.empty() || suffix.empty()) continue;
        bool digits = true;
        for (char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                digits = false;
                break;
            }
        }
        if (!digits) continue;
        Entry e;
        e.number = std::strtoll(suffix.c_str(), nullptr, 10);
        e.name = r.name;
        groups[base].push_back(std::move(e));
    }

    int created = 0;
    for (auto& kv : groups) {
        if (kv.second.size() < 2) continue;
        if (FindAnimation(kv.first) != nullptr) continue;
        std::sort(kv.second.begin(), kv.second.end(), [](const Entry& a, const Entry& b) {
            if (a.number != b.number) return a.number < b.number;
            return atlas_detail::NaturalLess(a.name, b.name);
        });
        AtlasAnimation anim;
        anim.name = kv.first;
        for (const Entry& e : kv.second) {
            const int idx = IndexOf(e.name);
            if (idx < 0) continue;
            anim.frames.push_back(idx);
            const f32 d = regions_[static_cast<usize>(idx)].durationMs;
            anim.durations.push_back(d > 0.0f ? d : 100.0f);
        }
        if (anim.frames.size() < 2) continue;
        anim.loop = true;
        animations_.push_back(std::move(anim));
        ++created;
    }
    return created;
}

int SpriteAtlas::AnimationFrameAt(const std::string& name, f32 timeSeconds) const {
    const AtlasAnimation* a = FindAnimation(name);
    return a ? a->FrameAt(timeSeconds) : -1;
}

bool SpriteAtlas::DrawAnimation(Renderer2D& r2d, const std::string& name, const Rect& dst,
                                f32 timeSeconds, const Color& tint) const {
    const int frame = AnimationFrameAt(name, timeSeconds);
    if (frame < 0) return false;
    return DrawAnimationFrame(r2d, name, frame, dst, tint);
}

bool SpriteAtlas::DrawAnimationFrame(Renderer2D& r2d, const std::string& name, int frame,
                                     const Rect& dst, const Color& tint) const {
    const AtlasAnimation* a = FindAnimation(name);
    if (!a || a->frames.empty()) return false;
    const int clamped = Clamp(frame, 0, static_cast<int>(a->frames.size()) - 1);
    return DrawRegion(r2d, a->frames[static_cast<usize>(clamped)], dst, tint, false, false);
}

}  // namespace crossrender
