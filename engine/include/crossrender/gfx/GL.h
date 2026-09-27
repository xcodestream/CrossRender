//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: собственный кроссплатформенный загрузчик OpenGL: типы, константы и указатели функций.
//
#pragma once

#include "crossrender/core/Base.h"

#include <cstdint>

namespace crossrender {
namespace gl {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLchar = char;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::ptrdiff_t;
using GLint64 = std::int64_t;
using GLuint64 = std::uint64_t;

// ---------------------------------------------------------------------------
// Перечисления (значения, общие для GL 3.3 core и GLES 3.0)
// ---------------------------------------------------------------------------
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_NONE = 0;
constexpr GLenum GL_ZERO = 0;
constexpr GLenum GL_ONE = 1;

constexpr GLenum GL_POINTS = 0x0000;
constexpr GLenum GL_LINES = 0x0001;
constexpr GLenum GL_LINE_LOOP = 0x0002;
constexpr GLenum GL_LINE_STRIP = 0x0003;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
constexpr GLenum GL_TRIANGLE_FAN = 0x0006;

constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum GL_STENCIL_BUFFER_BIT = 0x00000400;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x00004000;

constexpr GLenum GL_NEVER = 0x0200;
constexpr GLenum GL_LESS = 0x0201;
constexpr GLenum GL_EQUAL = 0x0202;
constexpr GLenum GL_LEQUAL = 0x0203;
constexpr GLenum GL_GREATER = 0x0204;
constexpr GLenum GL_NOTEQUAL = 0x0205;
constexpr GLenum GL_GEQUAL = 0x0206;
constexpr GLenum GL_ALWAYS = 0x0207;

constexpr GLenum GL_SRC_COLOR = 0x0300;
constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_DST_ALPHA = 0x0304;
constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
constexpr GLenum GL_DST_COLOR = 0x0306;
constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
constexpr GLenum GL_CONSTANT_COLOR = 0x8001;
constexpr GLenum GL_ONE_MINUS_CONSTANT_COLOR = 0x8002;
constexpr GLenum GL_CONSTANT_ALPHA = 0x8003;
constexpr GLenum GL_ONE_MINUS_CONSTANT_ALPHA = 0x8004;
constexpr GLenum GL_SRC_ALPHA_SATURATE = 0x0308;

constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_FRONT_AND_BACK = 0x0408;

constexpr GLenum GL_INVALID_ENUM = 0x0500;
constexpr GLenum GL_INVALID_VALUE = 0x0501;
constexpr GLenum GL_INVALID_OPERATION = 0x0502;
constexpr GLenum GL_OUT_OF_MEMORY = 0x0505;
constexpr GLenum GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506;

constexpr GLenum GL_CW = 0x0900;
constexpr GLenum GL_CCW = 0x0901;

constexpr GLenum GL_LINE_WIDTH = 0x0B21;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_STENCIL_TEST = 0x0B90;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum GL_POLYGON_OFFSET_FACTOR = 0x8038;
constexpr GLenum GL_POLYGON_OFFSET_UNITS = 0x2A00;
constexpr GLenum GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
constexpr GLenum GL_SAMPLE_COVERAGE = 0x80A0;
constexpr GLenum GL_MULTISAMPLE = 0x809D;
constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;
constexpr GLenum GL_DEPTH_CLAMP = 0x864F;
constexpr GLenum GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F;

constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872;
constexpr GLenum GL_MAX_VERTEX_ATTRIBS = 0x8869;
constexpr GLenum GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB;
constexpr GLenum GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD;
constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
constexpr GLenum GL_MAX_DRAW_BUFFERS = 0x8824;
constexpr GLenum GL_MAX_COLOR_ATTACHMENTS = 0x8CDF;

constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_3D = 0x806F;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;
constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_X = 0x8516;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_Y = 0x8517;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_Y = 0x8518;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_Z = 0x8519;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851A;

constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_TEXTURE_COMPARE_FUNC = 0x884D;
constexpr GLenum GL_TEXTURE_SWIZZLE_R = 0x8E42;
constexpr GLenum GL_TEXTURE_SWIZZLE_G = 0x8E43;
constexpr GLenum GL_TEXTURE_SWIZZLE_B = 0x8E44;
constexpr GLenum GL_TEXTURE_SWIZZLE_A = 0x8E45;

constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;

constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;

constexpr GLenum GL_BYTE = 0x1400;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_SHORT = 0x1402;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_INT = 0x1404;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_HALF_FLOAT = 0x140B;
constexpr GLenum GL_UNSIGNED_SHORT_5_6_5 = 0x8363;
constexpr GLenum GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033;
constexpr GLenum GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034;
constexpr GLenum GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368;
constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;

constexpr GLenum GL_RED = 0x1903;
constexpr GLenum GL_RG = 0x8227;
constexpr GLenum GL_RGB = 0x1907;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_BGRA = 0x80E1;
constexpr GLenum GL_R8 = 0x8229;
constexpr GLenum GL_RG8 = 0x822B;
constexpr GLenum GL_RGB8 = 0x8051;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
constexpr GLenum GL_R16F = 0x822D;
constexpr GLenum GL_RG16F = 0x822F;
constexpr GLenum GL_RGB16F = 0x881B;
constexpr GLenum GL_RGBA16F = 0x881A;
constexpr GLenum GL_R32F = 0x822E;
constexpr GLenum GL_RG32F = 0x8230;
constexpr GLenum GL_RGB32F = 0x8815;
constexpr GLenum GL_RGBA32F = 0x8814;
constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum GL_DEPTH_COMPONENT16 = 0x81A5;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0;
constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
constexpr GLenum GL_UNSIGNED_INT_8_8_8_8 = 0x8035;
constexpr GLenum GL_RGB10_A2 = 0x8059;

constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
constexpr GLenum GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
constexpr GLenum GL_STREAM_DRAW = 0x88E0;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;

constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_VALIDATE_STATUS = 0x8B83;
constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ACTIVE_UNIFORMS = 0x8B86;
constexpr GLenum GL_ACTIVE_ATTRIBUTES = 0x8B89;

constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_COLOR_ATTACHMENT1 = 0x8CE1;
constexpr GLenum GL_COLOR_ATTACHMENT2 = 0x8CE2;
constexpr GLenum GL_COLOR_ATTACHMENT3 = 0x8CE3;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_STENCIL_ATTACHMENT = 0x8D20;
constexpr GLenum GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_SHADER_TYPE = 0x8B4F;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x8CD6;
constexpr GLenum GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7;
constexpr GLenum GL_FRAMEBUFFER_UNSUPPORTED = 0x8CDD;
constexpr GLenum GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE = 0x8D56;

constexpr GLenum GL_VENDOR = 0x1F00;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;

constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_TEXTURE1 = 0x84C1;
constexpr GLenum GL_TEXTURE2 = 0x84C2;
constexpr GLenum GL_TEXTURE3 = 0x84C3;
constexpr GLenum GL_TEXTURE4 = 0x84C4;
constexpr GLenum GL_TEXTURE5 = 0x84C5;
constexpr GLenum GL_TEXTURE6 = 0x84C6;
constexpr GLenum GL_TEXTURE7 = 0x84C7;
constexpr GLenum GL_TEXTURE8 = 0x84C8;
constexpr GLenum GL_TEXTURE9 = 0x84C9;
constexpr GLenum GL_TEXTURE10 = 0x84CA;
constexpr GLenum GL_TEXTURE11 = 0x84CB;
constexpr GLenum GL_TEXTURE12 = 0x84CC;
constexpr GLenum GL_TEXTURE13 = 0x84CD;
constexpr GLenum GL_TEXTURE14 = 0x84CE;
constexpr GLenum GL_TEXTURE15 = 0x84CF;

// Уравнения смешивания
constexpr GLenum GL_FUNC_ADD = 0x8006;
constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum GL_MIN = 0x8007;
constexpr GLenum GL_MAX = 0x8008;

// Синхронизация / запросы
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117;
constexpr GLenum GL_ALREADY_SIGNALED = 0x911A;
constexpr GLenum GL_TIMEOUT_EXPIRED = 0x911B;
constexpr GLenum GL_CONDITION_SATISFIED = 0x911C;
constexpr GLenum GL_WAIT_FAILED = 0x911D;
constexpr GLenum GL_SYNC_FLUSH_COMMANDS_BIT = 0x00000001;

constexpr GLenum GL_DEBUG_OUTPUT = 0x92E0;
constexpr GLenum GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;

// ---------------------------------------------------------------------------
// Точки входа
// ---------------------------------------------------------------------------
#define ENG_GL_FUNCS(X)                                                                        \
    X(void, glClear, (GLbitfield mask))                                                        \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                        \
    X(void, glClearDepthf, (GLfloat d))                                                        \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                              \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                               \
    X(void, glEnable, (GLenum cap))                                                            \
    X(void, glDisable, (GLenum cap))                                                           \
    X(GLboolean, glIsEnabled, (GLenum cap))                                                    \
    X(void, glBlendFunc, (GLenum sf, GLenum df))                                               \
    X(void, glBlendFuncSeparate, (GLenum a, GLenum b, GLenum c, GLenum d))                     \
    X(void, glBlendEquation, (GLenum m))                                                       \
    X(void, glBlendEquationSeparate, (GLenum rgb, GLenum a))                                   \
    X(void, glBlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                        \
    X(void, glDepthFunc, (GLenum f))                                                           \
    X(void, glDepthMask, (GLboolean m))                                                        \
    X(void, glDepthRangef, (GLfloat n, GLfloat f))                                             \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                 \
    X(void, glCullFace, (GLenum m))                                                            \
    X(void, glFrontFace, (GLenum m))                                                           \
    X(void, glLineWidth, (GLfloat w))                                                          \
    X(void, glPolygonOffset, (GLfloat f, GLfloat u))                                           \
    X(void, glPixelStorei, (GLenum p, GLint v))                                                \
    X(void, glGetIntegerv, (GLenum p, GLint* v))                                               \
    X(void, glGetBooleanv, (GLenum p, GLboolean* v))                                           \
    X(void, glReadBuffer, (GLenum m))                                                          \
    X(void, glDrawBuffer, (GLenum m))                                                          \
    X(void, glGetFloatv, (GLenum p, GLfloat* v))                                               \
    X(GLenum, glGetError, (void))                                                              \
    X(const GLubyte*, glGetString, (GLenum name))                                              \
    X(const GLubyte*, glGetStringi, (GLenum name, GLuint index))                               \
    X(void, glFinish, (void))                                                                  \
    X(void, glFlush, (void))                                                                   \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p)) \
    X(void, glGetTexImage, (GLenum t, GLint l, GLenum f, GLenum ty, void* p))                   \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                           \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* idx))        \
    X(void, glDrawArraysInstanced, (GLenum m, GLint f, GLsizei c, GLsizei n))                  \
    X(void, glDrawElementsInstanced, (GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum* bufs))                                    \
    X(void, glGenBuffers, (GLsizei n, GLuint* b))                                              \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* b))                                     \
    X(void, glBindBuffer, (GLenum t, GLuint b))                                                \
    X(void, glBufferData, (GLenum t, GLsizeiptr s, const void* d, GLenum u))                   \
    X(void, glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void* d))              \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* a))                                         \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* a))                                \
    X(void, glBindVertexArray, (GLuint a))                                                     \
    X(void, glEnableVertexAttribArray, (GLuint i))                                             \
    X(void, glDisableVertexAttribArray, (GLuint i))                                            \
    X(void, glVertexAttribPointer, (GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void* p)) \
    X(void, glVertexAttribIPointer, (GLuint i, GLint s, GLenum t, GLsizei st, const void* p))  \
    X(void, glVertexAttribDivisor, (GLuint i, GLuint d))                                       \
    X(void, glVertexAttrib4f, (GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w))          \
    X(GLuint, glCreateShader, (GLenum t))                                                      \
    X(void, glShaderSource, (GLuint s, GLsizei c, const GLchar* const* str, const GLint* l))   \
    X(void, glCompileShader, (GLuint s))                                                       \
    X(void, glGetShaderiv, (GLuint s, GLenum p, GLint* v))                                     \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei m, GLsizei* l, GLchar* log))                \
    X(void, glDeleteShader, (GLuint s))                                                        \
    X(GLuint, glCreateProgram, (void))                                                         \
    X(void, glAttachShader, (GLuint p, GLuint s))                                              \
    X(void, glDetachShader, (GLuint p, GLuint s))                                              \
    X(void, glLinkProgram, (GLuint p))                                                         \
    X(void, glGetProgramiv, (GLuint p, GLenum pn, GLint* v))                                   \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei m, GLsizei* l, GLchar* log))               \
    X(void, glUseProgram, (GLuint p))                                                          \
    X(void, glDeleteProgram, (GLuint p))                                                       \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar* n))                                \
    X(GLint, glGetAttribLocation, (GLuint p, const GLchar* n))                                 \
    X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar* n))                       \
    X(void, glUniform1i, (GLint l, GLint v))                                                   \
    X(void, glUniform2i, (GLint l, GLint a, GLint b))                                          \
    X(void, glUniform1f, (GLint l, GLfloat v))                                                 \
    X(void, glUniform2f, (GLint l, GLfloat a, GLfloat b))                                      \
    X(void, glUniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c))                           \
    X(void, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                \
    X(void, glUniform1fv, (GLint l, GLsizei n, const GLfloat* v))                              \
    X(void, glUniform2fv, (GLint l, GLsizei n, const GLfloat* v))                              \
    X(void, glUniform3fv, (GLint l, GLsizei n, const GLfloat* v))                              \
    X(void, glUniform4fv, (GLint l, GLsizei n, const GLfloat* v))                              \
    X(void, glUniform1iv, (GLint l, GLsizei n, const GLint* v))                                \
    X(void, glUniformMatrix3fv, (GLint l, GLsizei n, GLboolean t, const GLfloat* v))           \
    X(void, glUniformMatrix4fv, (GLint l, GLsizei n, GLboolean t, const GLfloat* v))           \
    X(void, glGenTextures, (GLsizei n, GLuint* t))                                             \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* t))                                    \
    X(void, glBindTexture, (GLenum t, GLuint id))                                              \
    X(void, glActiveTexture, (GLenum t))                                                       \
    X(void, glTexImage2D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* p)) \
    X(void, glTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void* p)) \
    X(void, glTexImage3D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f, GLenum ty, const void* p)) \
    X(void, glTexSubImage3D, (GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void* p)) \
    X(void, glTexParameteri, (GLenum t, GLenum p, GLint v))                                    \
    X(void, glTexParameterf, (GLenum t, GLenum p, GLfloat v))                                  \
    X(void, glTexParameterfv, (GLenum t, GLenum p, const GLfloat* v))                          \
    X(void, glGenerateMipmap, (GLenum t))                                                      \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* f))                                         \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f))                                \
    X(void, glBindFramebuffer, (GLenum t, GLuint f))                                           \
    X(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l))      \
    X(GLenum, glCheckFramebufferStatus, (GLenum t))                                            \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint* r))                                        \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* r))                               \
    X(void, glBindRenderbuffer, (GLenum t, GLuint r))                                          \
    X(void, glRenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h))                 \
    X(void, glRenderbufferStorageMultisample, (GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h)) \
    X(void, glFramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint rb))             \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield m, GLenum f)) \
    X(void, glGenQueries, (GLsizei n, GLuint* q))                                              \
    X(void, glDeleteQueries, (GLsizei n, const GLuint* q))                                     \
    X(void, glBeginQuery, (GLenum t, GLuint q))                                                \
    X(void, glEndQuery, (GLenum t))                                                            \
    X(void, glGetQueryObjectuiv, (GLuint q, GLenum p, GLuint* v))                              \
    X(void, glGetQueryObjectui64v, (GLuint q, GLenum p, GLuint64* v))                          \
    X(void, glInvalidateFramebuffer, (GLenum t, GLsizei n, const GLenum* a))

#define ENG_GL_DECL(ret, name, params)         \
    using PFN_##name = ret (*) params;         \
    inline PFN_##name name = nullptr;
ENG_GL_FUNCS(ENG_GL_DECL)
#undef ENG_GL_DECL

// Загружает все перечисленные выше точки входа. Возвращает false, если обязательная отсутствует.
bool LoadFunctions(void* (*getProcAddress)(const char*));
// Освобождает все указатели (при завершении работы / потере контекста).
void UnloadFunctions();
// Описание GPU в человекочитаемом виде.
struct GpuInfo {
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string glslVersion;
    bool isGLES = false;
    int major = 0;
    int minor = 0;
    int maxTextureSize = 0;
    int maxSamples = 0;
    int maxDrawBuffers = 0;
    int maxVertexAttribs = 0;
    bool hasAnisotropy = false;
    bool hasDebugOutput = false;
};
GpuInfo QueryGpuInfo();

// Помощник проверки ошибок, используемый ENG_GL_CHECK.
void CheckErrorImpl(const char* file, int line, const char* expr);
#if !defined(NDEBUG)
#  define ENG_GL_CHECK(expr) \
      do { (expr); ::crossrender::gl::CheckErrorImpl(__FILE__, __LINE__, #expr); } while (0)
#else
#  define ENG_GL_CHECK(expr) (expr)
#endif

const char* ErrorString(GLenum err);

}  // namespace gl
}  // namespace crossrender
