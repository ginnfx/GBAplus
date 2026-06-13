/* Single translation unit that compiles the stb_image implementation.
   Cover art is PNG/JPEG, so only those decoders are enabled. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
