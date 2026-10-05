#include "plib/gnw/gpu_fx.h"

#include <SDL_opengl.h>

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#pragma comment(lib, "opengl32.lib")
#endif

namespace fallout {
namespace gpufx {

// OpenGL 2.0+ entry points are fetched at run time (the system library only
// exports OpenGL 1.1).
static PFNGLCREATESHADERPROC gl_CreateShader = NULL;
static PFNGLSHADERSOURCEPROC gl_ShaderSource = NULL;
static PFNGLCOMPILESHADERPROC gl_CompileShader = NULL;
static PFNGLGETSHADERIVPROC gl_GetShaderiv = NULL;
static PFNGLGETSHADERINFOLOGPROC gl_GetShaderInfoLog = NULL;
static PFNGLDELETESHADERPROC gl_DeleteShader = NULL;
static PFNGLCREATEPROGRAMPROC gl_CreateProgram = NULL;
static PFNGLATTACHSHADERPROC gl_AttachShader = NULL;
static PFNGLLINKPROGRAMPROC gl_LinkProgram = NULL;
static PFNGLGETPROGRAMIVPROC gl_GetProgramiv = NULL;
static PFNGLGETPROGRAMINFOLOGPROC gl_GetProgramInfoLog = NULL;
static PFNGLDELETEPROGRAMPROC gl_DeleteProgram = NULL;
static PFNGLUSEPROGRAMPROC gl_UseProgram = NULL;
static PFNGLGETUNIFORMLOCATIONPROC gl_GetUniformLocation = NULL;
static PFNGLUNIFORM1IPROC gl_Uniform1i = NULL;
static PFNGLUNIFORM2IPROC gl_Uniform2i = NULL;
static PFNGLUNIFORM2FPROC gl_Uniform2f = NULL;
static PFNGLUNIFORM4FPROC gl_Uniform4f = NULL;
static PFNGLACTIVETEXTUREPROC gl_ActiveTexture = NULL;
static PFNGLGENFRAMEBUFFERSPROC gl_GenFramebuffers = NULL;
static PFNGLBINDFRAMEBUFFERPROC gl_BindFramebuffer = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC gl_FramebufferTexture2D = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC gl_CheckFramebufferStatus = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC gl_DeleteFramebuffers = NULL;
static PFNGLGENERATEMIPMAPPROC gl_GenerateMipmap = NULL;

static bool g_ready = false;
static int g_srcWidth = 0;
static int g_srcHeight = 0;
static GLuint g_texSrc = 0;
static GLuint g_texUp = 0;
static GLuint g_texOverlay = 0;
static GLuint g_fbo = 0;
static GLuint g_progA = 0;
static GLuint g_progB = 0;

static GLint g_aSrc = -1;
static GLint g_aSrcSize = -1;
static GLint g_bTex = -1;
static GLint g_bOverlay = -1;
static GLint g_bViewSize = -1;
static GLint g_bLighting = -1;
static GLint g_bOverlayRect = -1;

static int g_overlayWidth = 0;
static int g_overlayHeight = 0;

// Texture units used here. SDL's own renderer only uses unit 0, so what it has
// bound there is left alone.
static const int kUnitSrc = 6;
static const int kUnitUp = 7;
static const int kUnitOverlay = 8;

static const char* kVertexShader = R"GLSL(
#version 130
void main()
{
    gl_Position = gl_Vertex;
}
)GLSL";

// Pass A: the xBRZ pixel-art upscaler, one output pixel at a time. Mirrors the
// processor version in xbrz2x.h step by step.
static const char* kUpscaleShader = R"GLSL(
#version 130
uniform sampler2D uSrc;
uniform ivec2 uSrcSize;
out vec4 outColor;

vec3 P(ivec2 p)
{
    p = clamp(p, ivec2(0), uSrcSize - ivec2(1));
    return texelFetch(uSrc, p, 0).rgb;
}

float dist(vec3 a, vec3 b)
{
    vec3 d = (a - b) * 255.0;
    float y = 0.2126 * d.r + 0.7152 * d.g + 0.0722 * d.b;
    float cb = (0.5 / (1.0 - 0.0722)) * (d.b - y);
    float cr = (0.5 / (1.0 - 0.2126)) * (d.r - y);
    return sqrt(y * y + cb * cb + cr * cr);
}

bool same(vec3 a, vec3 b)
{
    return all(equal(a, b));
}

bool similar(vec3 a, vec3 b)
{
    return dist(a, b) < 30.0;
}

// The edge decision at the junction between p, p+(1,0), p+(0,1) and p+(1,1):
// x = blend for the top-left pixel (f), y = top-right (g), z = bottom-left (j),
// w = bottom-right (k). 0 none, 1 normal, 2 dominant.
ivec4 junctionAt(ivec2 p)
{
    if (p.x < 0 || p.y < 0 || p.x > uSrcSize.x - 2 || p.y > uSrcSize.y - 2) {
        return ivec4(0);
    }

    vec3 f = P(p);
    vec3 g = P(p + ivec2(1, 0));
    vec3 j = P(p + ivec2(0, 1));
    vec3 k = P(p + ivec2(1, 1));
    if ((same(f, g) && same(j, k)) || (same(f, j) && same(g, k))) {
        return ivec4(0);
    }

    vec3 b = P(p + ivec2(0, -1));
    vec3 c = P(p + ivec2(1, -1));
    vec3 e = P(p + ivec2(-1, 0));
    vec3 h = P(p + ivec2(2, 0));
    vec3 i = P(p + ivec2(-1, 1));
    vec3 l = P(p + ivec2(2, 1));
    vec3 n = P(p + ivec2(0, 2));
    vec3 o = P(p + ivec2(1, 2));

    float jg = dist(i, f) + dist(f, c) + dist(n, k) + dist(k, h) + 4.0 * dist(j, g);
    float fk = dist(e, j) + dist(j, o) + dist(b, g) + dist(g, l) + 4.0 * dist(f, k);

    ivec4 r = ivec4(0);
    if (jg < fk) {
        int v = (3.6 * jg < fk) ? 2 : 1;
        if (!same(f, g) && !same(f, j)) {
            r.x = v;
        }
        if (!same(k, j) && !same(k, g)) {
            r.w = v;
        }
    } else if (fk < jg) {
        int v = (3.6 * fk < jg) ? 2 : 1;
        if (!same(j, f) && !same(j, k)) {
            r.z = v;
        }
        if (!same(g, f) && !same(g, k)) {
            r.y = v;
        }
    }
    return r;
}

// Offset in the source picture of the spot (vx, vy) of the 3x3 neighbourhood as
// seen after turning it rot * 90 degrees.
ivec2 rotOff(int rot, int vx, int vy)
{
    if (rot == 0) return ivec2(vx, vy);
    if (rot == 1) return ivec2(vy, -vx);
    if (rot == 2) return ivec2(-vx, -vy);
    return ivec2(-vy, vx);
}

// Spot (I, J) of the 2x2 output block in the rotated view -> real spot.
ivec2 cellOf(int rot, int I, int J)
{
    if (rot == 0) return ivec2(I, J);
    if (rot == 1) return ivec2(J, 1 - I);
    if (rot == 2) return ivec2(1 - I, 1 - J);
    return ivec2(1 - J, I);
}

void blendAt(inout vec3 color, int rot, int I, int J, ivec2 sub, vec3 front, float m, float n)
{
    if (cellOf(rot, I, J) == sub) {
        color = mix(color, front, m / n);
    }
}

void main()
{
    ivec2 o = ivec2(gl_FragCoord.xy);
    ivec2 s = o >> 1;
    ivec2 sub = o & ivec2(1);
    vec3 color = P(s);

    int info = 0;
    info |= junctionAt(s + ivec2(-1, -1)).w;
    info |= junctionAt(s + ivec2(0, -1)).z << 2;
    info |= junctionAt(s).x << 4;
    info |= junctionAt(s + ivec2(-1, 0)).y << 6;

    if (info != 0) {
        for (int rot = 0; rot < 4; rot++) {
            int shift = rot * 2;
            int rotated = ((info << shift) | (info >> (8 - shift))) & 255;
            int bottomR = (rotated >> 4) & 3;
            if (bottomR < 1) {
                continue;
            }
            int topR = (rotated >> 2) & 3;
            int bottomL = (rotated >> 6) & 3;

            vec3 vb = P(s + rotOff(rot, 0, -1));
            vec3 vc = P(s + rotOff(rot, 1, -1));
            vec3 vd = P(s + rotOff(rot, -1, 0));
            vec3 ve = P(s + rotOff(rot, 0, 0));
            vec3 vf = P(s + rotOff(rot, 1, 0));
            vec3 vg = P(s + rotOff(rot, -1, 1));
            vec3 vh = P(s + rotOff(rot, 0, 1));
            vec3 vi = P(s + rotOff(rot, 1, 1));

            bool doLineBlend;
            if (bottomR >= 2) {
                doLineBlend = true;
            } else if (topR != 0 && !similar(ve, vg)) {
                doLineBlend = false;
            } else if (bottomL != 0 && !similar(ve, vc)) {
                doLineBlend = false;
            } else if (!similar(ve, vi) && similar(vg, vh) && similar(vh, vi) && similar(vi, vf) && similar(vf, vc)) {
                doLineBlend = false;
            } else {
                doLineBlend = true;
            }

            vec3 px = dist(ve, vf) <= dist(ve, vh) ? vf : vh;

            if (doLineBlend) {
                float fg = dist(vf, vg);
                float hc = dist(vh, vc);
                bool shallow = 2.2 * fg <= hc && !same(ve, vg) && !same(vd, vg);
                bool steep = 2.2 * hc <= fg && !same(ve, vc) && !same(vb, vc);
                if (shallow) {
                    if (steep) {
                        blendAt(color, rot, 0, 1, sub, px, 1.0, 4.0);
                        blendAt(color, rot, 1, 0, sub, px, 1.0, 4.0);
                        blendAt(color, rot, 1, 1, sub, px, 5.0, 6.0);
                    } else {
                        blendAt(color, rot, 1, 0, sub, px, 1.0, 4.0);
                        blendAt(color, rot, 1, 1, sub, px, 3.0, 4.0);
                    }
                } else {
                    if (steep) {
                        blendAt(color, rot, 0, 1, sub, px, 1.0, 4.0);
                        blendAt(color, rot, 1, 1, sub, px, 3.0, 4.0);
                    } else {
                        blendAt(color, rot, 1, 1, sub, px, 1.0, 2.0);
                    }
                }
            } else {
                blendAt(color, rot, 1, 1, sub, px, 21.0, 100.0);
            }
        }
    }

    outColor = vec4(color, 1.0);
}
)GLSL";

// Pass B: fits the upscaled picture to the window, with the lighting and colour
// enrichment of lighting_fx.h (same strengths) when asked for. The glow and the
// wide "surroundings" brightness come from the picture's smaller mipmap levels.
static const char* kFinalShader = R"GLSL(
#version 130
uniform sampler2D uTex;
uniform sampler2D uOverlay;
uniform vec2 uViewSize;
uniform int uLighting;
uniform vec4 uOverlayRect;
out vec4 outColor;

float luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 frag = vec2(gl_FragCoord.x, uViewSize.y - gl_FragCoord.y);
    vec2 uv = frag / uViewSize;
    vec3 c = textureLod(uTex, uv, 0.0).rgb;
    vec3 result = c;

    if (uLighting > 0) {
        vec3 glow = textureLod(uTex, uv, 4.6).rgb;
        vec3 base = textureLod(uTex, uv, 6.5).rgb;

        float boost = 1.0 + 0.35 * (luma(c) - luma(base));
        float bright = max(luma(glow) - 0.52, 0.0);
        vec2 n2 = uv * 2.0 - 1.0;
        float vignette = 1.0 - 0.16 * dot(n2, n2) * 0.5;

        vec3 o = c * boost + glow * bright * 0.40;

        if (uLighting > 1) {
            float mx = max(o.r, max(o.g, o.b));
            float mn = min(o.r, min(o.g, o.b));
            float sat = mx > 0.001 ? (mx - mn) / mx : 0.0;
            float vib = 1.0 + 0.30 * (1.0 - sat);
            float lo0 = luma(o);
            o = vec3(lo0) + (o - vec3(lo0)) * vib;

            float shadow = max(1.0 - lo0 * 2.5, 0.0);
            float highlight = max(lo0 * 2.0 - 1.0, 0.0);
            o.r += 0.014 * highlight - 0.006 * shadow;
            o.g += 0.010 * highlight + 0.004 * shadow;
            o.b += 0.006 * 1.6 * shadow - 0.014 * highlight;
        }

        float lo = luma(o);
        o = vec3(lo) + (o - vec3(lo)) * 1.12;
        o.r *= 1.035;
        o.b *= 0.965;
        o *= vignette;
        o = clamp(o, 0.0, 1.0);
        o = o + 0.04 * o * (1.0 - o) * (o - 0.5) * 4.0;
        result = clamp(o, 0.0, 1.0);
    }

    if (uOverlayRect.z > 0.0) {
        vec2 q = frag - uOverlayRect.xy;
        if (q.x >= 0.0 && q.y >= 0.0 && q.x < uOverlayRect.z && q.y < uOverlayRect.w) {
            vec4 ov = texelFetch(uOverlay, ivec2(q), 0);
            result = mix(result, ov.rgb, ov.a);
        }
    }

    outColor = vec4(result, 1.0);
}
)GLSL";

static bool loadFunctions()
{
#define LOAD(name, type)                                                  \
    gl_##name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); \
    if (gl_##name == NULL) {                                              \
        return false;                                                     \
    }

    LOAD(CreateShader, PFNGLCREATESHADERPROC)
    LOAD(ShaderSource, PFNGLSHADERSOURCEPROC)
    LOAD(CompileShader, PFNGLCOMPILESHADERPROC)
    LOAD(GetShaderiv, PFNGLGETSHADERIVPROC)
    LOAD(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC)
    LOAD(DeleteShader, PFNGLDELETESHADERPROC)
    LOAD(CreateProgram, PFNGLCREATEPROGRAMPROC)
    LOAD(AttachShader, PFNGLATTACHSHADERPROC)
    LOAD(LinkProgram, PFNGLLINKPROGRAMPROC)
    LOAD(GetProgramiv, PFNGLGETPROGRAMIVPROC)
    LOAD(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC)
    LOAD(DeleteProgram, PFNGLDELETEPROGRAMPROC)
    LOAD(UseProgram, PFNGLUSEPROGRAMPROC)
    LOAD(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC)
    LOAD(Uniform1i, PFNGLUNIFORM1IPROC)
    LOAD(Uniform2i, PFNGLUNIFORM2IPROC)
    LOAD(Uniform2f, PFNGLUNIFORM2FPROC)
    LOAD(Uniform4f, PFNGLUNIFORM4FPROC)
    LOAD(ActiveTexture, PFNGLACTIVETEXTUREPROC)
    LOAD(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC)
    LOAD(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC)
    LOAD(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC)
    LOAD(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC)
    LOAD(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC)
    LOAD(GenerateMipmap, PFNGLGENERATEMIPMAPPROC)

#undef LOAD
    return true;
}

static GLuint compileShader(GLenum type, const char* source, const char* what, char* error, int errorSize)
{
    GLuint shader = gl_CreateShader(type);
    gl_ShaderSource(shader, 1, &source, NULL);
    gl_CompileShader(shader);

    GLint ok = 0;
    gl_GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[600];
        log[0] = '\0';
        gl_GetShaderInfoLog(shader, sizeof(log), NULL, log);
        snprintf(error, errorSize, "%s shader did not compile: %s", what, log);
        gl_DeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint buildProgram(const char* fragmentSource, const char* what, char* error, int errorSize)
{
    GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader, "vertex", error, errorSize);
    if (vertex == 0) {
        return 0;
    }
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource, what, error, errorSize);
    if (fragment == 0) {
        gl_DeleteShader(vertex);
        return 0;
    }

    GLuint program = gl_CreateProgram();
    gl_AttachShader(program, vertex);
    gl_AttachShader(program, fragment);
    gl_LinkProgram(program);
    gl_DeleteShader(vertex);
    gl_DeleteShader(fragment);

    GLint ok = 0;
    gl_GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[600];
        log[0] = '\0';
        gl_GetProgramInfoLog(program, sizeof(log), NULL, log);
        snprintf(error, errorSize, "%s program did not link: %s", what, log);
        gl_DeleteProgram(program);
        return 0;
    }
    return program;
}

static GLuint makeTexture(int unit, int width, int height, GLint minFilter, GLint magFilter)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    gl_ActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void shutdown()
{
    if (g_ready || g_texSrc != 0) {
        if (g_fbo != 0 && gl_DeleteFramebuffers != NULL) {
            gl_DeleteFramebuffers(1, &g_fbo);
        }
        if (g_progA != 0 && gl_DeleteProgram != NULL) {
            gl_DeleteProgram(g_progA);
        }
        if (g_progB != 0 && gl_DeleteProgram != NULL) {
            gl_DeleteProgram(g_progB);
        }
        GLuint textures[3] = { g_texSrc, g_texUp, g_texOverlay };
        glDeleteTextures(3, textures);
    }
    g_fbo = g_progA = g_progB = 0;
    g_texSrc = g_texUp = g_texOverlay = 0;
    g_ready = false;
}

bool ready()
{
    return g_ready;
}

static char g_driverInfo[300] = "unknown";

const char* driverInfo()
{
    return g_driverInfo;
}

bool init(int srcWidth, int srcHeight, char* error, int errorSize)
{
    shutdown();
    error[0] = '\0';

    if (SDL_GL_GetCurrentContext() == NULL) {
        snprintf(error, errorSize, "no OpenGL context");
        return false;
    }

    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    snprintf(g_driverInfo, sizeof(g_driverInfo), "%s | %s",
        version != NULL ? reinterpret_cast<const char*>(version) : "?",
        renderer != NULL ? reinterpret_cast<const char*>(renderer) : "?");
    if (!loadFunctions()) {
        snprintf(error, errorSize, "this OpenGL driver lacks shader/framebuffer functions");
        return false;
    }

    // Anything left over from drawing before this would be reported against us.
    while (glGetError() != GL_NO_ERROR) {
    }

    g_progA = buildProgram(kUpscaleShader, "upscale", error, errorSize);
    if (g_progA == 0) {
        return false;
    }
    g_progB = buildProgram(kFinalShader, "final", error, errorSize);
    if (g_progB == 0) {
        shutdown();
        return false;
    }

    g_aSrc = gl_GetUniformLocation(g_progA, "uSrc");
    g_aSrcSize = gl_GetUniformLocation(g_progA, "uSrcSize");
    g_bTex = gl_GetUniformLocation(g_progB, "uTex");
    g_bOverlay = gl_GetUniformLocation(g_progB, "uOverlay");
    g_bViewSize = gl_GetUniformLocation(g_progB, "uViewSize");
    g_bLighting = gl_GetUniformLocation(g_progB, "uLighting");
    g_bOverlayRect = gl_GetUniformLocation(g_progB, "uOverlayRect");

    g_srcWidth = srcWidth;
    g_srcHeight = srcHeight;

    g_texSrc = makeTexture(kUnitSrc, srcWidth, srcHeight, GL_NEAREST, GL_NEAREST);
    g_texUp = makeTexture(kUnitUp, srcWidth * 2, srcHeight * 2, GL_LINEAR, GL_LINEAR);
    g_texOverlay = makeTexture(kUnitOverlay, 1, 1, GL_NEAREST, GL_NEAREST);

    gl_GenFramebuffers(1, &g_fbo);
    gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_texUp, 0);
    GLenum status = gl_CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_ActiveTexture(GL_TEXTURE0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(error, errorSize, "framebuffer incomplete (0x%x)", static_cast<unsigned>(status));
        shutdown();
        return false;
    }
    if (glGetError() != GL_NO_ERROR) {
        snprintf(error, errorSize, "OpenGL error while creating textures");
        shutdown();
        return false;
    }

    g_overlayWidth = 0;
    g_overlayHeight = 0;
    g_ready = true;
    return true;
}

void setOverlay(const uint32_t* argb, int width, int height)
{
    if (!g_ready) {
        return;
    }
    if (argb == NULL || width <= 0 || height <= 0) {
        g_overlayWidth = 0;
        g_overlayHeight = 0;
        return;
    }

    gl_ActiveTexture(GL_TEXTURE0 + kUnitOverlay);
    glBindTexture(GL_TEXTURE_2D, g_texOverlay);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    gl_ActiveTexture(GL_TEXTURE0);
    g_overlayWidth = width;
    g_overlayHeight = height;
}

static void drawFullScreen()
{
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glVertex2f(-1.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glEnd();
}

void present(SDL_Renderer* renderer, SDL_Window* window, const uint32_t* pixels, int pitchBytes, int mode, bool changed)
{
    // Whatever SDL has queued must reach the card before this draws.
    SDL_RenderFlush(renderer);

    // Remember what SDL had set so its own drawing is undisturbed afterwards.
    GLint prevProgram = 0;
    GLint prevFbo = 0;
    GLint prevActive = 0;
    GLint prevViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    if (changed) {
        // The game's picture to the card.
        gl_ActiveTexture(GL_TEXTURE0 + kUnitSrc);
        glBindTexture(GL_TEXTURE_2D, g_texSrc);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitchBytes / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_srcWidth, g_srcHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        // Pass A: double-size pixel-art picture.
        gl_BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glViewport(0, 0, g_srcWidth * 2, g_srcHeight * 2);
        gl_UseProgram(g_progA);
        gl_Uniform1i(g_aSrc, kUnitSrc);
        gl_Uniform2i(g_aSrcSize, g_srcWidth, g_srcHeight);
        drawFullScreen();
        gl_BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));

        // The lighting needs smaller, blurrier copies of it.
        gl_ActiveTexture(GL_TEXTURE0 + kUnitUp);
        glBindTexture(GL_TEXTURE_2D, g_texUp);
        if (mode >= 3) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            gl_GenerateMipmap(GL_TEXTURE_2D);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
    }

    // Pass B: onto the window, keeping the picture's proportions.
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    const float scale = drawableWidth * g_srcHeight < drawableHeight * g_srcWidth
        ? static_cast<float>(drawableWidth) / static_cast<float>(g_srcWidth)
        : static_cast<float>(drawableHeight) / static_cast<float>(g_srcHeight);
    const int viewWidth = static_cast<int>(g_srcWidth * scale + 0.5f);
    const int viewHeight = static_cast<int>(g_srcHeight * scale + 0.5f);
    const int viewX = (drawableWidth - viewWidth) / 2;
    const int viewY = (drawableHeight - viewHeight) / 2;

    gl_BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(viewX, viewY, viewWidth, viewHeight);

    gl_ActiveTexture(GL_TEXTURE0 + kUnitUp);
    glBindTexture(GL_TEXTURE_2D, g_texUp);
    gl_ActiveTexture(GL_TEXTURE0 + kUnitOverlay);
    glBindTexture(GL_TEXTURE_2D, g_texOverlay);

    gl_UseProgram(g_progB);
    gl_Uniform1i(g_bTex, kUnitUp);
    gl_Uniform1i(g_bOverlay, kUnitOverlay);
    gl_Uniform2f(g_bViewSize, static_cast<float>(viewWidth), static_cast<float>(viewHeight));
    gl_Uniform1i(g_bLighting, mode >= 4 ? 2 : (mode >= 3 ? 1 : 0));
    gl_Uniform4f(g_bOverlayRect, 8.0f, 8.0f, static_cast<float>(g_overlayWidth), static_cast<float>(g_overlayHeight));
    drawFullScreen();

    // Hand the state back to SDL.
    gl_UseProgram(static_cast<GLuint>(prevProgram));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    gl_ActiveTexture(static_cast<GLenum>(prevActive));
    if (wasBlend) {
        glEnable(GL_BLEND);
    }
    if (wasScissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (wasDepth) {
        glEnable(GL_DEPTH_TEST);
    }
    if (wasCull) {
        glEnable(GL_CULL_FACE);
    }

    SDL_RenderPresent(renderer);
}

} // namespace gpufx
} // namespace fallout
