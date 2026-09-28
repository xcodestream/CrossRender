// GL-over-Metal: реализует подмножество точек входа OpenGL движка поверх
// Metal. Движок продолжает вызывать загрузчик gl::; при CR_METAL=ON эти
// указатели разрешаются здесь (см. MetalLoader.cpp), и каждый вызов транслируется.
// Используются только системные фреймворки Apple - сторонних слоёв трансляции нет.
//
// Объём v1: оконный 2D-конвейер - буферы, текстуры, программы с
// трансляцией GLSL->MSL во время выполнения, блендинг, состояние scissor/depth,
// render-таргеты RGBA8/16F, ReadPixels, BlitFramebuffer того же размера.
// Вызовы вне подмножества вырождаются в журналируемый no-op, чтобы движок продолжал работать.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>

#include "MetalTypes.h"
#include "MetalContext.h"
#include "ShaderTranslate.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"

#import <cstring>
#import <memory>
#import <unordered_map>
#import <utility>
#import <vector>

namespace crossrender::mtlgl {

using namespace crossrender::gl;

namespace {

// ---------------------------------------------------------------------------
// Объекты и глобальное состояние
// ---------------------------------------------------------------------------
struct GLBufferObj {
    id<MTLBuffer> buffer = nil;
    GLsizeiptr size = 0;
};

struct GLTextureObj {
    id<MTLTexture> texture = nil;
    MGPixelFormat format = MGPixelFormat::Invalid;
    GLint width = 0, height = 0;
    bool depth = false;
};

struct GLUniformInfo {
    std::string name;
    int stage = 0;          // 0 = vertex, 1 = fragment
    int binding = 0;        // индекс текстуры для сэмплеров
    int offset = 0;         // смещение в байтах внутри uniform-блока стадии
    int bytes = 0;
    bool sampler = false;
};

struct GLShaderObj {
    std::string source;
    bool isFragment = false;
    bool compiled = false;
    std::string error;
    id<MTLLibrary> library = nil;
    MSLTranslation translation;
};

struct GLProgramObj {
    std::shared_ptr<GLShaderObj> vs, fs;
    id<MTLLibrary> vertexLibrary = nil, fragmentLibrary = nil;
    std::vector<GLUniformInfo> uniforms;      // объединены; стадия выбирает сторону привязки
    std::vector<uint8_t> staging[2];          // [vertex, fragment]
    bool linked = false;
    std::string linkLog;
};

struct GLAttrib {
    bool enabled = false;
    GLuint buffer = 0;
    GLintptr offset = 0;
    GLsizei stride = 0;
    MGVertexFormat format = MGVertexFormat::Invalid;
};

struct GLVertexArrayObj {
    GLAttrib attribs[8];
    GLuint indexBuffer = 0;
};

struct GLFramebufferObj {
    id<MTLTexture> color, depth;
    MGPixelFormat colorFormat = MGPixelFormat::RGBA8Unorm;
    GLint width = 0, height = 0;
};

id<MTLDevice> gDevice = nil;
id<MTLCommandQueue> gQueue = nil;
CAMetalLayer* gLayer = nil;
id<CAMetalDrawable> gDrawable = nil;
id<MTLTexture> gDrawableDepth = nil;
id<MTLCommandBuffer> gFrameCommandBuffer = nil;
id<MTLRenderCommandEncoder> gEncoder = nil;
bool gPassHasDepth = false;

constexpr int kMaxUnits = 16;
GLuint gActiveUnit = 0;
GLuint gBoundTexture[kMaxUnits] = {};
GLint gSamplerUnit[kMaxUnits] = {};   // привязка сэмплера -> текстурный юнит

std::unordered_map<GLuint, GLBufferObj> gBuffers;
std::unordered_map<GLuint, GLTextureObj> gTextures;
std::unordered_map<GLuint, std::shared_ptr<GLShaderObj>> gShaders;
std::unordered_map<GLuint, std::shared_ptr<GLProgramObj>> gPrograms;
std::unordered_map<GLuint, GLVertexArrayObj> gVAOs;
std::unordered_map<GLuint, GLFramebufferObj> gFBOs;

GLuint gNextBuffer = 1, gNextTexture = 1, gNextShader = 1, gNextProgram = 1, gNextVAO = 1,
       gNextFBO = 1;

GLuint gArrayBuffer = 0, gElementBuffer = 0;
GLuint gCurrentProgram = 0, gCurrentVAO = 0, gDrawFBO = 0, gReadFBO = 0;
GLint gViewport[4] = {0, 0, 0, 0};  // 0 = вывести из размера drawable при отрисовке
GLint gScissor[4] = {0, 0, 0, 0};
GLfloat gClearColor[4] = {0, 0, 0, 0};
bool gBlendEnabled = false, gDepthTestEnabled = false, gScissorEnabled = false,
     gCullEnabled = false;
bool gDepthMask = true;
bool gColorMaskAll = true;
MGBlendFactor gBlendSrcColor = MGBlendFactor::One, gBlendDstColor = MGBlendFactor::Zero,
               gBlendSrcAlpha = MGBlendFactor::One, gBlendDstAlpha = MGBlendFactor::Zero;
MGBlendOperation gBlendOpColor = MGBlendOperation::Add, gBlendOpAlpha = MGBlendOperation::Add;
MGCompareFunction gDepthFunc = MGCompareFunction::Less;
MGCullMode gCullMode = MGCullMode::Back;
MGWinding gFrontFace = MGWinding::CounterClockwise;

id<MTLSamplerState> gDefaultSampler = nil;
bool gPendingClearColor = false, gPendingClearDepth = false;
GLfloat gPendingClearDepthValue = 1.0f;

int gUnimplementedCalls = 0;
const char* gLastUnimplemented = nullptr;

// ---------------------------------------------------------------------------
// Таблицы перечислений MG* -> значений Metal. Наши перечисления численно не
// совпадают с фреймворком, поэтому прямые касты молча дали бы неверные форматы.
// ---------------------------------------------------------------------------
MTLPixelFormat ToMetal(MGPixelFormat f) {
    switch (f) {
        case MGPixelFormat::R8Unorm: return MTLPixelFormatR8Unorm;
        case MGPixelFormat::RG8Unorm: return MTLPixelFormatRG8Unorm;
        case MGPixelFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
        case MGPixelFormat::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
        case MGPixelFormat::R16Float: return MTLPixelFormatR16Float;
        case MGPixelFormat::R32Float: return MTLPixelFormatR32Float;
        case MGPixelFormat::RGBA16Float: return MTLPixelFormatRGBA16Float;
        case MGPixelFormat::RGBA32Float: return MTLPixelFormatRGBA32Float;
        case MGPixelFormat::Depth32Float: return MTLPixelFormatDepth32Float;
        case MGPixelFormat::RGB8Unorm: return MTLPixelFormatRGBA8Unorm;  // нет RGB8 в Metal
        case MGPixelFormat::Invalid: break;
    }
    return MTLPixelFormatInvalid;
}

MTLVertexFormat ToMetal(MGVertexFormat f) {
    switch (f) {
        case MGVertexFormat::Float: return MTLVertexFormatFloat;
        case MGVertexFormat::Float2: return MTLVertexFormatFloat2;
        case MGVertexFormat::Float3: return MTLVertexFormatFloat3;
        case MGVertexFormat::Float4: return MTLVertexFormatFloat4;
        case MGVertexFormat::Half2: return MTLVertexFormatHalf2;
        case MGVertexFormat::Half4: return MTLVertexFormatHalf4;
        case MGVertexFormat::UChar2Norm: return MTLVertexFormatUChar2Normalized;
        case MGVertexFormat::UChar4Norm: return MTLVertexFormatUChar4Normalized;
        case MGVertexFormat::UInt: return MTLVertexFormatUInt;
        case MGVertexFormat::UInt4: return MTLVertexFormatUInt4;
        case MGVertexFormat::Invalid: break;
    }
    return MTLVertexFormatInvalid;
}

MTLPrimitiveType ToMetal(MGPrimitiveType p) {
    switch (p) {
        case MGPrimitiveType::Point: return MTLPrimitiveTypePoint;
        case MGPrimitiveType::Line: return MTLPrimitiveTypeLine;
        case MGPrimitiveType::LineStrip: return MTLPrimitiveTypeLineStrip;
        case MGPrimitiveType::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        case MGPrimitiveType::Triangle: break;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLIndexType ToMetal(MGIndexType t) {
    return t == MGIndexType::UInt16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

MTLBlendFactor ToMetal(MGBlendFactor f) {
    switch (f) {
        case MGBlendFactor::Zero: return MTLBlendFactorZero;
        case MGBlendFactor::One: return MTLBlendFactorOne;
        case MGBlendFactor::SourceColor: return MTLBlendFactorSourceColor;
        case MGBlendFactor::OneMinusSourceColor: return MTLBlendFactorOneMinusSourceColor;
        case MGBlendFactor::SourceAlpha: return MTLBlendFactorSourceAlpha;
        case MGBlendFactor::OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case MGBlendFactor::DestinationColor: return MTLBlendFactorDestinationColor;
        case MGBlendFactor::OneMinusDestinationColor: return MTLBlendFactorOneMinusDestinationColor;
        case MGBlendFactor::DestinationAlpha: return MTLBlendFactorDestinationAlpha;
        case MGBlendFactor::OneMinusDestinationAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        case MGBlendFactor::SourceAlphaSaturated: return MTLBlendFactorSourceAlphaSaturated;
        case MGBlendFactor::ConstantColor: return MTLBlendFactorBlendColor;
        case MGBlendFactor::OneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
    }
    return MTLBlendFactorOne;
}

MTLBlendOperation ToMetal(MGBlendOperation op) {
    switch (op) {
        case MGBlendOperation::Subtract: return MTLBlendOperationSubtract;
        case MGBlendOperation::ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case MGBlendOperation::Min: return MTLBlendOperationMin;
        case MGBlendOperation::Max: return MTLBlendOperationMax;
        case MGBlendOperation::Add: break;
    }
    return MTLBlendOperationAdd;
}

MTLCompareFunction ToMetal(MGCompareFunction c) {
    switch (c) {
        case MGCompareFunction::Never: return MTLCompareFunctionNever;
        case MGCompareFunction::Less: return MTLCompareFunctionLess;
        case MGCompareFunction::Equal: return MTLCompareFunctionEqual;
        case MGCompareFunction::LessEqual: return MTLCompareFunctionLessEqual;
        case MGCompareFunction::Greater: return MTLCompareFunctionGreater;
        case MGCompareFunction::NotEqual: return MTLCompareFunctionNotEqual;
        case MGCompareFunction::GreaterEqual: return MTLCompareFunctionGreaterEqual;
        case MGCompareFunction::Always: break;
    }
    return MTLCompareFunctionAlways;
}

MTLCullMode ToMetal(MGCullMode c) {
    switch (c) {
        case MGCullMode::None: return MTLCullModeNone;
        case MGCullMode::Front: return MTLCullModeFront;
        case MGCullMode::Back: break;
    }
    return MTLCullModeBack;
}

MTLWinding ToMetal(MGWinding w) {
    return w == MGWinding::Clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
}


}  // namespace


void NotImplemented(const char* name) {
    ++gUnimplementedCalls;
    if (gLastUnimplemented != name) {
        gLastUnimplemented = name;
        ENG_LOGW("metal", "%s: not implemented by the Metal layer (no-op)", name);
    }
}

std::shared_ptr<GLProgramObj> CurrentProgram() {
    auto it = gPrograms.find(gCurrentProgram);
    return it == gPrograms.end() ? nullptr : it->second;
}
GLVertexArrayObj* CurrentVAO() {
    auto it = gVAOs.find(gCurrentVAO);
    return it == gVAOs.end() ? nullptr : &it->second;
}

id<MTLTexture> DrawTargetTexture(MGPixelFormat* outFormat, GLint* outW, GLint* outH) {
    if (gDrawFBO == 0) {
        if (gDrawable == nil) return nil;
        *outFormat = MGPixelFormat::BGRA8Unorm;
        *outW = (GLint)gDrawable.texture.width;
        *outH = (GLint)gDrawable.texture.height;
        return gDrawable.texture;
    }
    auto it = gFBOs.find(gDrawFBO);
    if (it == gFBOs.end() || it->second.color == nil) return nil;
    *outFormat = it->second.colorFormat;
    *outW = it->second.width;
    *outH = it->second.height;
    return it->second.color;
}

id<MTLTexture> EnsureDefaultDepth(GLint w, GLint h) {
    if (gDrawableDepth == nil || gDrawableDepth.width != (NSUInteger)w ||
        gDrawableDepth.height != (NSUInteger)h) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:(NSUInteger)w
                                        height:(NSUInteger)h
                                     mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        gDrawableDepth = [gDevice newTextureWithDescriptor:d];
    }
    return gDrawableDepth;
}

void CloseEncoder() {
    if (gEncoder != nil) {
        [gEncoder endEncoding];
        gEncoder = nil;
    }
}

bool OpenEncoderIfNeeded() {
    if (gEncoder != nil) return true;
    if (!ContextActive()) return false;
    if (gFrameCommandBuffer == nil) gFrameCommandBuffer = [gQueue commandBuffer];

    MGPixelFormat colorFormat = MGPixelFormat::Invalid;
    GLint w = 0, h = 0;
    id<MTLTexture> color = DrawTargetTexture(&colorFormat, &w, &h);
    if (color == nil && gDrawFBO == 0) {
        gDrawable = [gLayer nextDrawable];
        if (gDrawable == nil) return false;
        color = gDrawable.texture;
        colorFormat = MGPixelFormat::BGRA8Unorm;
        w = (GLint)color.width;
        h = (GLint)color.height;
    }
    if (color == nil) return false;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLLoadAction load = gPendingClearColor ? MTLLoadActionClear : MTLLoadActionDontCare;
    if (gPendingClearColor) {
        pass.colorAttachments[0].clearColor = MTLClearColorMake(
            gClearColor[0], gClearColor[1], gClearColor[2], gClearColor[3]);
        gPendingClearColor = false;
    }
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = load;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLTexture> depth = gDrawFBO == 0 ? EnsureDefaultDepth(w, h)
                                         : gFBOs[gDrawFBO].depth;
    gPassHasDepth = depth != nil;
    if (depth != nil) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction =
            gPendingClearDepth ? MTLLoadActionClear : MTLLoadActionDontCare;
        pass.depthAttachment.clearDepth = gPendingClearDepthValue;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    gPendingClearDepth = false;

    gEncoder = [gFrameCommandBuffer renderCommandEncoderWithDescriptor:pass];
    return gEncoder != nil;
}

// ---------------------------------------------------------------------------
// Контекст / поверхность (MetalContext.h)
// ---------------------------------------------------------------------------
bool CreateContext() {
    if (gDevice != nil) return true;
    gDevice = MTLCreateSystemDefaultDevice();
    if (gDevice == nil) {
        ENG_LOGE("metal", "no Metal device available");
        return false;
    }
    gQueue = [gDevice newCommandQueue];
    gVAOs[0] = GLVertexArrayObj{};  // VAO-подобный объект по умолчанию для VAO 0
    ENG_LOGI("metal", "Metal layer active: %s", gDevice.name.UTF8String);
    return true;
}

void DestroyContext() {
    CloseEncoder();
    gFrameCommandBuffer = nil;
    gDrawable = nil;
    gDrawableDepth = nil;
    gLayer = nil;
    gBuffers.clear();
    gTextures.clear();
    gShaders.clear();
    gPrograms.clear();
    gVAOs.clear();
    gFBOs.clear();
    gQueue = nil;
    gDevice = nil;
}

bool ContextActive() { return gDevice != nil && gQueue != nil; }

void AttachLayer(void* cametalLayer, uint32_t width, uint32_t height) {
    if (!ContextActive()) CreateContext();
    gLayer = (__bridge CAMetalLayer*)cametalLayer;
    gLayer.device = gDevice;
    gLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    gLayer.framebufferOnly = NO;  // ReadPixels/блитам нужен доступ с CPU
    gLayer.drawableSize = CGSizeMake(width, height);
}

void SurfaceResized(uint32_t width, uint32_t height) {
    if (gLayer != nil) gLayer.drawableSize = CGSizeMake(width, height);
    // Хранимый вьюпорт больше не соответствует поверхности; сброс заставляет
    // SetupDraw вывести его из нового размера drawable.
    gViewport[2] = 0;
    gViewport[3] = 0;
    CloseEncoder();
}

void SurfaceSize(uint32_t* width, uint32_t* height) {
    if (width) *width = gLayer ? (uint32_t)gLayer.drawableSize.width : 0;
    if (height) *height = gLayer ? (uint32_t)gLayer.drawableSize.height : 0;
}

void Present() {
    CloseEncoder();
    if (gFrameCommandBuffer == nil) return;
    if (gDrawable != nil) {
        [gFrameCommandBuffer presentDrawable:gDrawable];
        gDrawable = nil;
    }
    [gFrameCommandBuffer commit];
    [gFrameCommandBuffer waitUntilCompleted];
    gFrameCommandBuffer = nil;
}

int UnimplementedCallCount() { return gUnimplementedCalls; }

// ---------------------------------------------------------------------------
// Точки входа GL. Сигнатуры повторяют PFN-типдефы из crossrender/gfx/GL.h, чтобы
// загрузчик мог привязать их напрямую.
// ---------------------------------------------------------------------------

namespace glimpl {

void MGL_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    gViewport[0] = x; gViewport[1] = y; gViewport[2] = w; gViewport[3] = h;
}

void MGL_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    gScissor[0] = x; gScissor[1] = y; gScissor[2] = w; gScissor[3] = h;
}

void MGL_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    gClearColor[0] = r; gClearColor[1] = g; gClearColor[2] = b; gClearColor[3] = a;
}

void MGL_glClear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) gPendingClearColor = true;
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) gPendingClearDepth = true;
}

void MGL_glEnable(GLenum cap) {
    switch (cap) {
        case GL_BLEND: gBlendEnabled = true; break;
        case GL_DEPTH_TEST: gDepthTestEnabled = true; break;
        case GL_SCISSOR_TEST: gScissorEnabled = true; break;
        case GL_CULL_FACE: gCullEnabled = true; break;
        default: break;
    }
}

void MGL_glDisable(GLenum cap) {
    switch (cap) {
        case GL_BLEND: gBlendEnabled = false; break;
        case GL_DEPTH_TEST: gDepthTestEnabled = false; break;
        case GL_SCISSOR_TEST: gScissorEnabled = false; break;
        case GL_CULL_FACE: gCullEnabled = false; break;
        default: break;
    }
}

void MGL_glBlendColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
void MGL_glBlendEquation(GLenum mode) { gBlendOpColor = gBlendOpAlpha = GLBlendOpToMTL(mode); }
void MGL_glBlendEquationSeparate(GLenum rgb, GLenum alpha) {
    gBlendOpColor = GLBlendOpToMTL(rgb);
    gBlendOpAlpha = GLBlendOpToMTL(alpha);
}
void MGL_glBlendFunc(GLenum src, GLenum dst) {
    gBlendSrcColor = gBlendSrcAlpha = GLBlendFactorToMTL(src);
    gBlendDstColor = gBlendDstAlpha = GLBlendFactorToMTL(dst);
}
void MGL_glBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da) {
    gBlendSrcColor = GLBlendFactorToMTL(srgb);
    gBlendDstColor = GLBlendFactorToMTL(drgb);
    gBlendSrcAlpha = GLBlendFactorToMTL(sa);
    gBlendDstAlpha = GLBlendFactorToMTL(da);
}
void MGL_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    gColorMaskAll = (r && g && b && a) != 0;
}
void MGL_glDepthFunc(GLenum func) { gDepthFunc = GLCompareToMTL(func); }
void MGL_glDepthMask(GLboolean flag) { gDepthMask = flag != 0; }
void MGL_glCullFace(GLenum mode) { gCullMode = mode == GL_FRONT ? MGCullMode::Front : MGCullMode::Back; }
void MGL_glFrontFace(GLenum mode) {
    gFrontFace = mode == GL_CW ? MGWinding::Clockwise : MGWinding::CounterClockwise;
}
void MGL_glPixelStorei(GLenum, GLint) {
    // Движок выставляет только GL_UNPACK_ALIGNMENT 1; Metal не требует
    // паддинга строк для используемых форматов.
}

void MGL_glGenBuffers(GLsizei n, GLuint* buffers) {
    for (GLsizei i = 0; i < n; ++i) buffers[i] = gNextBuffer++;
}
void MGL_glDeleteBuffers(GLsizei, const GLuint*) {}
void MGL_glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) gArrayBuffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) gElementBuffer = buffer;
}

GLuint& BoundBufferRef(GLenum target) {
    return target == GL_ELEMENT_ARRAY_BUFFER ? gElementBuffer : gArrayBuffer;
}

void MGL_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    (void)usage;
    const GLuint id = BoundBufferRef(target);
    if (id == 0) return;
    GLBufferObj& obj = gBuffers[id];
    obj.buffer = data != nullptr
                     ? [gDevice newBufferWithBytes:data length:size
                                           options:MTLResourceStorageModeShared]
                     : [gDevice newBufferWithLength:size options:MTLResourceStorageModeShared];
    obj.size = size;
}

void MGL_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    auto it = gBuffers.find(BoundBufferRef(target));
    if (it == gBuffers.end() || it->second.buffer == nil) return;
    memcpy((uint8_t*)it->second.buffer.contents + offset, data, (size_t)size);
}

void MGL_glGenTextures(GLsizei n, GLuint* textures) {
    for (GLsizei i = 0; i < n; ++i) textures[i] = gNextTexture++;
}
void MGL_glDeleteTextures(GLsizei, const GLuint*) {}
void MGL_glActiveTexture(GLenum texture) { gActiveUnit = texture - GL_TEXTURE0; }
void MGL_glBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) gBoundTexture[gActiveUnit] = texture;
}

NSUInteger BytesPerPixel(MGPixelFormat format) {
    switch (format) {
        case MGPixelFormat::R8Unorm: return 1;
        case MGPixelFormat::RG8Unorm: return 2;
        case MGPixelFormat::R16Float: return 2;
        case MGPixelFormat::R32Float: return 4;
        case MGPixelFormat::RGBA16Float: return 8;
        case MGPixelFormat::RGBA32Float: return 16;
        default: return 4;
    }
}

void MGL_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                      GLsizei height, GLint border, GLenum format, GLenum type,
                      const void* pixels) {
    (void)border;
    if (target != GL_TEXTURE_2D || level != 0) return;
    const GLuint id = gBoundTexture[gActiveUnit];
    if (id == 0) return;
    MGPixelFormat fmt = GLFormatToMTL((uint32_t)internalFormat, format, type);
    if (!MTLFormatSupported(fmt)) {
        ENG_LOGW("metal", "TexImage2D: format 0x%x unsupported, approximating RGBA8",
                 (unsigned)internalFormat);
        fmt = MGPixelFormat::RGBA8Unorm;
    }
    GLTextureObj& t = gTextures[id];
    MTLTextureDescriptor* d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:ToMetal(fmt)
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:NO];
    t.depth = fmt == MGPixelFormat::Depth32Float;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    t.texture = [gDevice newTextureWithDescriptor:d];
    t.format = fmt;
    t.width = width;
    t.height = height;
    if (pixels != nullptr && t.texture != nil) {
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [t.texture replaceRegion:region mipmapLevel:0 withBytes:pixels
                    bytesPerRow:(NSUInteger)width * BytesPerPixel(fmt)];
    }
}

void MGL_glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w,
                         GLsizei h, GLenum format, GLenum type, const void* pixels) {
    (void)target; (void)format; (void)type;
    if (level != 0) return;
    auto it = gTextures.find(gBoundTexture[gActiveUnit]);
    if (it == gTextures.end() || it->second.texture == nil) return;
    MTLRegion region = MTLRegionMake2D(xoff, yoff, w, h);
    [it->second.texture replaceRegion:region mipmapLevel:0 withBytes:pixels
                        bytesPerRow:(NSUInteger)w * BytesPerPixel(it->second.format)];
}

void MGL_glTexParameteri(GLenum, GLenum, GLint) {}
void MGL_glTexParameterf(GLenum, GLenum, GLfloat) {}
void MGL_glGenerateMipmap(GLenum) {}

// ---------------------------------------------------------------------------
// Шейдеры и программы
// ---------------------------------------------------------------------------
std::shared_ptr<GLShaderObj> ShaderObject(GLuint id) {
    auto it = gShaders.find(id);
    return it == gShaders.end() ? nullptr : it->second;
}

GLuint MGL_glCreateShader(GLenum type) {
    const GLuint id = gNextShader++;
    auto so = std::make_shared<GLShaderObj>();
    so->isFragment = type == GL_FRAGMENT_SHADER;
    gShaders[id] = so;
    return id;
}

GLuint MGL_glCreateProgram() {
    const GLuint id = gNextProgram++;
    gPrograms[id] = std::make_shared<GLProgramObj>();
    return id;
}

void MGL_glDeleteShader(GLuint) {}
void MGL_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                        const GLint* length) {
    auto so = ShaderObject(shader);
    if (!so) return;
    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
        src.append(string[i], length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]));
    }
    so->source = std::move(src);
    so->compiled = false;
}

void MGL_glCompileShader(GLuint shader) {
    auto so = ShaderObject(shader);
    if (!so || gDevice == nil) return;
    so->translation = TranslateGLSLToMSL(
        so->source, so->isFragment ? ShaderStage::Fragment : ShaderStage::Vertex);
    if (!so->translation.ok) {
        so->error = so->translation.error;
        ENG_LOGE("metal", "GLSL->MSL failed: %s", so->error.c_str());
        return;
    }
    NSError* err = nil;
    so->library = [gDevice newLibraryWithSource:
                            [NSString stringWithUTF8String:so->translation.msl.c_str()]
                                        options:nil
                                          error:&err];
    if (so->library == nil) {
        so->error = err.localizedDescription.UTF8String;
        ENG_LOGE("metal", "MSL compile failed: %s\n----\n%s", so->error.c_str(),
                 so->translation.msl.c_str());
        return;
    }
    so->compiled = true;
}

void MGL_glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    auto so = ShaderObject(shader);
    if (!so || !params) return;
    if (pname == GL_COMPILE_STATUS) *params = so->compiled ? GL_TRUE : GL_FALSE;
    else if (pname == GL_SHADER_TYPE)
        *params = so->isFragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
    else *params = 0;
}

void MGL_glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei* length, GLchar* log) {
    auto so = ShaderObject(shader);
    const std::string msg = so ? so->error : "";
    if (maxLength > 0 && log) {
        snprintf(log, (size_t)maxLength, "%s", msg.c_str());
        if (length) *length = (GLsizei)msg.size();
    }
}

void MGL_glAttachShader(GLuint program, GLuint shader) {
    auto poIt = gPrograms.find(program);
    auto so = ShaderObject(shader);
    if (poIt == gPrograms.end() || !so) return;
    if (so->isFragment) poIt->second->fs = so;
    else poIt->second->vs = so;
}

void MGL_glLinkProgram(GLuint program) {
    auto it = gPrograms.find(program);
    if (it == gPrograms.end()) return;
    std::shared_ptr<GLProgramObj> po = it->second;
    po->linked = false;
    po->linkLog.clear();
    if (!po->vs || !po->fs || !po->vs->compiled || !po->fs->compiled) {
        po->linkLog = "missing or uncompiled stages";
        return;
    }
    po->vertexLibrary = po->vs->library;
    po->fragmentLibrary = po->fs->library;
    po->uniforms.clear();
    for (const MSLUniform& u : po->vs->translation.uniforms) {
        po->uniforms.push_back({u.name, 0, u.binding, u.offset, u.bytes, u.sampler});
    }
    for (const MSLUniform& u : po->fs->translation.uniforms) {
        po->uniforms.push_back({u.name, 1, u.binding, u.offset, u.bytes, u.sampler});
    }
    for (int stage = 0; stage < 2; ++stage) {
        const int blockBytes = stage == 0 ? po->vs->translation.uniformsBlockBytes
                                          : po->fs->translation.uniformsBlockBytes;
        const size_t need = (size_t)((blockBytes + 255) / 256) * 256 + 256;
        if (need > po->staging[stage].size()) po->staging[stage].resize(need, 0);
    }
    po->linked = true;
}

void MGL_glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    auto it = gPrograms.find(program);
    if (!params) return;
    *params = (it != gPrograms.end() && pname == GL_LINK_STATUS && it->second->linked) ? GL_TRUE
                                                                                       : GL_FALSE;
}

void MGL_glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei* length, GLchar* log) {
    auto it = gPrograms.find(program);
    const std::string msg = it != gPrograms.end() ? it->second->linkLog : "";
    if (maxLength > 0 && log) {
        snprintf(log, (size_t)maxLength, "%s", msg.c_str());
        if (length) *length = (GLsizei)msg.size();
    }
}

void MGL_glUseProgram(GLuint program) { gCurrentProgram = program; }
void MGL_glDeleteProgram(GLuint program) { gPrograms.erase(program); }

GLint MGL_glGetUniformLocation(GLuint program, const GLchar* name) {
    auto it = gPrograms.find(program);
    if (it == gPrograms.end() || name == nullptr) return -1;
    // Кодировка: бит 0 = валидность, биты 1..15 = индекс uniform, бит 24 = стадия.
    for (int pass = 0; pass < 2; ++pass) {
        const int wantStage = pass == 0 ? 0 : 1;
        for (size_t i = 0; i < it->second->uniforms.size(); ++i) {
            const GLUniformInfo& u = it->second->uniforms[i];
            if (u.name == name && u.stage == wantStage) {
                return (GLint)((u.stage << 24) | (i << 1) | 1);
            }
        }
    }
    return -1;
}

GLint MGL_glGetAttribLocation(GLuint, const GLchar*) {
    // Атрибуты несут явный layout(location) в шейдерах движка.
    return -1;
}

bool DecodeUniform(GLint location, GLUniformInfo** info) {
    auto po = CurrentProgram();
    if (!po || location <= 0 || !(location & 1)) return false;
    const size_t index = (size_t)((location >> 1) & 0xFFFF);
    if (index >= po->uniforms.size()) return false;
    *info = &po->uniforms[index];
    return true;
}

uint8_t* UniformSlot(GLint location) {
    auto po = CurrentProgram();
    if (!po || location <= 0 || !(location & 1)) return nullptr;
    const size_t index = (size_t)((location >> 1) & 0xFFFF);
    const int stage = (location >> 24) & 1;
    const GLUniformInfo& u = po->uniforms[index];
    if (u.sampler || u.bytes == 0) return nullptr;
    const size_t offset = (size_t)u.offset;
    if (offset + (size_t)u.bytes > po->staging[stage].size()) return nullptr;
    return po->staging[stage].data() + offset;
}

// GL: одноимённый uniform, объявленный в обеих стадиях, - единый объект; запись
// через локацию одной стадии обязана обновить обе. Без зеркала фрагментная
// копия остаётся нулевой (так ломался батч глифов в Slug).
void WriteUniform(GLint location, const void* v, size_t bytes) {
    auto po = CurrentProgram();
    if (!po || location <= 0 || !(location & 1)) return;
    const size_t index = (size_t)((location >> 1) & 0xFFFF);
    const int stage = (location >> 24) & 1;
    if (index >= po->uniforms.size()) return;
    const GLUniformInfo& u = po->uniforms[index];
    if (u.sampler || u.bytes == 0 || bytes > (size_t)u.bytes) return;
    const size_t offset = (size_t)u.offset;
    if (offset + bytes > po->staging[stage].size()) return;
    memcpy(po->staging[stage].data() + offset, v, bytes);
    for (const GLUniformInfo& other : po->uniforms) {
        if (other.stage == stage || other.name != u.name || other.bytes != u.bytes) continue;
        if (offset + bytes <= po->staging[other.stage].size()) {
            memcpy(po->staging[other.stage].data() + offset, v, bytes);
        }
    }
}

void MGL_glUniform1i(GLint location, GLint v) {
    GLUniformInfo* info = nullptr;
    if (DecodeUniform(location, &info) && info->sampler) {
        gSamplerUnit[info->binding] = v;
        return;
    }
    WriteUniform(location, &v, sizeof(v));
}
void MGL_glUniform1f(GLint location, GLfloat v) {
    WriteUniform(location, &v, sizeof(v));
}
void MGL_glUniform2f(GLint location, GLfloat x, GLfloat y) {
    const GLfloat v[2] = {x, y};
    WriteUniform(location, v, sizeof(v));
}
void MGL_glUniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z) {
    const GLfloat v[3] = {x, y, z};
    WriteUniform(location, v, sizeof(v));
}
void MGL_glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    const GLfloat v[4] = {x, y, z, w};
    WriteUniform(location, v, sizeof(v));
}
void MGL_glUniform1fv(GLint location, GLsizei count, const GLfloat* v) {
    WriteUniform(location, v, sizeof(GLfloat) * count);
}
void MGL_glUniform2fv(GLint location, GLsizei count, const GLfloat* v) {
    WriteUniform(location, v, sizeof(GLfloat) * 2 * count);
}
void MGL_glUniform3fv(GLint location, GLsizei count, const GLfloat* v) {
    WriteUniform(location, v, sizeof(GLfloat) * 3 * count);
}
void MGL_glUniform4fv(GLint location, GLsizei count, const GLfloat* v) {
    WriteUniform(location, v, sizeof(GLfloat) * 4 * count);
}
void MGL_glUniform2iv(GLint location, GLsizei count, const GLint* v) {
    WriteUniform(location, v, sizeof(GLint) * 2 * count);
}
void MGL_glUniform3iv(GLint location, GLsizei count, const GLint* v) {
    WriteUniform(location, v, sizeof(GLint) * 3 * count);
}
void MGL_glUniform4iv(GLint location, GLsizei count, const GLint* v) {
    WriteUniform(location, v, sizeof(GLint) * 4 * count);
}
void MGL_glUniform1uiv(GLint location, GLsizei count, const GLuint* v) {
    WriteUniform(location, v, sizeof(GLuint) * count);
}
void MGL_glUniformMatrix2fv(GLint location, GLsizei count, GLboolean t, const GLfloat* v) {
    (void)t;
    WriteUniform(location, v, sizeof(GLfloat) * 4 * count);
}
void MGL_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean t, const GLfloat* v) {
    (void)t;
    WriteUniform(location, v, sizeof(GLfloat) * 9 * count);
}
void MGL_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean t, const GLfloat* v) {
    (void)t;  // движок передаёт GL_FALSE; float4x4 в MSL тоже column-major
    if (uint8_t* p = UniformSlot(location)) {
        memcpy(p, v, sizeof(GLfloat) * 16 * count);
        (void)p;
    }
}

// ---------------------------------------------------------------------------
// Вершинные массивы и отрисовка
// ---------------------------------------------------------------------------
void MGL_glGenVertexArrays(GLsizei n, GLuint* arrays) {
    if (gVAOs.find(0) == gVAOs.end()) gVAOs[0] = GLVertexArrayObj{};
    for (GLsizei i = 0; i < n; ++i) arrays[i] = gNextVAO++;
}
void MGL_glDeleteVertexArrays(GLsizei, const GLuint*) {}
void MGL_glBindVertexArray(GLuint array) {
    gVAOs[array];  // GL создаёт объект (в состоянии по умолчанию) при первой привязке.
    gCurrentVAO = array;
}
void MGL_glEnableVertexAttribArray(GLuint index) {
    if (auto vao = CurrentVAO(); vao && index < 8) vao->attribs[index].enabled = true;
}
void MGL_glDisableVertexAttribArray(GLuint index) {
    if (auto vao = CurrentVAO(); vao && index < 8) vao->attribs[index].enabled = false;
}
void MGL_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                               GLsizei stride, const void* pointer) {
    auto vao = CurrentVAO();
    if (!vao || index >= 8) return;
    GLAttrib& a = vao->attribs[index];
    a.buffer = gArrayBuffer;
    a.offset = (GLintptr)pointer;
    a.stride = stride;
    a.format = GLAttribFormatToMTL(type, size, normalized);
}

id<MTLRenderPipelineState> BuildPipeline(id<MTLTexture> colorTarget, MGPixelFormat colorFormat,
                                         bool targetHasDepth) {
    auto po = CurrentProgram();
    auto vao = CurrentVAO();
    if (!po || !vao || po->vertexLibrary == nil || po->fragmentLibrary == nil) return nil;

    MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [po->vertexLibrary newFunctionWithName:@"main0"];
    p.fragmentFunction = [po->fragmentLibrary newFunctionWithName:@"main0"];
    if (p.vertexFunction == nil || p.fragmentFunction == nil) return nil;
    p.colorAttachments[0].pixelFormat = ToMetal(colorFormat);
    p.colorAttachments[0].writeMask =
        gColorMaskAll ? (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue |
                         MTLColorWriteMaskAlpha)
                      : MTLColorWriteMaskNone;
    if (gBlendEnabled) {
        p.colorAttachments[0].blendingEnabled = YES;
        p.colorAttachments[0].rgbBlendOperation = ToMetal(gBlendOpColor);
        p.colorAttachments[0].alphaBlendOperation = ToMetal(gBlendOpAlpha);
        p.colorAttachments[0].sourceRGBBlendFactor = ToMetal(gBlendSrcColor);
        p.colorAttachments[0].sourceAlphaBlendFactor = ToMetal(gBlendSrcAlpha);
        p.colorAttachments[0].destinationRGBBlendFactor = ToMetal(gBlendDstColor);
        p.colorAttachments[0].destinationAlphaBlendFactor = ToMetal(gBlendDstAlpha);
    }
    (void)targetHasDepth;
    p.depthAttachmentPixelFormat =
        gPassHasDepth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    // Атрибуты с общими (буфер, stride) образуют одну группу раскладки -> один
    // привязанный вершинный буфер, что воспроизводит чередующиеся потоки GL.
    id<MTLBuffer> groupBuffer[8] = {nil};
    NSUInteger groupStride[8] = {0};
    int groupOfAttrib[8], groupCount = 0;
    for (int i = 0; i < 8; ++i) groupOfAttrib[i] = -1;
    for (int i = 0; i < 8; ++i) {
        const GLAttrib& a = vao->attribs[i];
        if (!a.enabled || a.format == MGVertexFormat::Invalid) continue;
        id<MTLBuffer> buf = gBuffers.count(a.buffer) ? gBuffers[a.buffer].buffer : nil;
        int group = -1;
        for (int g = 0; g < groupCount; ++g) {
            if (groupBuffer[g] == buf && groupStride[g] == (NSUInteger)a.stride) {
                group = g;
                break;
            }
        }
        if (group < 0 && groupCount < 8) {
            group = groupCount;
            groupBuffer[group] = buf;
            groupStride[group] = (NSUInteger)a.stride;
            ++groupCount;
        }
        if (group < 0) continue;
        groupOfAttrib[i] = group;
        vd.attributes[i].format = ToMetal(a.format);
        vd.attributes[i].offset = (NSUInteger)a.offset;
        vd.attributes[i].bufferIndex = group;
    }
    for (int g = 0; g < groupCount; ++g) {
        vd.layouts[g].stepFunction = MTLVertexStepFunctionPerVertex;
        vd.layouts[g].stride = groupStride[g] ? groupStride[g] : 256;
    }
    [p setVertexDescriptor:vd];


    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [gDevice newRenderPipelineStateWithDescriptor:p error:&err];
    if (pso == nil) {
        ENG_LOGE("metal", "pipeline creation failed: %s", err.localizedDescription.UTF8String);
    }
    return pso;
}

id<MTLDepthStencilState> BuildDepthStencil() {
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction =
        gDepthTestEnabled ? ToMetal(gDepthFunc) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = (gDepthTestEnabled && gDepthMask) ? YES : NO;
    return [gDevice newDepthStencilStateWithDescriptor:d];
}

// Общая прелюдия отрисовки: открывает проход, привязывает PSO/состояния,
// uniform-ы, текстуры и вершинные буферы. Возвращает false, когда отрисовка невозможна.
bool SetupDraw(id<MTLRenderPipelineState>* outPso) {
    if (!OpenEncoderIfNeeded()) return false;
    MGPixelFormat fmt = MGPixelFormat::Invalid;
    GLint w = 0, h = 0;
    id<MTLTexture> target = DrawTargetTexture(&fmt, &w, &h);
    if (target == nil) return false;
    auto po = CurrentProgram();
    auto vao = CurrentVAO();
    if (!po || !vao) return false;
    id<MTLRenderPipelineState> pso = BuildPipeline(target, fmt, gDepthTestEnabled);
    if (pso == nil) return false;
    [gEncoder setRenderPipelineState:pso];
    [gEncoder setDepthStencilState:BuildDepthStencil()];
    // Вьюпорт по умолчанию - вся поверхность: движок может и не вызывать
    // glViewport (2D-путь никогда не делал этого явно).
    MTLViewport metalViewport = {0.0, 0.0, (double)target.width, (double)target.height, 0.0, 1.0};
    if (gViewport[2] > 0 && gViewport[3] > 0) {
        metalViewport = (MTLViewport){(double)gViewport[0], (double)gViewport[1],
                                      (double)gViewport[2], (double)gViewport[3], 0.0, 1.0};
    }
    [gEncoder setViewport:metalViewport];
    if (gScissorEnabled && gScissor[2] > 0 && gScissor[3] > 0) {
        [gEncoder setScissorRect:(MTLScissorRect){(NSUInteger)gScissor[0], (NSUInteger)gScissor[1],
                                                  (NSUInteger)gScissor[2], (NSUInteger)gScissor[3]}];
    }
    if (gCullEnabled) [gEncoder setCullMode:ToMetal(gCullMode)];
    [gEncoder setFrontFacingWinding:ToMetal(gFrontFace)];

    // Один упакованный uniform-буфер на стадию, привязывается к [[buffer(16)]].
    for (int stage = 0; stage < 2; ++stage) {
        if (po->staging[stage].empty()) continue;
        id<MTLBuffer> buf = [gDevice newBufferWithBytes:po->staging[stage].data()
                                                 length:(NSUInteger)po->staging[stage].size()
                                                options:MTLResourceStorageModeShared];
        if (stage == 0) [gEncoder setVertexBuffer:buf offset:0 atIndex:16];
        else [gEncoder setFragmentBuffer:buf offset:0 atIndex:16];
    }
    // Линейный clamp-сэмплер для каждого объявленного uniform-сэмплера; Metal
    // требует привязку сэмплера, даже если шейдер на этом пути не сэмплирует.
    if (gDefaultSampler == nil && gDevice != nil) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        gDefaultSampler = [gDevice newSamplerStateWithDescriptor:sd];
    }
    for (const GLUniformInfo& u : po->uniforms) {
        if (!u.sampler) continue;
        const GLint unit = gSamplerUnit[u.binding];
        if (unit < 0 || unit >= kMaxUnits) continue;
        auto tit = gTextures.find(gBoundTexture[unit]);
        if (tit == gTextures.end() || tit->second.texture == nil) continue;
        if (u.stage == 0) {
            [gEncoder setVertexTexture:tit->second.texture atIndex:(NSUInteger)u.binding];
            [gEncoder setVertexSamplerState:gDefaultSampler atIndex:(NSUInteger)u.binding];
        } else {
            [gEncoder setFragmentTexture:tit->second.texture atIndex:(NSUInteger)u.binding];
            [gEncoder setFragmentSamplerState:gDefaultSampler atIndex:(NSUInteger)u.binding];
        }
    }

    // Группы вершинных буферов: то же правило группировки, что в BuildPipeline.
    std::vector<std::pair<GLuint, GLsizei>> groups;
    for (int i = 0; i < 8; ++i) {
        const GLAttrib& a = vao->attribs[i];
        if (!a.enabled || a.format == MGVertexFormat::Invalid) continue;
        bool seen = false;
        for (auto& g : groups) {
            if (g.first == a.buffer && g.second == a.stride) { seen = true; break; }
        }
        if (!seen) groups.push_back({a.buffer, a.stride});
    }
    int groupUsed = 0;
    for (auto& grp : groups) {
        auto bit = gBuffers.find(grp.first);
        if (bit == gBuffers.end() || bit->second.buffer == nil) continue;
        [gEncoder setVertexBuffer:bit->second.buffer offset:0 atIndex:(NSUInteger)groupUsed++];
    }

    *outPso = pso;
    return true;
}

void MGL_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    id<MTLRenderPipelineState> pso = nil;
    if (!SetupDraw(&pso)) return;
    [gEncoder drawPrimitives:ToMetal(GLPrimitiveToMTL(mode))
                 vertexStart:(NSUInteger)first
                 vertexCount:(NSUInteger)count];
}

void MGL_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    id<MTLRenderPipelineState> pso = nil;
    if (!SetupDraw(&pso)) return;
    auto vao = CurrentVAO();
    const GLuint ib = vao->indexBuffer ? vao->indexBuffer : gElementBuffer;
    auto it = gBuffers.find(ib);
    if (it == gBuffers.end() || it->second.buffer == nil) return;
    [gEncoder drawIndexedPrimitives:ToMetal(GLPrimitiveToMTL(mode))
                         indexCount:(NSUInteger)count
                          indexType:ToMetal(GLIndexTypeToMTL(type))
                        indexBuffer:it->second.buffer
                  indexBufferOffset:(NSUInteger)(uintptr_t)indices];
}

void MGL_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    if (instances <= 0) return;
    id<MTLRenderPipelineState> pso = nil;
    const bool setupOk = SetupDraw(&pso);
    if (!setupOk) return;
    [gEncoder drawPrimitives:ToMetal(GLPrimitiveToMTL(mode))
                 vertexStart:(NSUInteger)first
                 vertexCount:(NSUInteger)count
               instanceCount:(NSUInteger)instances];
}

void MGL_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                 GLsizei instances) {
    if (instances <= 0) return;
    id<MTLRenderPipelineState> pso = nil;
    if (!SetupDraw(&pso)) return;
    auto vao = CurrentVAO();
    const GLuint ib = vao->indexBuffer ? vao->indexBuffer : gElementBuffer;
    auto it = gBuffers.find(ib);
    if (it == gBuffers.end() || it->second.buffer == nil) return;
    [gEncoder drawIndexedPrimitives:ToMetal(GLPrimitiveToMTL(mode))
                         indexCount:(NSUInteger)count
                          indexType:ToMetal(GLIndexTypeToMTL(type))
                        indexBuffer:it->second.buffer
                  indexBufferOffset:(NSUInteger)(uintptr_t)indices
                      instanceCount:(NSUInteger)instances];
}

void MGL_glGenFramebuffers(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) ids[i] = gNextFBO++;
}
void MGL_glDeleteFramebuffers(GLsizei, const GLuint*) {}
void MGL_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (target == GL_READ_FRAMEBUFFER) gReadFBO = framebuffer;
    else gDrawFBO = framebuffer;
    if (target == GL_FRAMEBUFFER) gReadFBO = framebuffer;
    CloseEncoder();  // render pass должен переоткрыться на новой цели
}

void MGL_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum texTarget,
                                GLuint texture, GLint level) {
    (void)target; (void)texTarget; (void)level;
    auto it = gFBOs.find(gDrawFBO);
    if (it == gFBOs.end()) it = gFBOs.emplace(gDrawFBO, GLFramebufferObj{}).first;
    auto tit = gTextures.find(texture);
    if (tit == gTextures.end() || tit->second.texture == nil) return;
    if (attachment == GL_DEPTH_ATTACHMENT) {
        it->second.depth = tit->second.texture;
    } else {
        it->second.color = tit->second.texture;
        it->second.colorFormat = tit->second.format;
        it->second.width = tit->second.width;
        it->second.height = tit->second.height;
    }
}

GLenum MGL_glCheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }

void MGL_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                           GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    (void)filter;
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    CloseEncoder();
    if (!ContextActive()) return;
    if (gFrameCommandBuffer == nil) gFrameCommandBuffer = [gQueue commandBuffer];
    auto src = gFBOs.find(gReadFBO);
    auto dst = gFBOs.find(gDrawFBO);
    if (src == gFBOs.end() || dst == gFBOs.end()) return;
    if (src->second.color == nil || dst->second.color == nil) return;
    if (sx0 != 0 || sy0 != 0 || dx0 != 0 || dy0 != 0 || (sx1 - sx0) != (dx1 - dx0) ||
        (sy1 - sy0) != (dy1 - dy0)) {
        NotImplemented("glBlitFramebuffer (sub-rect)");
        return;
    }
    id<MTLBlitCommandEncoder> blit = [gFrameCommandBuffer blitCommandEncoder];
    [blit copyFromTexture:src->second.color
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)(sx1 - sx0), (NSUInteger)(sy1 - sy0), 1)
               toTexture:dst->second.color
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
}

void MGL_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                      void* data) {
    (void)format; (void)type;  // движок читает RGBA8
    MGPixelFormat fmt = MGPixelFormat::Invalid;
    GLint w = 0, h = 0;
    id<MTLTexture> tex = DrawTargetTexture(&fmt, &w, &h);
    if (tex == nil || (fmt != MGPixelFormat::BGRA8Unorm && fmt != MGPixelFormat::RGBA8Unorm)) {
        NotImplemented("glReadPixels (target format)");
        return;
    }
    CloseEncoder();
    if (!ContextActive()) return;
    if (gFrameCommandBuffer == nil) gFrameCommandBuffer = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [gFrameCommandBuffer blitCommandEncoder];
    id<MTLBuffer> staging = [gDevice newBufferWithLength:(NSUInteger)w * h * 4
                                                 options:MTLResourceStorageModeShared];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                toBuffer:staging
        destinationOffset:0
        destinationBytesPerRow:(NSUInteger)w * 4
        destinationBytesPerImage:0];
    [blit endEncoding];
    [gFrameCommandBuffer commit];
    [gFrameCommandBuffer waitUntilCompleted];
    gFrameCommandBuffer = nil;

    const bool bgra = fmt == MGPixelFormat::BGRA8Unorm;
    const uint8_t* src = (const uint8_t*)staging.contents;
    uint8_t* dst = (uint8_t*)data;
    for (GLint row = 0; row < height; ++row) {
        const GLint srcY = h - 1 - (y + row);  // GL читает снизу вверх
        const uint8_t* line = src + (size_t)srcY * w * 4 + (size_t)x * 4;
        for (GLint col = 0; col < width; ++col) {
            dst[(size_t)(row * width + col) * 4 + 0] = line[col * 4 + (bgra ? 2 : 0)];
            dst[(size_t)(row * width + col) * 4 + 1] = line[col * 4 + 1];
            dst[(size_t)(row * width + col) * 4 + 2] = line[col * 4 + (bgra ? 0 : 2)];
            dst[(size_t)(row * width + col) * 4 + 3] = line[col * 4 + 3];
        }
    }
}

void MGL_glGetIntegerv(GLenum pname, GLint* params) {
    switch (pname) {
        case GL_VIEWPORT: memcpy(params, gViewport, sizeof(gViewport)); break;
        case GL_MAX_TEXTURE_SIZE: *params = 8192; break;
        case GL_MAX_VERTEX_ATTRIBS: *params = 8; break;
        case GL_FRAMEBUFFER_BINDING: *params = (GLint)gDrawFBO; break;
        case GL_TEXTURE_BINDING_2D: *params = (GLint)gBoundTexture[gActiveUnit]; break;
        default: *params = 0; break;
    }
}

const GLubyte* MGL_glGetString(GLenum name) {
    static NSString* versionString = nil;
    if (name == GL_VERSION) {
        versionString = [NSString stringWithFormat:@"crossrender Metal compatibility layer (%@)",
                                                   gDevice ? gDevice.name : @"no device"];
        return (const GLubyte*)versionString.UTF8String;
    }
    if (name == GL_RENDERER) return (const GLubyte*)"Metal (translated)";
    return (const GLubyte*)"";
}

void MGL_glFinish() { Present(); }
void MGL_glFlush() {}

// Stencil-отсечение (пути Renderer2D) появится вместе с обновлением depth-stencil;
// вызовы состояния принимаются, чтобы конвейер продолжал работать.
void MGL_glStencilFunc(GLenum, GLint, GLuint) { NotImplemented("glStencilFunc"); }
void MGL_glStencilOp(GLenum, GLenum, GLenum) { NotImplemented("glStencilOp"); }
void MGL_glStencilMask(GLuint) {}
void MGL_glStencilFuncSeparate(GLenum, GLenum, GLint, GLuint) { NotImplemented("glStencilFuncSeparate"); }
void MGL_glStencilOpSeparate(GLenum, GLenum, GLenum, GLenum) { NotImplemented("glStencilOpSeparate"); }
void MGL_glStencilMaskSeparate(GLenum, GLuint) {}
void MGL_glDrawBuffers(GLsizei, const GLenum*) {}
void MGL_glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) { NotImplemented("glRenderbufferStorage"); }
void MGL_glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) { NotImplemented("glFramebufferRenderbuffer"); }

}  // namespace glimpl

void* ResolveGLProc(const char* name);
bool IsImplemented(const char* name);


// ---------------------------------------------------------------------------
// Перенаправление загрузчика (MetalLoader.h): реализованные имена привязываются
// к функциям MGL_* выше; остальные разрешаются в типизированные no-op-заглушки,
// чтобы движок продолжал работать с подмножеством, покрываемым этим слоем.
// ---------------------------------------------------------------------------
void* ResolveGLProc(const char* name);
bool IsImplemented(const char* name);

namespace {

int gStubCalls = 0;

#define ENG_METAL_STUB(ret, name, params) \
    ret Stub_##name params { ++gStubCalls; return (ret)0; }

ENG_GL_FUNCS(ENG_METAL_STUB)
#undef ENG_METAL_STUB

struct LoaderEntry {
    const char* name;
    void* fn;
};

const LoaderEntry kImplemented[] = {
    {"glViewport", (void*)&glimpl::MGL_glViewport},
    {"glScissor", (void*)&glimpl::MGL_glScissor},
    {"glClearColor", (void*)&glimpl::MGL_glClearColor},
    {"glClear", (void*)&glimpl::MGL_glClear},
    {"glEnable", (void*)&glimpl::MGL_glEnable},
    {"glDisable", (void*)&glimpl::MGL_glDisable},
    {"glBlendColor", (void*)&glimpl::MGL_glBlendColor},
    {"glBlendEquation", (void*)&glimpl::MGL_glBlendEquation},
    {"glBlendEquationSeparate", (void*)&glimpl::MGL_glBlendEquationSeparate},
    {"glBlendFunc", (void*)&glimpl::MGL_glBlendFunc},
    {"glBlendFuncSeparate", (void*)&glimpl::MGL_glBlendFuncSeparate},
    {"glColorMask", (void*)&glimpl::MGL_glColorMask},
    {"glDepthFunc", (void*)&glimpl::MGL_glDepthFunc},
    {"glDepthMask", (void*)&glimpl::MGL_glDepthMask},
    {"glCullFace", (void*)&glimpl::MGL_glCullFace},
    {"glFrontFace", (void*)&glimpl::MGL_glFrontFace},
    {"glPixelStorei", (void*)&glimpl::MGL_glPixelStorei},
    {"glGenBuffers", (void*)&glimpl::MGL_glGenBuffers},
    {"glDeleteBuffers", (void*)&glimpl::MGL_glDeleteBuffers},
    {"glBindBuffer", (void*)&glimpl::MGL_glBindBuffer},
    {"glBufferData", (void*)&glimpl::MGL_glBufferData},
    {"glBufferSubData", (void*)&glimpl::MGL_glBufferSubData},
    {"glGenTextures", (void*)&glimpl::MGL_glGenTextures},
    {"glDeleteTextures", (void*)&glimpl::MGL_glDeleteTextures},
    {"glActiveTexture", (void*)&glimpl::MGL_glActiveTexture},
    {"glBindTexture", (void*)&glimpl::MGL_glBindTexture},
    {"glTexImage2D", (void*)&glimpl::MGL_glTexImage2D},
    {"glTexSubImage2D", (void*)&glimpl::MGL_glTexSubImage2D},
    {"glTexParameteri", (void*)&glimpl::MGL_glTexParameteri},
    {"glTexParameterf", (void*)&glimpl::MGL_glTexParameterf},
    {"glGenerateMipmap", (void*)&glimpl::MGL_glGenerateMipmap},
    {"glCreateShader", (void*)&glimpl::MGL_glCreateShader},
    {"glDeleteShader", (void*)&glimpl::MGL_glDeleteShader},
    {"glShaderSource", (void*)&glimpl::MGL_glShaderSource},
    {"glCompileShader", (void*)&glimpl::MGL_glCompileShader},
    {"glGetShaderiv", (void*)&glimpl::MGL_glGetShaderiv},
    {"glGetShaderInfoLog", (void*)&glimpl::MGL_glGetShaderInfoLog},
    {"glCreateProgram", (void*)&glimpl::MGL_glCreateProgram},
    {"glDeleteProgram", (void*)&glimpl::MGL_glDeleteProgram},
    {"glAttachShader", (void*)&glimpl::MGL_glAttachShader},
    {"glLinkProgram", (void*)&glimpl::MGL_glLinkProgram},
    {"glGetProgramiv", (void*)&glimpl::MGL_glGetProgramiv},
    {"glGetProgramInfoLog", (void*)&glimpl::MGL_glGetProgramInfoLog},
    {"glUseProgram", (void*)&glimpl::MGL_glUseProgram},
    {"glGetUniformLocation", (void*)&glimpl::MGL_glGetUniformLocation},
    {"glGetAttribLocation", (void*)&glimpl::MGL_glGetAttribLocation},
    {"glUniform1i", (void*)&glimpl::MGL_glUniform1i},
    {"glUniform1f", (void*)&glimpl::MGL_glUniform1f},
    {"glUniform2f", (void*)&glimpl::MGL_glUniform2f},
    {"glUniform3f", (void*)&glimpl::MGL_glUniform3f},
    {"glUniform4f", (void*)&glimpl::MGL_glUniform4f},
    {"glUniform1fv", (void*)&glimpl::MGL_glUniform1fv},
    {"glUniform2fv", (void*)&glimpl::MGL_glUniform2fv},
    {"glUniform3fv", (void*)&glimpl::MGL_glUniform3fv},
    {"glUniform4fv", (void*)&glimpl::MGL_glUniform4fv},
    {"glUniform2iv", (void*)&glimpl::MGL_glUniform2iv},
    {"glUniform3iv", (void*)&glimpl::MGL_glUniform3iv},
    {"glUniform4iv", (void*)&glimpl::MGL_glUniform4iv},
    {"glUniform1uiv", (void*)&glimpl::MGL_glUniform1uiv},
    {"glUniformMatrix2fv", (void*)&glimpl::MGL_glUniformMatrix2fv},
    {"glUniformMatrix3fv", (void*)&glimpl::MGL_glUniformMatrix3fv},
    {"glUniformMatrix4fv", (void*)&glimpl::MGL_glUniformMatrix4fv},
    {"glGenVertexArrays", (void*)&glimpl::MGL_glGenVertexArrays},
    {"glDeleteVertexArrays", (void*)&glimpl::MGL_glDeleteVertexArrays},
    {"glBindVertexArray", (void*)&glimpl::MGL_glBindVertexArray},
    {"glEnableVertexAttribArray", (void*)&glimpl::MGL_glEnableVertexAttribArray},
    {"glDisableVertexAttribArray", (void*)&glimpl::MGL_glDisableVertexAttribArray},
    {"glVertexAttribPointer", (void*)&glimpl::MGL_glVertexAttribPointer},
    {"glDrawArrays", (void*)&glimpl::MGL_glDrawArrays},
    {"glDrawElements", (void*)&glimpl::MGL_glDrawElements},
    {"glDrawArraysInstanced", (void*)&glimpl::MGL_glDrawArraysInstanced},
    {"glDrawElementsInstanced", (void*)&glimpl::MGL_glDrawElementsInstanced},
    {"glGenFramebuffers", (void*)&glimpl::MGL_glGenFramebuffers},
    {"glDeleteFramebuffers", (void*)&glimpl::MGL_glDeleteFramebuffers},
    {"glBindFramebuffer", (void*)&glimpl::MGL_glBindFramebuffer},
    {"glFramebufferTexture2D", (void*)&glimpl::MGL_glFramebufferTexture2D},
    {"glCheckFramebufferStatus", (void*)&glimpl::MGL_glCheckFramebufferStatus},
    {"glBlitFramebuffer", (void*)&glimpl::MGL_glBlitFramebuffer},
    {"glReadPixels", (void*)&glimpl::MGL_glReadPixels},
    {"glGetIntegerv", (void*)&glimpl::MGL_glGetIntegerv},
    {"glGetString", (void*)&glimpl::MGL_glGetString},
    {"glFinish", (void*)&glimpl::MGL_glFinish},
    {"glFlush", (void*)&glimpl::MGL_glFlush},
    {"glStencilFunc", (void*)&glimpl::MGL_glStencilFunc},
    {"glStencilOp", (void*)&glimpl::MGL_glStencilOp},
    {"glStencilMask", (void*)&glimpl::MGL_glStencilMask},
    {"glStencilFuncSeparate", (void*)&glimpl::MGL_glStencilFuncSeparate},
    {"glStencilOpSeparate", (void*)&glimpl::MGL_glStencilOpSeparate},
    {"glStencilMaskSeparate", (void*)&glimpl::MGL_glStencilMaskSeparate},
    {"glDrawBuffers", (void*)&glimpl::MGL_glDrawBuffers},
    {"glRenderbufferStorage", (void*)&glimpl::MGL_glRenderbufferStorage},
    {"glFramebufferRenderbuffer", (void*)&glimpl::MGL_glFramebufferRenderbuffer},
};

}  // namespace

void* ResolveGLProc(const char* name) {
    for (const LoaderEntry& e : kImplemented) {
        if (std::strcmp(e.name, name) == 0) return e.fn;
    }
#define ENG_METAL_STUB_LOOKUP(ret, name2, params)                     \
    if (std::strcmp(name_str, #name2) == 0) return (void*)&Stub_##name2;
    const char* name_str = name;
    ENG_GL_FUNCS(ENG_METAL_STUB_LOOKUP)
#undef ENG_METAL_STUB_LOOKUP
    return nullptr;
}

bool IsImplemented(const char* name) {
    for (const LoaderEntry& e : kImplemented) {
        if (std::strcmp(e.name, name) == 0) return true;
    }
    return false;
}

}  // namespace crossrender::mtlgl
