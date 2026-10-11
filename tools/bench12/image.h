#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <filesystem>
struct RgbaImage { unsigned width=0, height=0; std::vector<uint8_t> pixels; };
struct HdrImage { unsigned width=0, height=0; std::vector<float> pixels; };
RgbaImage DecodeImage(const std::vector<uint8_t> &bytes);
HdrImage LoadHdr(const std::string &path);
// Frame dumps. bgra is width*height*4 bytes, rows top to bottom, alpha ignored by PNG.
// WriteBmp: 32-bit top-down BMP (the default dump). WritePng: 8-bit RGB, lossless; the same pixels as the BMP.
void WriteBmp(const std::filesystem::path &path, unsigned width, unsigned height, const uint8_t *bgra);
void WritePng(const std::filesystem::path &path, unsigned width, unsigned height, const uint8_t *bgra);
