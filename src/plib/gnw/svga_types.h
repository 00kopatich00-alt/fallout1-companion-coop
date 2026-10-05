#ifndef FALLOUT_PLIB_GNW_SVGA_TYPES_H_
#define FALLOUT_PLIB_GNW_SVGA_TYPES_H_

namespace fallout {

// NOTE: These typedefs always appear in this order in every implementation file
// with extended debug info. However `mouse.c` does not have DirectX types
// implying it does not include `svga.h` which does so to expose primary
// DirectDraw objects.

typedef void(UpdatePaletteFunc)();
typedef void(ResetModeFunc)();
typedef int(SetModeFunc)();
typedef void(ScreenTransBlitFunc)(unsigned char* srcBuf, unsigned int srcW, unsigned int srcH, unsigned int subX, unsigned int subY, unsigned int subW, unsigned int subH, unsigned int dstX, unsigned int dstY, unsigned char trans);
typedef void(ScreenBlitFunc)(unsigned char* srcBuf, unsigned int srcW, unsigned int srcH, unsigned int subX, unsigned int subY, unsigned int subW, unsigned int subH, unsigned int dstX, unsigned int dstY);

typedef struct VideoOptions {
    int width;
    int height;
    bool fullscreen;
    int scale;
    // Fullscreen only: when above 0 the game's own resolution is the monitor's
    // real pixel size divided by this, filling the whole screen with no black
    // bars (the picture is then shown nativeScale times larger).
    int nativeScale;
} VideoOptions;

} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_SVGA_TYPES_H_ */
