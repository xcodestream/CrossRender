#include "crossrender/gfx/Texture.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <algorithm>

// stb_image / stb_image_write инстанцируются в engine/third_party/stb_impl.c;
// здесь нужны только объявления.
#include "stb_image.h"
#include "stb_image_write.h"

namespace crossrender {
namespace {

struct GlFormat {
    gl::GLenum internal = 0;
    gl::GLenum format = 0;
    gl::GLenum type = 0;
};

GlFormat ToGl(PixelFormat f) {
    switch (f) {
        case PixelFormat::R8: return {gl::GL_R8, gl::GL_RED, gl::GL_UNSIGNED_BYTE};
        case PixelFormat::RG8: return {gl::GL_RG8, gl::GL_RG, gl::GL_UNSIGNED_BYTE};
        case PixelFormat::RGB8: return {gl::GL_RGB8, gl::GL_RGB, gl::GL_UNSIGNED_BYTE};
        case PixelFormat::RGBA8: return {gl::GL_RGBA8, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE};
        case PixelFormat::SRGBA8: return {gl::GL_SRGB8_ALPHA8, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE};
        case PixelFormat::R16F: return {gl::GL_R16F, gl::GL_RED, gl::GL_HALF_FLOAT};
        case PixelFormat::RG16F: return {gl::GL_RG16F, gl::GL_RG, gl::GL_HALF_FLOAT};
        case PixelFormat::RGB16F: return {gl::GL_RGB16F, gl::GL_RGB, gl::GL_HALF_FLOAT};
        case PixelFormat::RGBA16F: return {gl::GL_RGBA16F, gl::GL_RGBA, gl::GL_HALF_FLOAT};
        case PixelFormat::R32F: return {gl::GL_R32F, gl::GL_RED, gl::GL_FLOAT};
        case PixelFormat::RGBA32F: return {gl::GL_RGBA32F, gl::GL_RGBA, gl::GL_FLOAT};
        case PixelFormat::Depth16: return {gl::GL_DEPTH_COMPONENT16, gl::GL_DEPTH_COMPONENT, gl::GL_FLOAT};
        case PixelFormat::Depth24: return {gl::GL_DEPTH_COMPONENT24, gl::GL_DEPTH_COMPONENT, gl::GL_FLOAT};
        case PixelFormat::Depth24Stencil8:
            return {gl::GL_DEPTH24_STENCIL8, gl::GL_DEPTH_STENCIL, gl::GL_UNSIGNED_INT_24_8};
        case PixelFormat::Depth32F: return {gl::GL_DEPTH_COMPONENT32F, gl::GL_DEPTH_COMPONENT, gl::GL_FLOAT};
        case PixelFormat::RGB10A2:
            return {gl::GL_RGB10_A2, gl::GL_RGBA, gl::GL_UNSIGNED_INT_2_10_10_10_REV};
        default: return {gl::GL_RGBA8, gl::GL_RGBA, gl::GL_UNSIGNED_BYTE};
    }
}

gl::GLenum ToGlFilter(TextureFilter f) {
    switch (f) {
        case TextureFilter::Nearest: return gl::GL_NEAREST;
        case TextureFilter::Linear: return gl::GL_LINEAR;
        case TextureFilter::NearestMipmapNearest: return gl::GL_NEAREST_MIPMAP_NEAREST;
        case TextureFilter::LinearMipmapLinear: return gl::GL_LINEAR_MIPMAP_LINEAR;
    }
    return gl::GL_LINEAR;
}

gl::GLenum ToGlWrap(TextureWrap w) {
    switch (w) {
        case TextureWrap::Repeat: return gl::GL_REPEAT;
        case TextureWrap::ClampToEdge: return gl::GL_CLAMP_TO_EDGE;
        case TextureWrap::MirroredRepeat: return gl::GL_MIRRORED_REPEAT;
        case TextureWrap::ClampToBorder: return gl::GL_CLAMP_TO_EDGE;
    }
    return gl::GL_CLAMP_TO_EDGE;
}

}  // namespace

int PixelFormatSize(PixelFormat f) {
    switch (f) {
        case PixelFormat::R8: return 1;
        case PixelFormat::RG8: return 2;
        case PixelFormat::RGB8: return 3;
        case PixelFormat::RGBA8:
        case PixelFormat::SRGBA8: return 4;
        case PixelFormat::R16F: return 2;
        case PixelFormat::RG16F: return 4;
        case PixelFormat::RGB16F: return 6;
        case PixelFormat::RGBA16F: return 8;
        case PixelFormat::R32F: return 4;
        case PixelFormat::RGBA32F: return 16;
        case PixelFormat::Depth16: return 2;
        case PixelFormat::Depth24: return 3;
        case PixelFormat::Depth24Stencil8: return 4;
        case PixelFormat::Depth32F: return 4;
        case PixelFormat::RGB10A2: return 4;
        default: return 0;
    }
}

bool PixelFormatIsDepth(PixelFormat f) {
    return f == PixelFormat::Depth16 || f == PixelFormat::Depth24 ||
           f == PixelFormat::Depth24Stencil8 || f == PixelFormat::Depth32F;
}

bool PixelFormatIsFloat(PixelFormat f) {
    return f == PixelFormat::R16F || f == PixelFormat::RG16F || f == PixelFormat::RGB16F ||
           f == PixelFormat::RGBA16F || f == PixelFormat::R32F || f == PixelFormat::RGBA32F;
}

Texture::~Texture() { Destroy(); }

Texture::Texture(Texture&& o) noexcept { *this = std::move(o); }

Texture& Texture::operator=(Texture&& o) noexcept {
    if (this == &o) return *this;
    Destroy();
    id_ = o.id_;
    width_ = o.width_;
    height_ = o.height_;
    depth_ = o.depth_;
    format_ = o.format_;
    cube_ = o.cube_;
    sdfSpread_ = o.sdfSpread_;
    sdfSize_ = o.sdfSize_;
    name_ = std::move(o.name_);
    o.id_ = 0;
    o.width_ = o.height_ = o.depth_ = 0;
    o.cube_ = false;
    return *this;
}

bool Texture::Create(int width, int height, PixelFormat format, const void* pixels,
                     TextureFilter filter, TextureWrap wrap, bool mipmaps) {
    Destroy();
    if (width <= 0 || height <= 0) {
        ENG_LOGE("texture", "invalid size %dx%d", width, height);
        return false;
    }
    if (!gl::glGenTextures) {
        // «Нет GL-контекста» — свойство процесса, а не конкретной текстуры:
        // CPU-утилита или headless-тест может создать их сотни, и стена
        // одинаковых предупреждений похоронит всё остальное в логе. Сообщаем
        // о состоянии один раз и дальше молчим.
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("texture",
                     "no GL context; textures stay on the CPU (first was %dx%d, "
                     "this warning is shown once)",
                     width, height);
        }
        return false;
    }
    width_ = width;
    height_ = height;
    depth_ = 1;
    format_ = format;
    cube_ = false;

    gl::glGenTextures(1, &id_);
    gl::glBindTexture(gl::GL_TEXTURE_2D, id_);
    GlFormat gf = ToGl(format);
    gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);

    bool depth = PixelFormatIsDepth(format);
    if (depth) {
        gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gf.internal), width, height, 0,
                         gf.format, gf.type, pixels);
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    } else {
        gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gf.internal), width, height, 0,
                         gf.format, gf.type, pixels);
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, ToGlFilter(filter));
        gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER,
                            mipmaps ? gl::GL_LINEAR_MIPMAP_LINEAR : ToGlFilter(filter));
        if (mipmaps) gl::glGenerateMipmap(gl::GL_TEXTURE_2D);
    }
    gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_S, ToGlWrap(wrap));
    gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_WRAP_T, ToGlWrap(wrap));
    gl::glBindTexture(gl::GL_TEXTURE_2D, 0);
    return true;
}

bool Texture::Create3D(int width, int height, int depth, PixelFormat format, const void* pixels,
                       TextureFilter filter, TextureWrap wrap) {
    Destroy();
    if (width <= 0 || height <= 0 || depth <= 0) return false;
    if (!gl::glGenTextures || !gl::glTexImage3D) return false;
    width_ = width;
    height_ = height;
    depth_ = depth;
    format_ = format;
    gl::glGenTextures(1, &id_);
    gl::glBindTexture(gl::GL_TEXTURE_3D, id_);
    GlFormat gf = ToGl(format);
    gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
    gl::glTexImage3D(gl::GL_TEXTURE_3D, 0, static_cast<gl::GLint>(gf.internal), width, height, depth,
                     0, gf.format, gf.type, pixels);
    gl::glTexParameteri(gl::GL_TEXTURE_3D, gl::GL_TEXTURE_MAG_FILTER, ToGlFilter(filter));
    gl::glTexParameteri(gl::GL_TEXTURE_3D, gl::GL_TEXTURE_MIN_FILTER, ToGlFilter(filter));
    gl::glTexParameteri(gl::GL_TEXTURE_3D, gl::GL_TEXTURE_WRAP_S, ToGlWrap(wrap));
    gl::glTexParameteri(gl::GL_TEXTURE_3D, gl::GL_TEXTURE_WRAP_T, ToGlWrap(wrap));
    gl::glTexParameteri(gl::GL_TEXTURE_3D, gl::GL_TEXTURE_WRAP_R, ToGlWrap(wrap));
    gl::glBindTexture(gl::GL_TEXTURE_3D, 0);
    return true;
}

bool Texture::CreateCubemap(int size, PixelFormat format, const void* const faces[6],
                            TextureFilter filter) {
    Destroy();
    if (size <= 0 || !faces) return false;
    if (!gl::glGenTextures) return false;
    width_ = height_ = size;
    depth_ = 1;
    format_ = format;
    cube_ = true;
    gl::glGenTextures(1, &id_);
    gl::glBindTexture(gl::GL_TEXTURE_CUBE_MAP, id_);
    GlFormat gf = ToGl(format);
    gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 6; ++i) {
        gl::glTexImage2D(static_cast<gl::GLenum>(gl::GL_TEXTURE_CUBE_MAP_POSITIVE_X + i), 0,
                         static_cast<gl::GLint>(gf.internal), size, size, 0, gf.format, gf.type,
                         faces[i]);
    }
    gl::glTexParameteri(gl::GL_TEXTURE_CUBE_MAP, gl::GL_TEXTURE_MAG_FILTER, ToGlFilter(filter));
    gl::glTexParameteri(gl::GL_TEXTURE_CUBE_MAP, gl::GL_TEXTURE_MIN_FILTER,
                        filter == TextureFilter::Nearest ? gl::GL_NEAREST : gl::GL_LINEAR);
    gl::glTexParameteri(gl::GL_TEXTURE_CUBE_MAP, gl::GL_TEXTURE_WRAP_S, gl::GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(gl::GL_TEXTURE_CUBE_MAP, gl::GL_TEXTURE_WRAP_T, gl::GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(gl::GL_TEXTURE_CUBE_MAP, gl::GL_TEXTURE_WRAP_R, gl::GL_CLAMP_TO_EDGE);
    gl::glBindTexture(gl::GL_TEXTURE_CUBE_MAP, 0);
    return true;
}

bool Texture::DecodeImage(const void* data, usize size, ImageData* out, bool flipVertically) {
    if (!data || size == 0 || !out) return false;
    int w = 0, h = 0, comp = 0;
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data),
                                            static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) {
        ENG_LOGE("texture", "image decode failed: %s", stbi_failure_reason());
        return false;
    }
    out->width = w;
    out->height = h;
    out->channels = 4;
    out->pixels.assign(pixels, pixels + static_cast<usize>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

bool Texture::DecodeImageFile(const std::string& path, ImageData* out, bool flipVertically) {
    ByteBuffer bytes = ReadBinaryFile(path);
    if (bytes.empty()) {
        ENG_LOGE("texture", "cannot read image %s", path.c_str());
        return false;
    }
    return DecodeImage(bytes.data(), bytes.size(), out, flipVertically);
}

bool Texture::LoadFromMemory(const void* data, usize size, bool srgb, bool mipmaps) {
    ImageData img;
    if (!DecodeImage(data, size, &img, false)) return false;
    bool ok = Create(img.width, img.height, srgb ? PixelFormat::SRGBA8 : PixelFormat::RGBA8,
                     img.pixels.data(), TextureFilter::Linear, TextureWrap::Repeat, mipmaps);
    return ok;
}

bool Texture::LoadFromFile(const std::string& path, bool srgb, bool mipmaps) {
    ByteBuffer bytes = ReadBinaryFile(path);
    if (bytes.empty()) return false;
    bool ok = LoadFromMemory(bytes.data(), bytes.size(), srgb, mipmaps);
    if (ok) name_ = path;
    return ok;
}

bool Texture::CreateSolid(const Color& c) {
    u8 px[4] = {static_cast<u8>(Clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<u8>(Clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<u8>(Clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<u8>(Clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f)};
    return Create(1, 1, PixelFormat::RGBA8, px, TextureFilter::Nearest, TextureWrap::ClampToEdge,
                  false);
}

bool Texture::CreateCheckerboard(int size, Color a, Color b) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    int cell = size / 8 > 0 ? size / 8 : 1;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool odd = ((x / cell) + (y / cell)) % 2 != 0;
            Color c = odd ? b : a;
            usize i = (static_cast<usize>(y) * size + x) * 4;
            pixels[i + 0] = static_cast<u8>(Clamp(c.r, 0.0f, 1.0f) * 255.0f);
            pixels[i + 1] = static_cast<u8>(Clamp(c.g, 0.0f, 1.0f) * 255.0f);
            pixels[i + 2] = static_cast<u8>(Clamp(c.b, 0.0f, 1.0f) * 255.0f);
            pixels[i + 3] = static_cast<u8>(Clamp(c.a, 0.0f, 1.0f) * 255.0f);
        }
    }
    return Create(size, size, PixelFormat::RGBA8, pixels.data(), TextureFilter::Linear,
                  TextureWrap::Repeat, true);
}

void Texture::Destroy() {
    if (id_ && gl::glDeleteTextures) {
        gl::glDeleteTextures(1, &id_);
    }
    id_ = 0;
    width_ = height_ = depth_ = 0;
    cube_ = false;
}

void Texture::Update(const void* pixels, int x, int y, int w, int h) {
    if (!id_ || !pixels || !gl::glTexSubImage2D) return;
    if (w < 0) w = width_ - x;
    if (h < 0) h = height_ - y;
    if (w <= 0 || h <= 0) return;
    GlFormat gf = ToGl(format_);
    gl::glBindTexture(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D, id_);
    gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
    gl::glTexSubImage2D(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D, 0, x, y, w, h, gf.format,
                        gf.type, pixels);
    gl::glBindTexture(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D, 0);
}

void Texture::Update3D(const void* pixels, int x, int y, int z, int w, int h, int d) {
    if (!id_ || !pixels || !gl::glTexSubImage3D) return;
    GlFormat gf = ToGl(format_);
    gl::glBindTexture(gl::GL_TEXTURE_3D, id_);
    gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
    gl::glTexSubImage3D(gl::GL_TEXTURE_3D, 0, x, y, z, w, h, d, gf.format, gf.type, pixels);
    gl::glBindTexture(gl::GL_TEXTURE_3D, 0);
}

void Texture::GenerateMipmaps() {
    if (!id_ || !gl::glGenerateMipmap) return;
    gl::glBindTexture(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D, id_);
    gl::glGenerateMipmap(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D);
    gl::glBindTexture(cube_ ? gl::GL_TEXTURE_CUBE_MAP : gl::GL_TEXTURE_2D, 0);
}

void Texture::SetFilter(TextureFilter f) {
    if (!id_) return;
    gl::GLenum target = cube_ ? gl::GL_TEXTURE_CUBE_MAP : (depth_ > 1 ? gl::GL_TEXTURE_3D : gl::GL_TEXTURE_2D);
    gl::glBindTexture(target, id_);
    gl::glTexParameteri(target, gl::GL_TEXTURE_MAG_FILTER, ToGlFilter(f));
    gl::glTexParameteri(target, gl::GL_TEXTURE_MIN_FILTER,
                        f == TextureFilter::Nearest ? gl::GL_NEAREST : gl::GL_LINEAR);
    gl::glBindTexture(target, 0);
}

void Texture::SetWrap(TextureWrap w) {
    if (!id_) return;
    gl::GLenum target = cube_ ? gl::GL_TEXTURE_CUBE_MAP : (depth_ > 1 ? gl::GL_TEXTURE_3D : gl::GL_TEXTURE_2D);
    gl::glBindTexture(target, id_);
    gl::glTexParameteri(target, gl::GL_TEXTURE_WRAP_S, ToGlWrap(w));
    gl::glTexParameteri(target, gl::GL_TEXTURE_WRAP_T, ToGlWrap(w));
    if (depth_ > 1) gl::glTexParameteri(target, gl::GL_TEXTURE_WRAP_R, ToGlWrap(w));
    gl::glBindTexture(target, 0);
}

void Texture::SetAnisotropy(f32 level) {
    if (!id_ || !gl::glTexParameterf) return;
    gl::GLenum target = gl::GL_TEXTURE_2D;
    gl::glBindTexture(target, id_);
    gl::glTexParameterf(target, gl::GL_TEXTURE_MAX_ANISOTROPY_EXT, level);
    gl::glBindTexture(target, 0);
}

bool Texture::EncodePng(const std::string& path, int w, int h, int channels, const void* pixels) {
    if (!pixels || w <= 0 || h <= 0) return false;
    // Как в WriteTextFile: убеждаемся, что каталог назначения существует, чтобы
    // утилиты могли писать в свежее дерево ассетов.
    if (!PathDir(path).empty()) CreateDirectories(PathDir(path));
    int stride = w * channels;
    int ok = stbi_write_png(path.c_str(), w, h, channels, pixels, stride);
    return ok != 0;
}

}  // namespace crossrender
