// Single translation unit that instantiates the vendored stb libraries.
// (stb_truetype is intentionally NOT instantiated: the engine ships its own
// TrueType/CFF rasteriser and SDF generator in engine/src/text/.)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO 0
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
