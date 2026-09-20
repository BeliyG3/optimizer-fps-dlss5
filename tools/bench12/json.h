#pragma once
#include <map>
#include <string>
#include <vector>

struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object } kind=Kind::Null;
    bool boolean=false;
    double number=0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string,Json> object;
    const Json &At(const std::string &key) const;
};
Json ReadJson(const std::string &text);
std::string JsonString(const std::string &text);
