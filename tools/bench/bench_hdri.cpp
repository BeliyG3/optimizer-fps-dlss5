#include "bench_hdri.h"

#include <cstdio>
#include <cstring>
#include <string>

bool LoadHdr(const char *path, HdrImage &img)
{
    std::vector<unsigned char> data;
    {
        FILE *fp = nullptr; fopen_s(&fp, path, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n <= 0) { fclose(fp); return false; }
        data.resize((size_t) n);
        const size_t got = fread(data.data(), 1, data.size(), fp);
        fclose(fp);
        if (got != data.size()) return false;
    }
    size_t p = 0;
    auto line = [&]() { std::string s; while (p < data.size() && data[p] != '\n') s.push_back((char) data[p++]); if (p < data.size()) ++p; return s; };
    const std::string magic = line();
    if (magic.rfind("#?", 0) != 0) return false;
    for (;;) { const std::string l = line(); if (l.empty()) break; if (p >= data.size()) return false; }
    int w = 0, h = 0;
    if (sscanf_s(line().c_str(), "-Y %d +X %d", &h, &w) != 2 || w <= 0 || h <= 0) return false;
    img.w = w; img.h = h; img.rgb.assign((size_t) w * h * 3, 0.0f);
    std::vector<unsigned char> scan((size_t) w * 4);
    auto store = [&](int y) {
        float *dst = img.rgb.data() + (size_t) y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int e = scan[(size_t) w * 3 + x];
            const float s = e ? std::ldexp(1.0f / 256.0f, e - 128) : 0.0f;
            dst[x * 3 + 0] = (scan[x] + 0.5f) * s;
            dst[x * 3 + 1] = (scan[(size_t) w + x] + 0.5f) * s;
            dst[x * 3 + 2] = (scan[(size_t) w * 2 + x] + 0.5f) * s;
        }
    };
    for (int y = 0; y < h; ++y) {
        if (p + 4 > data.size()) return false;
        const unsigned char *m = &data[p];
        if (m[0] == 2 && m[1] == 2 && ((m[2] << 8) | m[3]) == w && w >= 8 && w < 0x8000) {
            p += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < w) {
                    if (p >= data.size()) return false;
                    int count = data[p++];
                    if (count > 128) { // a run of one value
                        count -= 128;
                        if (p >= data.size() || x + count > w) return false;
                        const unsigned char v = data[p++];
                        while (count-- > 0) scan[(size_t) c * w + x++] = v;
                    } else {           // literal bytes
                        if (count == 0 || p + count > data.size() || x + count > w) return false;
                        while (count-- > 0) scan[(size_t) c * w + x++] = data[p++];
                    }
                }
            }
        } else { // flat RGBE, one 4-byte pixel after another (old-style runs are not produced by 2k HDRIs)
            if (p + (size_t) w * 4 > data.size()) return false;
            for (int x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) scan[(size_t) c * w + x] = data[p + (size_t) x * 4 + c];
            p += (size_t) w * 4;
        }
        store(y);
    }
    return true;
}

void HdrDirToUv(Vec3 w, float &u, float &v)
{
    const Vec3 d{w.x * g_hdriYawCos - w.z * g_hdriYawSin, w.y, w.x * g_hdriYawSin + w.z * g_hdriYawCos}; // world -> HDRI space
    u = std::atan2(d.x, -d.z) * (1.0f / 6.2831853f) + 0.5f;
    if (g_hdriMirror) u = 1.0f - u;
    v = std::acos(std::max(-1.0f, std::min(1.0f, d.y))) * (1.0f / 3.14159265f);
}

Vec3 HdrUvToDir(float u, float v)
{
    const float theta = v * 3.14159265f, phi = (u - 0.5f) * 6.2831853f;
    const float st = std::sin(theta);
    return {st * std::sin(phi), std::cos(theta), -st * std::cos(phi)};
}

unsigned short FloatToHalf(float f)
{
    unsigned int x; memcpy(&x, &f, 4);
    const unsigned int sign = (x >> 16) & 0x8000u;
    int exp = (int) ((x >> 23) & 0xFF) - 127 + 15;
    unsigned int mant = x & 0x7FFFFFu;
    if (exp <= 0) return (unsigned short) sign;                       // underflow -> 0
    if (exp >= 31) return (unsigned short) (sign | 0x7BFFu);          // clamp to the largest finite half
    return (unsigned short) (sign | ((unsigned int) exp << 10) | (mant >> 13));
}
