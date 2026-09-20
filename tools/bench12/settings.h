#pragma once
#include "camera.h"
#include "lamps.h"
#include <array>
#include <filesystem>

struct Bookmark { bool valid=false; CameraState pose{}; };
struct Settings {
    Options options;
    CameraState pose{};
    bool hasPose=false, menu=true, overlay=true;
    float speed=3;
    std::array<Bookmark,4> bookmarks{};
    std::vector<LampGroup> lamps;
};
std::string EncodeSettings(const Settings &settings);
Settings DecodeSettings(const std::string &text, const Settings &defaults={});
Settings LoadSettings(const std::filesystem::path &path, const Settings &defaults={});
void SaveSettings(const std::filesystem::path &path, const Settings &settings);
Options CommandLineOptions(int argc, char **argv, const Options &defaults);
