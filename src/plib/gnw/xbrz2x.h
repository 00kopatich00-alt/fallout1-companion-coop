#ifndef FALLOUT_PLIB_GNW_XBRZ2X_H_
#define FALLOUT_PLIB_GNW_XBRZ2X_H_

// Edge-aware 2x pixel-art upscaler (an implementation of the xBRZ idea): looks at
// the pattern of colours around every pixel, finds diagonal and curved edges and
// blends along them, so staircases become smooth lines instead of bigger blocks.
// Input and output are 0x00RRGGBB pixels. The output is (2*width) x (2*height).
// Rows are processed on a small shared thread pool, and a changed part of the
// picture can be redone on its own.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <thread>
#include <vector>

namespace fallout {
namespace xbrz2x {

static const float kEqualColorTolerance = 30.0f;
static const float kDominantDirectionThreshold = 3.6f;
static const float kSteepDirectionThreshold = 2.2f;

enum { BLEND_NONE = 0, BLEND_NORMAL = 1, BLEND_DOMINANT = 2 };

// ---------------------------------------------------------------------------
// A pool of worker threads that live as long as the program. Starting threads
// for every pass of every frame (what this used to do) cost more than some of
// the passes themselves. Rows are handed out in small chunks, so the busy parts
// of a picture do not leave the other threads idle.
// ---------------------------------------------------------------------------
class RowPool {
public:
    static RowPool& get()
    {
        static RowPool pool;
        return pool;
    }

    void run(int height, const std::function<void(int, int)>& fn)
    {
        if (height <= 0) {
            return;
        }
        if (workers_.empty() || height < 64) {
            fn(0, height);
            return;
        }

        const int chunkRows = 16;
        const int total = (height + chunkRows - 1) / chunkRows;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &fn;
            height_ = height;
            chunkRows_ = chunkRows;
            totalChunks_ = total;
            nextChunk_ = 0;
            completedChunks_ = 0;
            jobId_++;
        }
        wake_.notify_all();

        work(fn, height, chunkRows, total);

        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&]() { return completedChunks_ >= total; });
        job_ = NULL;
    }

private:
    RowPool()
    {
        unsigned int cores = std::thread::hardware_concurrency();
        int workers = static_cast<int>(cores) - 1;
        if (workers > 7) {
            workers = 7;
        }
        for (int i = 0; i < workers; i++) {
            workers_.emplace_back([this]() { workerMain(); });
        }
    }

    ~RowPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (size_t i = 0; i < workers_.size(); i++) {
            workers_[i].join();
        }
    }

    void work(const std::function<void(int, int)>& fn, int height, int chunkRows, int total)
    {
        for (;;) {
            const int chunk = nextChunk_.fetch_add(1);
            if (chunk >= total) {
                return;
            }
            const int begin = chunk * chunkRows;
            const int end = begin + chunkRows < height ? begin + chunkRows : height;
            fn(begin, end);
            if (completedChunks_.fetch_add(1) + 1 >= total) {
                std::lock_guard<std::mutex> lock(mutex_);
                done_.notify_all();
            }
        }
    }

    void workerMain()
    {
        unsigned int seen = 0;
        for (;;) {
            const std::function<void(int, int)>* fn;
            int height;
            int chunkRows;
            int total;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&]() { return stop_ || jobId_ != seen; });
                if (stop_) {
                    return;
                }
                seen = jobId_;
                fn = job_;
                height = height_;
                chunkRows = chunkRows_;
                total = totalChunks_;
            }
            if (fn != NULL) {
                work(*fn, height, chunkRows, total);
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(int, int)>* job_ = NULL;
    int height_ = 0;
    int chunkRows_ = 1;
    int totalChunks_ = 0;
    std::atomic<int> nextChunk_ { 0 };
    std::atomic<int> completedChunks_ { 0 };
    unsigned int jobId_ = 0;
    bool stop_ = false;
};

// Runs fn(firstRow, endRow) over [0, height) on the pool.
template <typename Fn>
static void parallelRows(int height, Fn fn)
{
    std::function<void(int, int)> wrapper = fn;
    RowPool::get().run(height, wrapper);
}

static inline float colorDist(uint32_t a, uint32_t b)
{
    if (a == b) {
        return 0.0f;
    }
    const float rd = static_cast<float>(static_cast<int>((a >> 16) & 255) - static_cast<int>((b >> 16) & 255));
    const float gd = static_cast<float>(static_cast<int>((a >> 8) & 255) - static_cast<int>((b >> 8) & 255));
    const float bd = static_cast<float>(static_cast<int>(a & 255) - static_cast<int>(b & 255));
    const float kb = 0.0722f;
    const float kr = 0.2126f;
    const float kg = 1.0f - kb - kr;
    const float y = kr * rd + kg * gd + kb * bd;
    const float cb = (0.5f / (1.0f - kb)) * (bd - y);
    const float cr = (0.5f / (1.0f - kr)) * (rd - y);
    return sqrtf(y * y + cb * cb + cr * cr);
}

static inline bool colorEq(uint32_t a, uint32_t b)
{
    return colorDist(a, b) < kEqualColorTolerance;
}

// back = (front * m + back * (n - m)) / n, per channel.
static inline uint32_t alphaGrad(uint32_t back, uint32_t front, int m, int n)
{
    const uint32_t r = (((front >> 16) & 255) * m + ((back >> 16) & 255) * (n - m)) / n;
    const uint32_t g = (((front >> 8) & 255) * m + ((back >> 8) & 255) * (n - m)) / n;
    const uint32_t b = ((front & 255) * m + (back & 255) * (n - m)) / n;
    return (r << 16) | (g << 8) | b;
}

// Which of the four pixels around the junction between (x,y), (x+1,y), (x,y+1),
// (x+1,y+1) get blended into it. Pixels are named like this (the junction lies
// between f, g, j and k):
//   a b c d
//   e f g h
//   i j k l
//   m n o p
struct BlendResult {
    uint8_t f, g, j, k;
};

static inline BlendResult preProcessCorners(uint32_t b, uint32_t c,
    uint32_t e, uint32_t f, uint32_t g, uint32_t h,
    uint32_t i, uint32_t j, uint32_t k, uint32_t l,
    uint32_t n, uint32_t o)
{
    BlendResult result = { 0, 0, 0, 0 };

    if ((f == g && j == k) || (f == j && g == k)) {
        return result;
    }

    const float weight = 4.0f;
    const float jg = colorDist(i, f) + colorDist(f, c) + colorDist(n, k) + colorDist(k, h) + weight * colorDist(j, g);
    const float fk = colorDist(e, j) + colorDist(j, o) + colorDist(b, g) + colorDist(g, l) + weight * colorDist(f, k);

    if (jg < fk) {
        const bool dominantGradient = kDominantDirectionThreshold * jg < fk;
        if (f != g && f != j) {
            result.f = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
        }
        if (k != j && k != g) {
            result.k = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
        }
    } else if (fk < jg) {
        const bool dominantGradient = kDominantDirectionThreshold * fk < jg;
        if (j != f && j != k) {
            result.j = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
        }
        if (g != f && g != k) {
            result.g = dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL;
        }
    }

    return result;
}

static inline int clampInt(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

// Corner bits of a pixel's blend info: top-left 0-1, top-right 2-3,
// bottom-right 4-5, bottom-left 6-7.
static inline int getTopR(uint8_t blend) { return (blend >> 2) & 3; }
static inline int getBottomR(uint8_t blend) { return (blend >> 4) & 3; }
static inline int getBottomL(uint8_t blend) { return (blend >> 6) & 3; }

// A rectangle of source pixels: [x0, x1) x [y0, y1).
struct Rect {
    int x0, y0, x1, y1;
};

// Upscales width x height source pixels (pitch in pixels) into the
// 2*width x 2*height `dst` (dstPitch in pixels). `junctions` is state kept
// between calls, sized width*height by this function.
//
// With `dirty` NULL the whole picture is done. With a rectangle only what that
// rectangle of changed source pixels can influence is redone (the junctions
// within 2 pixels of it, the output within 3), and everything else in `dst` and
// `junctions` is assumed to still be right from an earlier call. The rectangle
// of source pixels that was rewritten is returned in `written`.
static void upscale(const uint32_t* src, int width, int height, int srcPitch,
    uint32_t* dst, int dstPitch, std::vector<BlendResult>& junctions,
    const Rect* dirty = NULL, Rect* written = NULL)
{
    const BlendResult none = { 0, 0, 0, 0 };
    if (dirty == NULL || junctions.size() != static_cast<size_t>(width) * height) {
        junctions.assign(static_cast<size_t>(width) * height, none);
        dirty = NULL;
    }
    BlendResult* junction = junctions.data();

#define XBRZ_SRC(px, py) src[clampInt((py), 0, height - 1) * srcPitch + clampInt((px), 0, width - 1)]

    // Junction rectangle to recompute and pixel rectangle to write.
    Rect junctionRect = { 0, 0, width - 1, height - 1 };
    Rect pixelRect = { 0, 0, width, height };
    if (dirty != NULL) {
        junctionRect.x0 = clampInt(dirty->x0 - 2, 0, width - 1);
        junctionRect.y0 = clampInt(dirty->y0 - 2, 0, height - 1);
        junctionRect.x1 = clampInt(dirty->x1 + 2, 0, width - 1);
        junctionRect.y1 = clampInt(dirty->y1 + 2, 0, height - 1);
        pixelRect.x0 = clampInt(dirty->x0 - 3, 0, width);
        pixelRect.y0 = clampInt(dirty->y0 - 3, 0, height);
        pixelRect.x1 = clampInt(dirty->x1 + 3, 0, width);
        pixelRect.y1 = clampInt(dirty->y1 + 3, 0, height);
    }
    if (written != NULL) {
        *written = pixelRect;
    }

    // Pass 1: find the edges at every junction between four pixels. Each junction
    // owns its entry, so rows can run in parallel.
    parallelRows(junctionRect.y1 - junctionRect.y0, [&](int firstRow, int endRow) {
        for (int y = junctionRect.y0 + firstRow; y < junctionRect.y0 + endRow; y++) {
            for (int x = junctionRect.x0; x < junctionRect.x1; x++) {
                const uint32_t f = XBRZ_SRC(x, y);
                const uint32_t g = XBRZ_SRC(x + 1, y);
                const uint32_t j = XBRZ_SRC(x, y + 1);
                const uint32_t k = XBRZ_SRC(x + 1, y + 1);
                if (f == g && g == j && j == k) {
                    junction[static_cast<size_t>(y) * width + x] = none;
                    continue;
                }

                junction[static_cast<size_t>(y) * width + x] = preProcessCorners(
                    XBRZ_SRC(x, y - 1), XBRZ_SRC(x + 1, y - 1),
                    XBRZ_SRC(x - 1, y), f, g, XBRZ_SRC(x + 2, y),
                    XBRZ_SRC(x - 1, y + 1), j, k, XBRZ_SRC(x + 2, y + 1),
                    XBRZ_SRC(x, y + 2), XBRZ_SRC(x + 1, y + 2));
            }
        }
    });

    // Pass 2: write each source pixel as a 2x2 block and blend its corners.
    parallelRows(pixelRect.y1 - pixelRect.y0, [&](int firstRow, int endRow) {
        for (int y = pixelRect.y0 + firstRow; y < pixelRect.y0 + endRow; y++) {
            for (int x = pixelRect.x0; x < pixelRect.x1; x++) {
                const uint32_t center = XBRZ_SRC(x, y);
                uint32_t* out = dst + static_cast<size_t>(y) * 2 * dstPitch + static_cast<size_t>(x) * 2;
                out[0] = center;
                out[1] = center;
                out[dstPitch] = center;
                out[dstPitch + 1] = center;

                // This pixel's four corners come from the four junctions around it.
                uint8_t blendInfo = 0;
                if (x > 0 && y > 0) {
                    blendInfo |= junction[static_cast<size_t>(y - 1) * width + (x - 1)].k; // top-left
                }
                if (y > 0 && x < width - 1) {
                    blendInfo |= static_cast<uint8_t>(junction[static_cast<size_t>(y - 1) * width + x].j << 2); // top-right
                }
                if (x < width - 1 && y < height - 1) {
                    blendInfo |= static_cast<uint8_t>(junction[static_cast<size_t>(y) * width + x].f << 4); // bottom-right
                }
                if (x > 0 && y < height - 1) {
                    blendInfo |= static_cast<uint8_t>(junction[static_cast<size_t>(y) * width + (x - 1)].g << 6); // bottom-left
                }
                if (blendInfo == 0) {
                    continue;
                }

                for (int rot = 0; rot < 4; rot++) {
                    // Rotate the blend bits so the corner under inspection is bottom-right.
                    const int shift = rot * 2;
                    const uint8_t rotated = static_cast<uint8_t>(((blendInfo << shift) | (blendInfo >> (8 - shift))) & 0xFF);
                    if (getBottomR(rotated) < BLEND_NORMAL) {
                        continue;
                    }

                    // The 3x3 neighbourhood as seen after rotating by rot * 90 degrees.
                    uint32_t view[3][3]; // [row][col]
                    for (int vy = -1; vy <= 1; vy++) {
                        for (int vx = -1; vx <= 1; vx++) {
                            int ox;
                            int oy;
                            switch (rot) {
                            case 0:
                                ox = vx;
                                oy = vy;
                                break;
                            case 1:
                                ox = vy;
                                oy = -vx;
                                break;
                            case 2:
                                ox = -vx;
                                oy = -vy;
                                break;
                            default:
                                ox = -vy;
                                oy = vx;
                                break;
                            }
                            view[vy + 1][vx + 1] = XBRZ_SRC(x + ox, y + oy);
                        }
                    }
                    const uint32_t vb = view[0][1], vc = view[0][2];
                    const uint32_t vd = view[1][0], ve = view[1][1], vf = view[1][2];
                    const uint32_t vg = view[2][0], vh = view[2][1], vi = view[2][2];

                    bool doLineBlend;
                    if (getBottomR(rotated) >= BLEND_DOMINANT) {
                        doLineBlend = true;
                    } else if (getTopR(rotated) != BLEND_NONE && !colorEq(ve, vg)) {
                        doLineBlend = false;
                    } else if (getBottomL(rotated) != BLEND_NONE && !colorEq(ve, vc)) {
                        doLineBlend = false;
                    } else if (!colorEq(ve, vi) && colorEq(vg, vh) && colorEq(vh, vi) && colorEq(vi, vf) && colorEq(vf, vc)) {
                        doLineBlend = false;
                    } else {
                        doLineBlend = true;
                    }

                    const uint32_t px = colorDist(ve, vf) <= colorDist(ve, vh) ? vf : vh;

                    // Output positions in the rotated view -> real positions in the 2x2 block.
                    uint32_t* cell[2][2]; // [J][I]
                    for (int J = 0; J < 2; J++) {
                        for (int I = 0; I < 2; I++) {
                            int px2;
                            int py2;
                            switch (rot) {
                            case 0:
                                px2 = I;
                                py2 = J;
                                break;
                            case 1:
                                px2 = J;
                                py2 = 1 - I;
                                break;
                            case 2:
                                px2 = 1 - I;
                                py2 = 1 - J;
                                break;
                            default:
                                px2 = 1 - J;
                                py2 = I;
                                break;
                            }
                            cell[J][I] = out + py2 * dstPitch + px2;
                        }
                    }

                    if (doLineBlend) {
                        const float fg = colorDist(vf, vg);
                        const float hc = colorDist(vh, vc);
                        const bool haveShallowLine = kSteepDirectionThreshold * fg <= hc && ve != vg && vd != vg;
                        const bool haveSteepLine = kSteepDirectionThreshold * hc <= fg && ve != vc && vb != vc;

                        if (haveShallowLine) {
                            if (haveSteepLine) {
                                *cell[1][0] = alphaGrad(*cell[1][0], px, 1, 4);
                                *cell[0][1] = alphaGrad(*cell[0][1], px, 1, 4);
                                *cell[1][1] = alphaGrad(*cell[1][1], px, 5, 6);
                            } else {
                                *cell[0][1] = alphaGrad(*cell[0][1], px, 1, 4);
                                *cell[1][1] = alphaGrad(*cell[1][1], px, 3, 4);
                            }
                        } else {
                            if (haveSteepLine) {
                                *cell[1][0] = alphaGrad(*cell[1][0], px, 1, 4);
                                *cell[1][1] = alphaGrad(*cell[1][1], px, 3, 4);
                            } else {
                                *cell[1][1] = alphaGrad(*cell[1][1], px, 1, 2);
                            }
                        }
                    } else {
                        *cell[1][1] = alphaGrad(*cell[1][1], px, 21, 100);
                    }
                }
            }
        }
    });

#undef XBRZ_SRC
}

} // namespace xbrz2x
} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_XBRZ2X_H_ */
