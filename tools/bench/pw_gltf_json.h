// Tiny JSON reader for the glTF loader: a Json value tree and the recursive-descent parser that fills it.
// Part of pw_gltf.h; include that header.
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pwgltf {

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double number = 0.0;
    bool boolean = false;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json *Get(const char *key) const
    {
        if (kind != Object) return nullptr;
        for (const auto &f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    const Json &At(size_t i) const { static Json none; return (kind == Array && i < items.size()) ? items[i] : none; }
    size_t Size() const { return kind == Array ? items.size() : 0; }
    double Num(double fallback = 0.0) const { return kind == Number ? number : fallback; }
    int Int(int fallback = -1) const { return kind == Number ? (int) number : fallback; }
    double NumField(const char *key, double fallback) const { const Json *j = Get(key); return j ? j->Num(fallback) : fallback; }
    int IntField(const char *key, int fallback = -1) const { const Json *j = Get(key); return j ? j->Int(fallback) : fallback; }
    std::string StrField(const char *key) const { const Json *j = Get(key); return (j && j->kind == String) ? j->text : std::string(); }
};

struct JsonParser {
    const char *p, *end;
    void Ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool Parse(Json &out)
    {
        Ws();
        if (p >= end) return false;
        if (*p == '{') {
            out.kind = Json::Object; ++p; Ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (p < end) {
                Json key; if (!ParseString(key)) return false;
                Ws(); if (p >= end || *p != ':') return false; ++p;
                Json value; if (!Parse(value)) return false;
                out.fields.emplace_back(key.text, std::move(value));
                Ws(); if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '[') {
            out.kind = Json::Array; ++p; Ws();
            if (p < end && *p == ']') { ++p; return true; }
            while (p < end) {
                Json value; if (!Parse(value)) return false;
                out.items.push_back(std::move(value));
                Ws(); if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '"') return ParseString(out);
        if (!strncmp(p, "true", 4)) { out.kind = Json::Bool; out.boolean = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { out.kind = Json::Bool; out.boolean = false; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { out.kind = Json::Null; p += 4; return true; }
        char *stop = nullptr;
        out.number = strtod(p, &stop);
        if (stop == p) return false;
        out.kind = Json::Number; p = stop; return true;
    }
    bool ParseString(Json &out)
    {
        Ws();
        if (p >= end || *p != '"') return false;
        ++p; out.kind = Json::String; out.text.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                case 'n': out.text += '\n'; break;
                case 't': out.text += '\t'; break;
                case 'u': { if (p + 4 < end) { unsigned code = (unsigned) strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); if (code < 0x80) out.text += (char) code; else out.text += '?'; p += 4; } break; }
                default: out.text += *p; break;
                }
            } else out.text += *p;
            ++p;
        }
        if (p >= end) return false;
        ++p; return true;
    }
};

} // namespace pwgltf
