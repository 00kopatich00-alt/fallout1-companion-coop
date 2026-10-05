#include "plib/gnw/svga.h"

#include <stdio.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "game/coopnet.h"
#include "plib/gnw/gnw.h"
#include "plib/gnw/gpu_fx.h"
#include "plib/gnw/grbuf.h"
#include "plib/gnw/mouse.h"
#include "plib/gnw/lighting_fx.h"
#include "plib/gnw/winmain.h"
#include "plib/gnw/xbrz2x.h"

#if _WIN32
#include <windows.h>

#include <psapi.h>

#pragma comment(lib, "psapi.lib")
#endif

namespace fallout {

// True when the filters run on the graphics card (see gpu_fx.h). Otherwise the
// processor version below is used.
static bool gGpuReady = false;

// FPS counter (F11): frames the game presented in the last second, the filter
// mode, where it runs, and the game's memory.
static bool gShowFps = false;
static unsigned int gFpsWindowStart = 0;
static int gFpsFrames = 0;
static int gFpsShown = 0;
static SDL_Texture* gFpsTexture = NULL;
static int gFpsTextureWidth = 0;
static int gFpsTextureHeight = 0;

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect scr_size;

// 0x6ACA18
ScreenBlitFunc* scr_blit = GNW95_ShowRect;

SDL_Window* gSdlWindow = NULL;
SDL_Surface* gSdlSurface = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Texture* gSdlTexture = NULL;
SDL_Surface* gSdlTextureSurface = NULL;

// How the picture is scaled up to the window instead of hard pixel blocks.
//   0 = plain (blocky)
//   1 = smooth: whole-number enlargement with hard edges, then a soft shrink
//   2 = pixel-art upscaler (xBRZ) to double size, then a soft fit to the window
//   3 = the same plus a lighting pass (glow, local contrast, vignette, colour grade)
//   4 = the same plus colour enrichment (regions pushed apart in hue, vibrance, split toning)
// F8 cycles through them; the choice is kept in this file next to the game.
static SDL_Texture* gSdlScaledTarget = NULL;
static SDL_Texture* gSdlUpscaledTexture = NULL;
static std::vector<uint32_t> gLastFramePixels;
static bool gUpscaledValid = false;

// The pixel-art and lighting filters take longer than a frame at high
// resolutions. They run on a background thread: the game hands over a copy of
// its picture and carries on, and the window shows the newest finished one.
struct UpscaleWorker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool exit = false;
    bool jobPending = false; // an input waits to be taken
    std::vector<uint32_t> input;
    int jobWidth = 0;
    int jobHeight = 0;
    int jobMode = 0;
    unsigned jobGeneration = 0;
    std::vector<uint32_t> output; // newest finished picture
    bool resultReady = false;
    unsigned resultGeneration = 0;
};
static UpscaleWorker* gWorker = NULL;
static unsigned gGeneration = 1; // bumped whenever pending results become useless (mode change, resize)

static void upscaleWorkerMain(UpscaleWorker* worker)
{
    std::vector<uint32_t> in;
    std::vector<uint32_t> out;
    std::vector<xbrz2x::BlendResult> upscaleScratch;
    lightingfx::Scratch lightingScratch;

    for (;;) {
        int width;
        int height;
        int mode;
        unsigned generation;
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->cv.wait(lock, [worker]() { return worker->exit || worker->jobPending; });
            if (worker->exit) {
                return;
            }
            in.swap(worker->input);
            width = worker->jobWidth;
            height = worker->jobHeight;
            mode = worker->jobMode;
            generation = worker->jobGeneration;
            worker->jobPending = false;
            worker->cv.notify_all();
        }

        out.resize(static_cast<size_t>(width) * height * 4);
        xbrz2x::upscale(in.data(), width, height, width, out.data(), width * 2, upscaleScratch);
        if (mode >= 3) {
            lightingfx::apply(out.data(), width * 2, height * 2, width * 2, lightingScratch, mode >= 4);
        }

        {
            std::lock_guard<std::mutex> lock(worker->mutex);
            worker->output.swap(out);
            worker->resultReady = true;
            worker->resultGeneration = generation;
            worker->cv.notify_all();
        }
    }
}

static void stopUpscaleWorker()
{
    if (gWorker == NULL) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gWorker->mutex);
        gWorker->exit = true;
        gWorker->cv.notify_all();
    }
    gWorker->thread.join();
    delete gWorker;
    gWorker = NULL;
}
static int gScalingMode = 2;
static bool gScalingModeLoaded = false;
static const char* const kSmoothScalingFile = "smooth_scaling.txt";

static void smoothScalingLoadSetting()
{
    if (gScalingModeLoaded) {
        return;
    }
    gScalingModeLoaded = true;

    FILE* file = fopen(kSmoothScalingFile, "r");
    if (file != NULL) {
        int value = 2;
        if (fscanf(file, "%d", &value) == 1 && value >= 0 && value <= 4) {
            gScalingMode = value;
        }
        fclose(file);
    }
}

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// 0x4CB310
void GNW95_SetPaletteEntries(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }
}

// 0x4CB568
void GNW95_SetPalette(unsigned char* palette)
{
    if (gSdlSurface != NULL && gSdlSurface->format->palette != NULL) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        SDL_BlitSurface(gSdlSurface, NULL, gSdlTextureSurface, NULL);
    }
}

// 0x4CB850
void GNW95_ShowRect(unsigned char* src, unsigned int srcPitch, unsigned int a3, unsigned int srcX, unsigned int srcY, unsigned int srcWidth, unsigned int srcHeight, unsigned int destX, unsigned int destY)
{
    buf_to_buf(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
}

bool svga_init(VideoOptions* video_options)
{
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

    // Native fullscreen needs the monitor's REAL pixels. Without this Windows
    // pretends the screen is smaller on a scaled display (a 2560x1600 laptop at
    // 150% shows up as 1707x1067) and stretches the game's window up afterwards,
    // which is the blurry "not native" picture.
    if (video_options->fullscreen && video_options->nativeScale > 0) {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        return false;
    }

    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

    if (video_options->fullscreen) {
        // Fullscreen at the desktop's own resolution (the picture is scaled up to
        // it) instead of switching the monitor to the game's tiny video mode.
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    gSdlWindow = SDL_CreateWindow(GNW95_title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        video_options->width * video_options->scale,
        video_options->height * video_options->scale,
        windowFlags);
    if (gSdlWindow == NULL) {
        return false;
    }

    {
        int winW = 0, winH = 0, drawW = 0, drawH = 0;
        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
        SDL_GL_GetDrawableSize(gSdlWindow, &drawW, &drawH);
        SDL_DisplayMode desktop;
        int deskW = 0, deskH = 0;
        if (SDL_GetDesktopDisplayMode(0, &desktop) == 0) {
            deskW = desktop.w;
            deskH = desktop.h;
        }
        char sizeLog[200];
        snprintf(sizeLog, sizeof(sizeLog), "\nVideo: fullscreen=%d nativeScale=%d window=%dx%d drawable=%dx%d desktop=%dx%d requested=%dx%d scale=%d\n",
            video_options->fullscreen ? 1 : 0, video_options->nativeScale, winW, winH, drawW, drawH, deskW, deskH,
            video_options->width, video_options->height, video_options->scale);
        FILE* sizeFile = fopen("video_info.txt", "w");
        if (sizeFile != NULL) {
            fputs(sizeLog, sizeFile);
            fclose(sizeFile);
        }
    }

    // Native fullscreen: size the game's picture from the monitor itself.
    if (video_options->fullscreen && video_options->nativeScale > 0) {
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_GL_GetDrawableSize(gSdlWindow, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0) {
            SDL_GetWindowSize(gSdlWindow, &drawableWidth, &drawableHeight);
        }
        int nativeWidth = drawableWidth / video_options->nativeScale;
        int nativeHeight = drawableHeight / video_options->nativeScale;
        if (nativeWidth >= 640 && nativeHeight >= 480) {
            video_options->width = nativeWidth;
            video_options->height = nativeHeight;
            video_options->scale = video_options->nativeScale;
        }
    }

    if (!createRenderer(video_options->width, video_options->height)) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;

        return false;
    }

    gSdlSurface = SDL_CreateRGBSurface(0,
        video_options->width,
        video_options->height,
        8,
        0,
        0,
        0,
        0);
    if (gSdlSurface == NULL) {
        destroyRenderer();

        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    scr_size.ulx = 0;
    scr_size.uly = 0;
    scr_size.lrx = video_options->width - 1;
    scr_size.lry = video_options->height - 1;

    mouse_blit_trans = NULL;
    scr_blit = GNW95_ShowRect;
    mouse_blit = GNW95_ShowRect;

    return true;
}

void svga_exit()
{
    destroyRenderer();

    if (gSdlWindow != NULL) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&scr_size);
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == NULL) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == NULL) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, NULL, NULL, NULL) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == NULL) {
        return false;
    }

    smoothScalingLoadSetting();

    // Sharp-bilinear scaling: the picture is first enlarged by a whole number
    // with hard edges (so no blur), and that bigger picture is then shrunk to the
    // window smoothly. Blocky pixels become clean, softly blended edges.
    SDL_SetTextureScaleMode(gSdlTexture, SDL_ScaleModeNearest);

    int outputWidth = width;
    int outputHeight = height;
    SDL_GetRendererOutputSize(gSdlRenderer, &outputWidth, &outputHeight);
    int factor = (outputWidth + width - 1) / width;
    if (factor < 2) {
        factor = 2;
    } else if (factor > 6) {
        factor = 6;
    }
    gSdlScaledTarget = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, width * factor, height * factor);
    if (gSdlScaledTarget != NULL) {
        SDL_SetTextureScaleMode(gSdlScaledTarget, SDL_ScaleModeLinear);
    }

    // The pixel-art upscaler's picture: double size, shown with soft scaling.
    gSdlUpscaledTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width * 2, height * 2);
    if (gSdlUpscaledTexture != NULL) {
        SDL_SetTextureScaleMode(gSdlUpscaledTexture, SDL_ScaleModeLinear);
    }
    gLastFramePixels.clear();
    gUpscaledValid = false;
    gGeneration++;

    // The same filters as shaders on the graphics card; the processor version
    // stays as the fallback when that is not possible.
    char gpuError[400];
    gGpuReady = gpufx::init(width, height, gpuError, sizeof(gpuError));
    FILE* infoFile = fopen("video_info.txt", "a");
    if (infoFile != NULL) {
        fprintf(infoFile, "OpenGL: %s\n", gpufx::driverInfo());
        if (gGpuReady) {
            fprintf(infoFile, "GPU filters: ready\n");
        } else {
            fprintf(infoFile, "GPU filters: not available (%s) - using the processor version\n", gpuError);
        }
        fclose(infoFile);
    }

    return true;
}

static void destroyRenderer()
{
    stopUpscaleWorker();

    if (gFpsTexture != NULL) {
        SDL_DestroyTexture(gFpsTexture);
        gFpsTexture = NULL;
    }

    if (gGpuReady) {
        gpufx::shutdown();
        gGpuReady = false;
    }

    if (gSdlUpscaledTexture != NULL) {
        SDL_DestroyTexture(gSdlUpscaledTexture);
        gSdlUpscaledTexture = NULL;
    }

    if (gSdlScaledTarget != NULL) {
        SDL_DestroyTexture(gSdlScaledTarget);
        gSdlScaledTarget = NULL;
    }

    if (gSdlTextureSurface != NULL) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = NULL;
    }

    if (gSdlTexture != NULL) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = NULL;
    }

    if (gSdlRenderer != NULL) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

// ---- FPS counter -----------------------------------------------------------

static std::vector<uint32_t> gFpsPixels;
static bool gFpsDirty = false;

struct FpsGlyph {
    char ch;
    uint8_t rows[5]; // 3 bits per row, left to right
};

static const FpsGlyph kFpsGlyphs[] = {
    { '0', { 7, 5, 5, 5, 7 } }, { '1', { 2, 6, 2, 2, 7 } }, { '2', { 7, 1, 7, 4, 7 } },
    { '3', { 7, 1, 7, 1, 7 } }, { '4', { 5, 5, 7, 1, 1 } }, { '5', { 7, 4, 7, 1, 7 } },
    { '6', { 7, 4, 7, 5, 7 } }, { '7', { 7, 1, 2, 2, 2 } }, { '8', { 7, 5, 7, 5, 7 } },
    { '9', { 7, 5, 7, 1, 7 } }, { 'F', { 7, 4, 6, 4, 4 } }, { 'P', { 7, 5, 7, 4, 4 } },
    { 'S', { 7, 4, 7, 1, 7 } }, { 'M', { 5, 7, 7, 5, 5 } }, { 'O', { 7, 5, 5, 5, 7 } },
    { 'D', { 6, 5, 5, 5, 6 } }, { 'E', { 7, 4, 7, 4, 7 } }, { 'G', { 7, 4, 5, 5, 7 } },
    { 'U', { 5, 5, 5, 5, 7 } }, { 'B', { 6, 5, 6, 5, 6 } }, { 'C', { 7, 4, 4, 4, 7 } },
    { 'L', { 4, 4, 4, 4, 7 } },
};

static int gFpsOverlayWidth = 0;
static int gFpsOverlayHeight = 0;

static void buildFpsOverlay(int fps)
{
    unsigned long long memoryMb = 0;
#if _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    ZeroMemory(&counters, sizeof(counters));
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        memoryMb = counters.PrivateUsage / (1024 * 1024);
    }
#endif

    const char* where = gScalingMode >= 2 ? (gGpuReady ? "GPU" : "CPU") : "SDL";
    char text[96];
    snprintf(text, sizeof(text), "FPS %d  MODE %d  %s  MEM %lluMB", fps, gScalingMode, where, memoryMb);

    const int scale = 4;
    const int cellWidth = 4 * scale;
    const int length = static_cast<int>(strlen(text));
    gFpsOverlayWidth = length * cellWidth + 2 * scale * 2;
    gFpsOverlayHeight = 5 * scale + 2 * scale * 2;
    gFpsPixels.assign(static_cast<size_t>(gFpsOverlayWidth) * gFpsOverlayHeight, 0xB0000000u);

    for (int i = 0; i < length; i++) {
        const FpsGlyph* glyph = NULL;
        for (size_t g = 0; g < sizeof(kFpsGlyphs) / sizeof(kFpsGlyphs[0]); g++) {
            if (kFpsGlyphs[g].ch == text[i]) {
                glyph = &kFpsGlyphs[g];
                break;
            }
        }
        if (glyph == NULL) {
            continue; // spaces and anything unknown stay empty
        }
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if ((glyph->rows[row] & (4 >> col)) == 0) {
                    continue;
                }
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        const int px = 2 * scale + i * cellWidth + col * scale + dx;
                        const int py = 2 * scale + row * scale + dy;
                        gFpsPixels[static_cast<size_t>(py) * gFpsOverlayWidth + px] = 0xFFFFFFFFu;
                    }
                }
            }
        }
    }

    gFpsDirty = true;
    if (gGpuReady) {
        gpufx::setOverlay(gFpsPixels.data(), gFpsOverlayWidth, gFpsOverlayHeight);
    }
}

static void fpsFrame()
{
    gFpsFrames++;
    const unsigned int now = SDL_GetTicks();
    if (gFpsWindowStart == 0) {
        gFpsWindowStart = now;
    }
    if (now - gFpsWindowStart >= 1000) {
        gFpsShown = static_cast<int>(static_cast<unsigned long long>(gFpsFrames) * 1000 / (now - gFpsWindowStart));
        gFpsFrames = 0;
        gFpsWindowStart = now;
        if (gShowFps) {
            buildFpsOverlay(gFpsShown);
        }
    }
}

// The FPS picture over the SDL renderer's own drawing (the modes that do not run on the card).
static void drawFpsOverlaySdl()
{
    if (!gShowFps || gFpsPixels.empty()) {
        return;
    }

    if (gFpsTexture == NULL || gFpsTextureWidth != gFpsOverlayWidth || gFpsTextureHeight != gFpsOverlayHeight) {
        if (gFpsTexture != NULL) {
            SDL_DestroyTexture(gFpsTexture);
        }
        gFpsTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, gFpsOverlayWidth, gFpsOverlayHeight);
        gFpsTextureWidth = gFpsOverlayWidth;
        gFpsTextureHeight = gFpsOverlayHeight;
        gFpsDirty = true;
        if (gFpsTexture != NULL) {
            SDL_SetTextureBlendMode(gFpsTexture, SDL_BLENDMODE_BLEND);
        }
    }
    if (gFpsTexture == NULL) {
        return;
    }
    if (gFpsDirty) {
        SDL_UpdateTexture(gFpsTexture, NULL, gFpsPixels.data(), gFpsOverlayWidth * 4);
        gFpsDirty = false;
    }

    int outputWidth = 0;
    int outputHeight = 0;
    SDL_GetRendererOutputSize(gSdlRenderer, &outputWidth, &outputHeight);
    const float factor = outputWidth > 0 ? static_cast<float>(screenGetWidth()) / static_cast<float>(outputWidth) : 1.0f;
    SDL_Rect destination;
    destination.x = static_cast<int>(8 * factor);
    destination.y = static_cast<int>(8 * factor);
    destination.w = static_cast<int>(gFpsOverlayWidth * factor + 0.5f);
    destination.h = static_cast<int>(gFpsOverlayHeight * factor + 0.5f);
    SDL_RenderCopy(gSdlRenderer, gFpsTexture, NULL, &destination);
}

void svga_toggle_fps_overlay()
{
    gShowFps = !gShowFps;
    if (gShowFps) {
        buildFpsOverlay(gFpsShown);
    } else if (gGpuReady) {
        gpufx::setOverlay(NULL, 0, 0);
    }
}

bool svga_fps_overlay_enabled()
{
    return gShowFps;
}

void renderPresent()
{
    // Coop remote screen: on the host, stream this frame to the driving
    // client; on the client, draw the host's screen instead of our own.
    coopnet_remote_screen_frame_hook();

    fpsFrame();

    // The filters on the graphics card: the game's picture goes over as it is
    // and everything else happens there.
    if (gGpuReady && gScalingMode >= 2) {
        const int width = gSdlTextureSurface->w;
        const int height = gSdlTextureSurface->h;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        const uint32_t* pixels = static_cast<const uint32_t*>(gSdlTextureSurface->pixels);
        const int pitchInPixels = gSdlTextureSurface->pitch / 4;

        bool changed = !gUpscaledValid || gLastFramePixels.size() != pixelCount;
        if (!changed) {
            for (int y = 0; y < height && !changed; y++) {
                changed = memcmp(pixels + static_cast<size_t>(y) * pitchInPixels, gLastFramePixels.data() + static_cast<size_t>(y) * width, static_cast<size_t>(width) * 4) != 0;
            }
        }
        if (changed) {
            gLastFramePixels.resize(pixelCount);
            for (int y = 0; y < height; y++) {
                memcpy(gLastFramePixels.data() + static_cast<size_t>(y) * width, pixels + static_cast<size_t>(y) * pitchInPixels, static_cast<size_t>(width) * 4);
            }
        }

        gpufx::present(gSdlRenderer, gSdlWindow, pixels, gSdlTextureSurface->pitch, gScalingMode, changed);
        gUpscaledValid = true;
        return;
    }

    SDL_UpdateTexture(gSdlTexture, NULL, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);

    if (gScalingMode >= 2 && gSdlUpscaledTexture != NULL) {
        // Pixel-art upscaler: redo the doubled picture only when the frame
        // changed, then fit it to the window softly.
        const int width = gSdlTextureSurface->w;
        const int height = gSdlTextureSurface->h;
        const size_t pixelCount = static_cast<size_t>(width) * height;
        const uint32_t* pixels = static_cast<const uint32_t*>(gSdlTextureSurface->pixels);
        const int pitchInPixels = gSdlTextureSurface->pitch / 4;

        if (gWorker == NULL) {
            gWorker = new UpscaleWorker();
            gWorker->thread = std::thread(upscaleWorkerMain, gWorker);
        }

        bool changed = !gUpscaledValid || gLastFramePixels.size() != pixelCount;
        if (!changed) {
            for (int y = 0; y < height && !changed; y++) {
                changed = memcmp(pixels + static_cast<size_t>(y) * pitchInPixels, gLastFramePixels.data() + static_cast<size_t>(y) * width, static_cast<size_t>(width) * 4) != 0;
            }
        }

        if (changed) {
            std::unique_lock<std::mutex> lock(gWorker->mutex);

            // Right after a mode change or resize there is nothing valid to show:
            // wait for the worker so the screen never shows a stale picture.
            const bool mustWait = !gUpscaledValid;
            if (mustWait) {
                gWorker->cv.wait(lock, []() { return !gWorker->jobPending; });
            }

            // Otherwise, if the worker is still busy with the previous picture
            // this one is simply skipped -- the next present tries again.
            if (!gWorker->jobPending) {
                gWorker->input.resize(pixelCount);
                for (int y = 0; y < height; y++) {
                    memcpy(gWorker->input.data() + static_cast<size_t>(y) * width, pixels + static_cast<size_t>(y) * pitchInPixels, static_cast<size_t>(width) * 4);
                }
                gWorker->jobWidth = width;
                gWorker->jobHeight = height;
                gWorker->jobMode = gScalingMode;
                gWorker->jobGeneration = gGeneration;
                gWorker->jobPending = true;
                gWorker->cv.notify_all();

                gLastFramePixels.resize(pixelCount);
                memcpy(gLastFramePixels.data(), gWorker->input.data(), pixelCount * 4);

                if (mustWait) {
                    gWorker->cv.wait(lock, []() { return gWorker->resultReady && gWorker->resultGeneration == gGeneration; });
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(gWorker->mutex);
            if (gWorker->resultReady) {
                if (gWorker->resultGeneration == gGeneration) {
                    SDL_UpdateTexture(gSdlUpscaledTexture, NULL, gWorker->output.data(), width * 2 * 4);
                    gUpscaledValid = true;
                }
                gWorker->resultReady = false;
            }
        }

        SDL_RenderClear(gSdlRenderer);
        SDL_RenderCopy(gSdlRenderer, gSdlUpscaledTexture, NULL, NULL);
    } else if (gScalingMode == 1 && gSdlScaledTarget != NULL) {
        // Pass 1: whole-number enlargement with hard edges into the big target.
        // The logical size is switched off for it so the target is filled
        // edge to edge.
        SDL_RenderSetLogicalSize(gSdlRenderer, 0, 0);
        if (SDL_SetRenderTarget(gSdlRenderer, gSdlScaledTarget) == 0) {
            SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
            SDL_SetRenderTarget(gSdlRenderer, NULL);
        }
        SDL_RenderSetLogicalSize(gSdlRenderer, screenGetWidth(), screenGetHeight());

        // Pass 2: smooth shrink of that to the window.
        SDL_RenderClear(gSdlRenderer);
        SDL_RenderCopy(gSdlRenderer, gSdlScaledTarget, NULL, NULL);
    } else {
        SDL_RenderClear(gSdlRenderer);
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, NULL, NULL);
    }

    drawFpsOverlaySdl();

    SDL_RenderPresent(gSdlRenderer);
}

void svga_toggle_smooth_scaling()
{
    gScalingMode = (gScalingMode + 1) % 5;
    gUpscaledValid = false;
    gGeneration++;

    FILE* file = fopen(kSmoothScalingFile, "w");
    if (file != NULL) {
        fprintf(file, "%d\n", gScalingMode);
        fclose(file);
    }
}

int svga_scaling_mode()
{
    return gScalingMode;
}

} // namespace fallout
