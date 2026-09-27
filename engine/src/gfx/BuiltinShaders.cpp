// Встроенные исходники GLSL-шейдеров, написанные против общего шима: одна и та
// же строка компилируется как GLSL 330 core (macOS/Windows/Linux) и GLSL ES 3.00
// (iOS/Android/WASM). `Shader::Build` добавляет в начало builtin::Preamble().
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Shader.h"

namespace crossrender {
namespace builtin {

const char* Preamble() {
#if ENG_GLES
    return "#version 300 es\n"
           "precision highp float;\n"
           "precision highp int;\n"
           "precision mediump sampler3D;\n"
           "#define ENG_GLES 1\n";
#else
    return "#version 330 core\n"
           "#define ENG_GLES 0\n";
#endif
}

// ---------------------------------------------------------------------------
// Полноэкранный blit / композитинг
// ---------------------------------------------------------------------------
const char* kBlitVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uTransform;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uTransform * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kBlitFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec4 uTint;
out vec4 fragColor;
void main() {
    fragColor = texture(uTexture, vUV) * uTint;
}
)GLSL";

// ---------------------------------------------------------------------------
// Батч-шейдер 2D-спрайтов и путей (используется Renderer2D)
//   uType: 0 = сплошной цвет, 1 = линейный градиент, 2 = радиальный градиент,
//          3 = изображение, 4 = box-градиент
// ---------------------------------------------------------------------------
const char* kSpriteVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uViewProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kSpriteFrag = R"GLSL(
in vec2 vUV;
in vec4 vColor;
uniform int uType;
uniform sampler2D uTexture;
uniform vec4 uInnerColor;
uniform vec4 uOuterColor;
uniform vec2 uGradA;
uniform vec2 uGradB;
uniform float uRadiusA;
uniform float uRadiusB;
uniform float uFeather;
uniform vec2 uBoxSize;
out vec4 fragColor;

float boxDistance(vec2 p, vec2 halfSize, float radius) {
    vec2 q = abs(p) - halfSize + vec2(radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    vec4 c;
    if (uType == 0) {
        c = vColor;
    } else if (uType == 1) {
        vec2 ab = uGradB - uGradA;
        float len2 = max(dot(ab, ab), 1e-6);
        float t = clamp(dot(vUV - uGradA, ab) / len2, 0.0, 1.0);
        c = mix(uInnerColor, uOuterColor, t);
        c.a *= vColor.a;
    } else if (uType == 2) {
        float d = distance(vUV, uGradA);
        float t = clamp((d - uRadiusA) / max(uRadiusB - uRadiusA, 1e-6), 0.0, 1.0);
        c = mix(uInnerColor, uOuterColor, t);
        c.a *= vColor.a;
    } else if (uType == 3) {
        c = texture(uTexture, vUV);
        c *= vColor;
    } else {
        float d = boxDistance(vUV - uGradA, uBoxSize * 0.5, uRadiusA);
        float t = clamp((d - (uRadiusA - uFeather)) / max(uFeather, 1e-6), 0.0, 1.0);
        c = mix(uInnerColor, uOuterColor, t);
        c.a *= vColor.a;
    }
    c.rgb *= c.a;  // premultiplied output
    fragColor = c;
}
)GLSL";

// ---------------------------------------------------------------------------
// SDF-шейдер для текста
// ---------------------------------------------------------------------------
const char* kSdfTextVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat4 uViewProj;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kSdfTextFrag = R"GLSL(
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTexture;
uniform float uSdfSpread;     // distance range encoded in the atlas (pixels)
uniform float uSdfSize;       // atlas pixel size of the glyph
uniform float uPixelRange;    // screen pixels per atlas pixel
uniform float uOutlineWidth;  // 0 = no outline
uniform vec4 uOutlineColor;
uniform float uSoftness;
out vec4 fragColor;
void main() {
    float dist = texture(uTexture, vUV).a;
    float screenPxRange = max(uSdfSpread * uPixelRange, 1.0);
    // The atlas stores 0.5 exactly at the glyph edge, *below* 0.5 inside the
    // shape and above 0.5 outside it, covering +/- uSdfSpread atlas pixels.
    // The hint in the rasteriser is `128 - distance * scale` when inside, so a
    // smaller stored value means further in. Convert that to a signed
    // screen-space distance that is positive inside the glyph:
    //   sd = (0.5 - dist) * 2 * spread * pixelsPerAtlasPixel
    // (the 2 undoes the 1/(2*spread) encoding, and the sign flip is what makes
    // the interior opaque and the padding transparent).
    float sd = (0.5 - dist) * 2.0 * screenPxRange;
    float aa = max(uSoftness, 1.0);
    float alpha = clamp(sd / aa + 0.5, 0.0, 1.0);
    vec4 c = vColor;
    c.a *= alpha;
    if (uOutlineWidth > 0.0) {
        float oa = clamp((sd + uOutlineWidth) / aa + 0.5, 0.0, 1.0);
        c = mix(vec4(uOutlineColor.rgb, uOutlineColor.a * oa), c, alpha);
        c.a = max(c.a, uOutlineColor.a * oa);
    }
    if (c.a <= 0.001) discard;
    c.rgb *= c.a;
    fragColor = c;
}
)GLSL";

// ---------------------------------------------------------------------------
// Прямой (forward) 3D-шейдер: PBR-lite, до 8 источников света и 3 каскада теней.
// ---------------------------------------------------------------------------
const char* kForwardVert = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in vec4 aColor;
layout(location = 5) in vec2 aUV2;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec2 vUV2;
out vec4 vTangent;
out vec4 vColor;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uNormalMatrix) * aNormal);
    vTangent = vec4(normalize(mat3(uModel) * aTangent.xyz), aTangent.w);
    vUV = aUV;
    vUV2 = aUV2;
    vColor = aColor;
    gl_Position = uViewProj * wp;
}
)GLSL";

const char* kForwardFrag = R"GLSL(
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec2 vUV2;
in vec4 vTangent;
in vec4 vColor;

#define MAX_LIGHTS 8

uniform vec3 uCameraPos;
uniform vec4 uBaseColor;
uniform vec3 uEmissive;
uniform float uMetallic;
uniform float uRoughness;
uniform float uNormalStrength;
uniform float uOcclusionStrength;
uniform float uAlphaCutoff;
uniform int uAlphaMode;       // 0 opaque, 1 mask, 2 blend
uniform int uUnlit;
uniform int uVertexColors;
uniform vec2 uUvScale;
uniform vec2 uUvOffset;

uniform sampler2D uBaseColorTex;
uniform sampler2D uNormalTex;
uniform sampler2D uMetallicRoughnessTex;
uniform sampler2D uEmissiveTex;
uniform sampler2D uOcclusionTex;
uniform int uHasBaseColorTex;
uniform int uHasNormalTex;
uniform int uHasMetallicRoughnessTex;
uniform int uHasEmissiveTex;
uniform int uHasOcclusionTex;

uniform int uLightCount;
uniform int uLightType[MAX_LIGHTS];       // 0 dir, 1 point, 2 spot, 3 area
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightIntensity[MAX_LIGHTS];
uniform float uLightRange[MAX_LIGHTS];
uniform vec2 uLightCone[MAX_LIGHTS];      // cos(inner), cos(outer)
uniform vec2 uLightArea[MAX_LIGHTS];

uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform float uAmbientIntensity;

uniform int uFogMode;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogStart;
uniform float uFogEnd;

uniform int uShadowCascadeCount;
uniform mat4 uShadowMatrices[3];
uniform sampler2D uShadowMap0;
uniform sampler2D uShadowMap1;
uniform sampler2D uShadowMap2;
uniform float uShadowBias;
uniform float uShadowNormalBias;
uniform float uShadowPcfRadius;
uniform float uCascadeSplit[3];
uniform int uShadowEnabled;

out vec4 fragColor;

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}

float geometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), rough) *
           geometrySchlickGGX(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float sampleShadow(sampler2D map, int index, vec3 N) {
    vec4 lp = uShadowMatrices[index] * vec4(vWorldPos + N * uShadowNormalBias, 1.0);
    vec3 proj = lp.xyz / lp.w * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    float bias = uShadowBias;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float d = texture(map, proj.xy + vec2(float(x), float(y)) * texel * uShadowPcfRadius).r;
            sum += proj.z - bias > d ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

float shadowFactor(vec3 N) {
    if (uShadowEnabled == 0 || uShadowCascadeCount == 0) return 1.0;
    float dist = length(vWorldPos - uCameraPos);
    if (dist < uCascadeSplit[0] || uShadowCascadeCount == 1) {
        return sampleShadow(uShadowMap0, 0, N);
    } else if (uShadowCascadeCount == 2 || dist < uCascadeSplit[1]) {
        return sampleShadow(uShadowMap1, 1, N);
    }
    return sampleShadow(uShadowMap2, 2, N);
}

void main() {
    vec2 uv = vUV * uUvScale + uUvOffset;
    vec4 base = uBaseColor;
    if (uHasBaseColorTex == 1) base *= texture(uBaseColorTex, uv);
    if (uVertexColors == 1) base *= vColor;
    if (uAlphaMode == 1 && base.a < uAlphaCutoff) discard;

    vec3 albedo = base.rgb;
    float metallic = uMetallic;
    float rough = uRoughness;
    vec4 mr = vec4(1.0);
    if (uHasMetallicRoughnessTex == 1) {
        mr = texture(uMetallicRoughnessTex, uv);
        rough *= mr.g;
        metallic *= mr.b;
    }
    rough = clamp(rough, 0.045, 1.0);

    vec3 N = normalize(vNormal);
    if (uHasNormalTex == 1) {
        vec3 nm = texture(uNormalTex, uv).xyz * 2.0 - 1.0;
        nm.xy *= uNormalStrength;
        vec3 T = normalize(vTangent.xyz);
        vec3 B = normalize(cross(N, T) * vTangent.w);
        N = normalize(mat3(T, B, N) * nm);
    }
    vec3 V = normalize(uCameraPos - vWorldPos);

    if (uUnlit == 1) {
        vec3 c = albedo;
        if (uHasEmissiveTex == 1) c += texture(uEmissiveTex, uv).rgb;
        c += uEmissive;
        c.rgb *= base.a;
        fragColor = vec4(c, base.a);
        return;
    }

    float ao = 1.0;
    if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uOcclusionTex, uv).r, uOcclusionStrength);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);

    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= uLightCount) break;
        vec3 L;
        float atten = 1.0;
        if (uLightType[i] == 0) {
            L = normalize(-uLightDir[i]);
        } else {
            vec3 toLight = uLightPos[i] - vWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-4);
            float ratio = dist / max(uLightRange[i], 1e-4);
            atten = clamp(1.0 - ratio * ratio, 0.0, 1.0);
            atten *= atten;
            if (uLightType[i] == 2) {
                float theta = dot(L, normalize(-uLightDir[i]));
                float epsilon = max(uLightCone[i].x - uLightCone[i].y, 1e-4);
                atten *= clamp((theta - uLightCone[i].y) / epsilon, 0.0, 1.0);
            } else if (uLightType[i] == 3) {
                // Area light: approximate with a smooth cosine lobe.
                vec3 nrm = normalize(uLightDir[i]);
                float d = dot(-L, nrm);
                float area = uLightArea[i].x * uLightArea[i].y;
                atten *= clamp(d, 0.0, 1.0) * clamp(area, 0.05, 4.0);
            }
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || atten <= 0.0) continue;
        vec3 H = normalize(V + L);
        float D = distributionGGX(N, H, rough);
        float G = geometrySmith(N, V, L, rough);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 spec = (D * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-4);
        vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
        vec3 radiance = uLightColor[i] * uLightIntensity[i] * atten;
        Lo += (kd * albedo / PI + spec) * radiance * NdotL;
    }

    // Analytic hemisphere ambient (IBL approximation).
    vec3 hemi = mix(uAmbientGround, uAmbientSky, N.y * 0.5 + 0.5);
    vec3 ambient = hemi * albedo * uAmbientIntensity * ao;
    vec3 emissive = uEmissive;
    if (uHasEmissiveTex == 1) emissive += texture(uEmissiveTex, uv).rgb;

    vec3 color = ambient + Lo + emissive;
    color *= shadowFactor(N);

    if (uFogMode > 0) {
        float dist = length(vWorldPos - uCameraPos);
        float f = uFogMode == 1 ? clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 1e-3), 0.0, 1.0)
                                : 1.0 - exp(-uFogDensity * dist);
        color = mix(color, uFogColor, f);
    }

    fragColor = vec4(color * base.a, base.a);
}
)GLSL";

// ---------------------------------------------------------------------------
// Теневой проход (только глубина)
// ---------------------------------------------------------------------------
const char* kShadowVert = R"GLSL(
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
void main() {
    gl_Position = uLightViewProj * uModel * vec4(aPos, 1.0);
}
)GLSL";

const char* kShadowFrag = R"GLSL(
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Вокселы
// ---------------------------------------------------------------------------
const char* kVoxelVert = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 4) in vec4 aColor;
layout(location = 5) in vec2 aUV2;   // x = AO, y = light

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vAo;
out float vLight;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uNormalMatrix) * aNormal);
    vUV = aUV;
    vColor = aColor;
    vAo = aUV2.x;
    vLight = aUV2.y;
    gl_Position = uViewProj * wp;
}
)GLSL";

const char* kVoxelFrag = R"GLSL(
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in float vAo;
in float vLight;

uniform sampler2D uTexture;
uniform int uHasTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
uniform vec3 uCameraPos;
uniform float uFogDensity;
uniform vec3 uFogColor;

out vec4 fragColor;

void main() {
    vec4 base = vColor;
    if (uHasTexture == 1) base *= texture(uTexture, vUV);
    float ndl = max(dot(normalize(vNormal), normalize(-uLightDir)), 0.0);
    float light = clamp(vLight, 0.0, 1.0);
    vec3 lit = base.rgb * (uAmbient + uLightColor * ndl * (0.25 + 0.75 * light));
    lit *= mix(0.45, 1.0, clamp(vAo, 0.0, 1.0));
    float dist = length(vWorldPos - uCameraPos);
    float fog = 1.0 - exp(-uFogDensity * dist);
    lit = mix(lit, uFogColor, fog);
    fragColor = vec4(lit * base.a, base.a);
}
)GLSL";

// ---------------------------------------------------------------------------
// GPU-частицы (инстансированные биллборды / меши)
// ---------------------------------------------------------------------------
const char* kParticleVert = R"GLSL(
layout(location = 0) in vec3 aPos;        // quad/mesh vertex
layout(location = 1) in vec3 aNormal;     // matches the engine's Vertex layout
layout(location = 2) in vec2 aUV;

layout(location = 3) in vec4 iPosSize;    // xyz position, w size
layout(location = 4) in vec4 iColor;      // rgba
layout(location = 5) in vec4 iRotStretch; // x rotation, y stretch, zw (reserved)
layout(location = 6) in vec4 iVelLife;    // xyz velocity, w normalised age

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform vec3 uCameraForward;
uniform int uRenderMode;   // 0 billboard, 1 stretched, 2 horizontal, 3 vertical, 4 mesh
uniform float uStretchScale;

out vec2 vUV;
out vec4 vColor;
out float vAge;

void main() {
    vUV = aUV;
    vColor = iColor;
    vAge = iVelLife.w;

    vec3 worldPos;
    vec3 local = aPos;
    float size = iPosSize.w;

    if (uRenderMode == 4) {
        worldPos = (uModel * vec4(iPosSize.xyz, 1.0)).xyz + local * size;
    } else {
        vec3 right = uCameraRight;
        vec3 up = uCameraUp;
        if (uRenderMode == 1 && length(iVelLife.xyz) > 1e-4) {
            vec3 vel = normalize(iVelLife.xyz);
            up = vel;
            right = normalize(cross(up, uCameraForward));
            local.y *= 1.0 + length(iVelLife.xyz) * uStretchScale;
        } else if (uRenderMode == 2) {
            right = vec3(1.0, 0.0, 0.0);
            up = vec3(0.0, 0.0, 1.0);
        } else if (uRenderMode == 3) {
            right = vec3(1.0, 0.0, 0.0);
            up = vec3(0.0, 1.0, 0.0);
        }
        float c = cos(iRotStretch.x);
        float s = sin(iRotStretch.x);
        vec2 r = vec2(local.x * c - local.y * s, local.x * s + local.y * c);
        worldPos = iPosSize.xyz + (right * r.x + up * r.y) * size;
    }
    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)GLSL";

const char* kParticleFrag = R"GLSL(
in vec2 vUV;
in vec4 vColor;
in float vAge;

uniform sampler2D uTexture;
uniform int uHasTexture;
uniform float uSoftFade;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform int uFogEnabled;

out vec4 fragColor;

void main() {
    vec4 c = vColor;
    if (uHasTexture == 1) {
        vec4 t = texture(uTexture, vUV);
        c *= t;
    } else {
        float d = length(vUV - vec2(0.5)) * 2.0;
        float a = clamp(1.0 - d, 0.0, 1.0);
        c.a *= a * a;
    }
    if (uFogEnabled == 1) {
        c.rgb = mix(c.rgb, uFogColor, clamp(uFogDensity * vAge, 0.0, 1.0));
    }
    c.rgb *= c.a;
    fragColor = c;
}
)GLSL";

// ---------------------------------------------------------------------------
// Постобработка
// ---------------------------------------------------------------------------
const char* kPostVert = R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uTransform;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uTransform * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* kFxaaFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
out vec4 fragColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgbM = texture(uTexture, vUV).rgb;
    vec3 rgbNW = texture(uTexture, vUV + vec2(-1.0, -1.0) * uTexelSize).rgb;
    vec3 rgbNE = texture(uTexture, vUV + vec2(1.0, -1.0) * uTexelSize).rgb;
    vec3 rgbSW = texture(uTexture, vUV + vec2(-1.0, 1.0) * uTexelSize).rgb;
    vec3 rgbSE = texture(uTexture, vUV + vec2(1.0, 1.0) * uTexelSize).rgb;
    float lM = luma(rgbM);
    float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * uTexelSize;
    vec3 rgbA = 0.5 * (texture(uTexture, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(uTexture, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(uTexture, vUV + dir * -0.5).rgb +
                                     texture(uTexture, vUV + dir * 0.5).rgb);
    float lB = luma(rgbB);
    fragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
)GLSL";

const char* kBloomDownFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uThreshold;
out vec4 fragColor;
void main() {
    vec3 c = texture(uTexture, vUV).rgb;
    vec3 a = texture(uTexture, vUV + vec2(-1.0, -1.0) * uTexelSize).rgb;
    vec3 b = texture(uTexture, vUV + vec2(1.0, -1.0) * uTexelSize).rgb;
    vec3 d = texture(uTexture, vUV + vec2(-1.0, 1.0) * uTexelSize).rgb;
    vec3 e = texture(uTexture, vUV + vec2(1.0, 1.0) * uTexelSize).rgb;
    vec3 sum = (c + a + b + d + e) * 0.2;
    float l = dot(sum, vec3(0.2126, 0.7152, 0.0722));
    float w = max(l - uThreshold, 0.0) / max(l, 1e-4);
    fragColor = vec4(sum * w, 1.0);
}
)GLSL";

const char* kBloomUpFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform vec2 uTexelSize;
uniform float uIntensity;
out vec4 fragColor;
void main() {
    vec3 sum = texture(uTexture, vUV).rgb * 4.0;
    sum += texture(uTexture, vUV + vec2(-1.0, 0.0) * uTexelSize).rgb * 2.0;
    sum += texture(uTexture, vUV + vec2(1.0, 0.0) * uTexelSize).rgb * 2.0;
    sum += texture(uTexture, vUV + vec2(0.0, -1.0) * uTexelSize).rgb * 2.0;
    sum += texture(uTexture, vUV + vec2(0.0, 1.0) * uTexelSize).rgb * 2.0;
    sum += texture(uTexture, vUV + vec2(-1.0, -1.0) * uTexelSize).rgb;
    sum += texture(uTexture, vUV + vec2(1.0, -1.0) * uTexelSize).rgb;
    sum += texture(uTexture, vUV + vec2(-1.0, 1.0) * uTexelSize).rgb;
    sum += texture(uTexture, vUV + vec2(1.0, 1.0) * uTexelSize).rgb;
    fragColor = vec4(sum / 16.0 * uIntensity, 1.0);
}
)GLSL";

const char* kTonemapFrag = R"GLSL(
in vec2 vUV;
uniform sampler2D uTexture;
uniform sampler2D uBloom;
uniform int uUseBloom;
uniform float uExposure;
uniform int uMode;          // 0 ACES, 1 Reinhard, 2 Uncharted2, 3 none
uniform float uGamma;
uniform float uVignette;
uniform float uGrain;
uniform float uTime;
uniform float uAberration;
out vec4 fragColor;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 reinhard(vec3 x) { return x / (x + vec3(1.0)); }
vec3 uncharted2(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec2 uv = vUV;
    vec3 c;
    if (uAberration > 0.0) {
        vec2 d = (uv - 0.5) * uAberration;
        c.r = texture(uTexture, uv + d).r;
        c.g = texture(uTexture, uv).g;
        c.b = texture(uTexture, uv - d).b;
    } else {
        c = texture(uTexture, uv).rgb;
    }
    if (uUseBloom == 1) c += texture(uBloom, uv).rgb;
    c *= uExposure;
    if (uMode == 0) c = aces(c);
    else if (uMode == 1) c = reinhard(c);
    else if (uMode == 2) c = uncharted2(c * 2.0) / uncharted2(vec3(11.2));
    if (uVignette > 0.0) {
        float v = 1.0 - uVignette * dot(uv - 0.5, uv - 0.5) * 2.0;
        c *= clamp(v, 0.0, 1.0);
    }
    if (uGrain > 0.0) c += (hash(uv + uTime) - 0.5) * uGrain;
    c = pow(max(c, vec3(0.0)), vec3(1.0 / max(uGamma, 0.01)));
    fragColor = vec4(c, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// Небо
// ---------------------------------------------------------------------------
const char* kSkyVert = R"GLSL(
layout(location = 0) in vec2 aPos;
uniform mat4 uInvViewProj;
out vec3 vDir;
void main() {
    vec4 near = uInvViewProj * vec4(aPos, -1.0, 1.0);
    vec4 far = uInvViewProj * vec4(aPos, 1.0, 1.0);
    vDir = normalize(far.xyz / far.w - near.xyz / near.w);
    gl_Position = vec4(aPos, 1.0, 1.0);
}
)GLSL";

const char* kSkyFrag = R"GLSL(
in vec3 vDir;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;
uniform vec3 uSkyBottom;
uniform vec3 uSunDirection;
uniform vec3 uSunColor;
out vec4 fragColor;
void main() {
    float y = normalize(vDir).y;
    vec3 c;
    if (y > 0.0) c = mix(uSkyHorizon, uSkyTop, pow(clamp(y, 0.0, 1.0), 0.55));
    else c = mix(uSkyHorizon, uSkyBottom, pow(clamp(-y, 0.0, 1.0), 0.5));
    float sun = pow(max(dot(normalize(vDir), normalize(uSunDirection)), 0.0), 220.0);
    float crossrender = pow(max(dot(normalize(vDir), normalize(uSunDirection)), 0.0), 8.0) * 0.25;
    c += uSunColor * (sun * 4.0 + crossrender);
    fragColor = vec4(c, 1.0);
}
)GLSL";

}  // namespace builtin
}  // namespace crossrender
