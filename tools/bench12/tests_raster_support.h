// Helpers shared by the raster CPU checks (tests_raster.cpp, tests_raster_csm_ao.cpp).
#pragma once
#include "settings.h"
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
namespace raster_tests {
inline void Require(bool condition, const char *message) { if(!condition) throw std::runtime_error(message); }
inline bool Near(float a, float b) { return std::abs(a-b)<1e-4f; }
inline Options Parse(std::vector<std::string> args)
{
    std::vector<char *> pointers; for(auto &arg:args) pointers.push_back(arg.data());
    return ParseOptions(int(pointers.size()),pointers.data());
}
inline bool Rejected(std::vector<std::string> args) { try { (void)Parse(args); } catch(const std::exception &) { return true; } return false; }
// True when parsing throws an error whose text contains the given words.
inline bool RejectedWith(std::vector<std::string> args, const char *words)
{
    try { (void)Parse(args); } catch(const std::exception &e) { return std::string(e.what()).find(words)!=std::string::npos; }
    return false;
}
}
