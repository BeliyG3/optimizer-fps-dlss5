#pragma once
#include <cstdint>
#include <vector>
#include <string>
struct RgbaImage { unsigned width=0, height=0; std::vector<uint8_t> pixels; };
struct HdrImage { unsigned width=0, height=0; std::vector<float> pixels; };
RgbaImage DecodeImage(const std::vector<uint8_t> &bytes);
HdrImage LoadHdr(const std::string &path);
