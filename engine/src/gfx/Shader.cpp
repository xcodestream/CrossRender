#include "crossrender/gfx/Shader.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/gfx/Texture.h"

#include <cstring>

namespace crossrender {
namespace {

bool StartsWithVersion(const char* src) {
    if (!src) return false;
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') ++src;
    return std::strncmp(src, "#version", 8) == 0;
}

std::string BuildSource(const char* src, const std::vector<std::string>& defines) {
    std::string out;
    if (!StartsWithVersion(src)) {
        out += builtin::Preamble();
    }
    for (const auto& d : defines) {
        out += "#define ";
        out += d;
        out += "\n";
    }
    out += src;
    return out;
}

unsigned int CompileStage(gl::GLenum type, const std::string& source, const std::string& name,
                          std::string* logOut) {
    unsigned int shader = gl::glCreateShader(type);
    const char* ptr = source.c_str();
    gl::GLint len = static_cast<gl::GLint>(source.size());
    gl::glShaderSource(shader, 1, &ptr, &len);
    gl::glCompileShader(shader);
    gl::GLint ok = 0;
    gl::glGetShaderiv(shader, gl::GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint logLen = 0;
        gl::glGetShaderiv(shader, gl::GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<usize>(logLen > 1 ? logLen : 1), '\0');
        gl::glGetShaderInfoLog(shader, logLen, nullptr, log.data());
        *logOut += std::string(type == gl::GL_VERTEX_SHADER ? "[vertex] " : "[fragment] ") + log + "\n";
        ENG_LOGE("shader", "%s: compilation failed\n%s", name.c_str(), log.c_str());
        gl::glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

Shader::~Shader() { Destroy(); }

Shader::Shader(Shader&& o) noexcept { *this = std::move(o); }

Shader& Shader::operator=(Shader&& o) noexcept {
    if (this == &o) return *this;
    Destroy();
    program_ = o.program_;
    name_ = std::move(o.name_);
    log_ = std::move(o.log_);
    defines_ = std::move(o.defines_);
    uniforms_ = std::move(o.uniforms_);
    o.program_ = 0;
    return *this;
}

bool Shader::Build(const char* vertexSrc, const char* fragmentSrc, const std::string& name) {
    Destroy();
    name_ = name;
    log_.clear();
    if (!gl::glCreateShader) {
        ENG_LOGE("shader", "%s: no GL context", name.c_str());
        return false;
    }
    std::string vs = BuildSource(vertexSrc, defines_);
    std::string fs = BuildSource(fragmentSrc, defines_);

    unsigned int v = CompileStage(gl::GL_VERTEX_SHADER, vs, name, &log_);
    if (!v) return false;
    unsigned int f = CompileStage(gl::GL_FRAGMENT_SHADER, fs, name, &log_);
    if (!f) {
        gl::glDeleteShader(v);
        return false;
    }
    program_ = gl::glCreateProgram();
    gl::glAttachShader(program_, v);
    gl::glAttachShader(program_, f);
    gl::glLinkProgram(program_);
    gl::GLint ok = 0;
    gl::glGetProgramiv(program_, gl::GL_LINK_STATUS, &ok);
    if (!ok) {
        gl::GLint logLen = 0;
        gl::glGetProgramiv(program_, gl::GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<usize>(logLen > 1 ? logLen : 1), '\0');
        gl::glGetProgramInfoLog(program_, logLen, nullptr, log.data());
        log_ += log;
        ENG_LOGE("shader", "%s: link failed\n%s", name.c_str(), log.c_str());
        gl::glDeleteProgram(program_);
        program_ = 0;
    }
    gl::glDetachShader(program_, v);
    gl::glDetachShader(program_, f);
    gl::glDeleteShader(v);
    gl::glDeleteShader(f);
    return program_ != 0;
}

bool Shader::BuildFile(const std::string& vertPath, const std::string& fragPath) {
    std::string vs = ReadTextFile(vertPath);
    std::string fs = ReadTextFile(fragPath);
    if (vs.empty() || fs.empty()) {
        ENG_LOGE("shader", "cannot read %s / %s", vertPath.c_str(), fragPath.c_str());
        return false;
    }
    return Build(vs.c_str(), fs.c_str(), PathBase(vertPath));
}

bool Shader::Load(const std::string& basePath) {
    return BuildFile(basePath + ".vert", basePath + ".frag");
}

void Shader::Destroy() {
    if (program_ && gl::glDeleteProgram) gl::glDeleteProgram(program_);
    program_ = 0;
    uniforms_.clear();
}

void Shader::Bind() const {
    if (program_) gl::glUseProgram(program_);
}

void Shader::Unbind() { gl::glUseProgram(0); }

int Shader::UniformLocation(const char* name) {
    if (!program_ || !name) return -1;
    auto it = uniforms_.find(name);
    if (it != uniforms_.end()) return it->second;
    int loc = gl::glGetUniformLocation(program_, name);
    uniforms_.emplace(name, loc);
    return loc;
}

void Shader::Set(const char* name, i32 v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform1i(l, v);
}
void Shader::Set(const char* name, f32 v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform1f(l, v);
}
void Shader::Set(const char* name, const Vec2& v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform2f(l, v.x, v.y);
}
void Shader::Set(const char* name, const Vec3& v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform3f(l, v.x, v.y, v.z);
}
void Shader::Set(const char* name, const Vec4& v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform4f(l, v.x, v.y, v.z, v.w);
}
void Shader::Set(const char* name, const Color& v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform4f(l, v.r, v.g, v.b, v.a);
}
void Shader::Set(const char* name, const Mat4& v) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniformMatrix4fv(l, 1, 0, v.data());
}
void Shader::Set(const char* name, const float* m3) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniformMatrix3fv(l, 1, 0, m3);
}
void Shader::Set(const char* name, const std::vector<Vec3>& v) {
    if (v.empty()) return;
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform3fv(l, static_cast<gl::GLsizei>(v.size()),
                                 reinterpret_cast<const float*>(v.data()));
}
void Shader::Set(const char* name, const std::vector<Vec4>& v) {
    if (v.empty()) return;
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform4fv(l, static_cast<gl::GLsizei>(v.size()),
                                 reinterpret_cast<const float*>(v.data()));
}
void Shader::Set(const char* name, const std::vector<Mat4>& v) {
    if (v.empty()) return;
    int l = UniformLocation(name);
    if (l >= 0)
        gl::glUniformMatrix4fv(l, static_cast<gl::GLsizei>(v.size()), 0,
                               reinterpret_cast<const float*>(v.data()));
}

void Shader::SetTexture(const char* name, const Texture& tex, int slot) {
    int l = UniformLocation(name);
    if (l < 0) return;
    gl::glActiveTexture(static_cast<gl::GLenum>(gl::GL_TEXTURE0 + slot));
    gl::glBindTexture(gl::GL_TEXTURE_2D, tex.Id());
    gl::glUniform1i(l, slot);
}

void Shader::SetIntArray(const char* name, const i32* values, int count) {
    int l = UniformLocation(name);
    if (l >= 0) gl::glUniform1iv(l, count, values);
}

}  // namespace crossrender
