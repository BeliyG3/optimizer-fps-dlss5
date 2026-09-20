#pragma once
#include <vector>
#include <cstdint>
struct ImageMip { unsigned width, height; std::vector<uint8_t> bytes; };
std::vector<ImageMip> BuildMips(unsigned width, unsigned height, unsigned pixelBytes, const void *data, bool srgb);
