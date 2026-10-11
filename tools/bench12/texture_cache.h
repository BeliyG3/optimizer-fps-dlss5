#pragma once
#include "image_mips.h"
#include <cstdint>
#include <filesystem>
#include <vector>

// --texture-cache DIR: a scene image's 8-bit mip chain as DecodeImage and BuildMips made it, one file per image named by
// FNV-1a 64 of the encoded bytes and the colour flag (sRGB filtering); the header repeats the key, the encoded size and
// kMipCacheVersion (bump it when DecodeImage or BuildMips change their output). Any thread.
constexpr std::uint32_t kMipCacheVersion=1;
std::uint64_t MipCacheKey(const std::vector<std::uint8_t> &encoded);
// False (and levels untouched) when the file is missing or does not match.
bool ReadMipCache(const std::filesystem::path &dir, std::uint64_t key, std::size_t encodedSize, bool srgb, std::vector<ImageMip> &levels);
// Written to a temporary name and renamed, so a reader never sees half a file; failures only skip the cache.
void WriteMipCache(const std::filesystem::path &dir, std::uint64_t key, std::size_t encodedSize, bool srgb, const std::vector<ImageMip> &levels);
