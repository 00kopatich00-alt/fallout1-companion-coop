#ifndef FALLOUT_PLIB_GNW_GPU_FX_H_
#define FALLOUT_PLIB_GNW_GPU_FX_H_

// The picture filters (pixel-art upscaling, lighting, colour enrichment) run as
// shaders on the graphics card, in the OpenGL context SDL's renderer already
// owns. The processor only hands over the finished game picture.

#include <SDL.h>
#include <stdint.h>

namespace fallout {
namespace gpufx {

// Builds the shaders and textures for a game picture of srcWidth x srcHeight
// pixels in the current OpenGL context. Returns false (and says why in `error`)
// when the graphics card or driver cannot do it; the caller then keeps using the
// processor version.
bool init(int srcWidth, int srcHeight, char* error, int errorSize);

void shutdown();

bool ready();

// "OpenGL version | graphics card", for the log.
const char* driverInfo();

// Draws the game picture (`pixels`, 0x00RRGGBB, `pitchBytes` per row) to the
// window. mode 2 = pixel-art upscaler, 3 = plus lighting, 4 = plus colour
// enrichment. With `changed` false the picture from the last call is reused.
void present(SDL_Renderer* renderer, SDL_Window* window, const uint32_t* pixels, int pitchBytes, int mode, bool changed);

// The FPS counter's picture (0xAARRGGBB), drawn in the top-left corner. NULL hides it.
void setOverlay(const uint32_t* argb, int width, int height);

} // namespace gpufx
} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_GPU_FX_H_ */
