// Minimal glTF 2.0 (.glb) scene loader for pw_bench: meshes (positions, indices), node hierarchy
// with TRS, translation / rotation / scale / morph-weight animations (LINEAR / STEP), skins (joints +
// inverse bind matrices, skinned on the CPU in BuildVertices), one camera node, material base colour.
// Coordinates: glTF is right-handed Y-up, the bench is D3D left-handed Y-up -> bench = (x, y, -z).
// Requires Vec3 / Mat4 from pw_bench.cpp to be declared before this header is included.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace pwgltf {

// ---- tiny JSON --------------------------------------------------------------------------------
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

// ---- scene data ---------------------------------------------------------------------------------
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vertex { Vec3 pos, normal; float material; float localY; float stripe; float colour[3]; float uv[2] = {0, 0}; int texture = -1; bool alphaMask = false; float alphaCutoff = 0.5f; };
struct Primitive { std::vector<Vec3> positions; std::vector<Vec3> normals; std::vector<float> uvs; std::vector<uint32_t> indices; float colour[3] = {1, 1, 1}; bool stripes = false; bool hasColour = false; std::string materialName; int texture = -1; bool alphaMask = false; float alphaCutoff = 0.5f;
    std::vector<std::vector<Vec3>> targets; // morph targets: POSITION deltas per target (facial animation)
    std::vector<uint16_t> joints; std::vector<float> skinWeights; }; // JOINTS_0 / WEIGHTS_0, 4 per vertex (empty = not skinned)
// Embedded image of a .glb (baseColorTexture source): the encoded bytes (PNG / JPEG) for the host to decode.
struct Image { std::vector<uint8_t> bytes; std::string mime; };
// A run of consecutive vertices from BuildVertices that share one texture (draw ranges for the host).
struct DrawRange { uint32_t start = 0, count = 0; int texture = -1; bool alphaMask = false; };
static const float kPalette[6][3] = {{0.85f, 0.25f, 0.2f}, {0.2f, 0.7f, 0.3f}, {0.25f, 0.4f, 0.9f}, {0.9f, 0.8f, 0.2f}, {0.8f, 0.3f, 0.8f}, {0.9f, 0.9f, 0.9f}};
struct Mesh { std::vector<Primitive> primitives; float minY = 0.0f; };
struct Sampler { std::vector<float> times; std::vector<float> values; int components = 3; bool step = false; bool quaternion = false; void Sample(float t, float *out) const; };
struct Channel { int node = -1; int path = 0; /* 0 translation, 1 rotation, 2 scale, 3 morph weights */ Sampler sampler; };
struct Node {
    std::string name; Vec3 translation{0, 0, 0}; Quat rotation; Vec3 scale{1, 1, 1};
    int mesh = -1, camera = -1, parent = -1, skin = -1; std::vector<int> children;
    bool isGround = false; int pwColour = -1;
    std::vector<float> weights; // default morph weights (node or mesh), animated by a "weights" channel
};
struct Camera { float yfovDeg = 60.0f; float znear = 0.1f; float zfar = 300.0f; };
// One glTF skin: the joint nodes and their inverse bind matrices (already row-major like Mat4).
struct Skin { std::vector<int> joints; std::vector<Mat4> inverseBind; int skeleton = -1; };

// Inverse of an affine matrix (last row 0 0 0 1); handles the non-uniform scale glTF nodes may carry.
inline Mat4 InvertAffine(const Mat4 &m)
{
    const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2], d = m.m[1][0], e = m.m[1][1], f = m.m[1][2], g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
    const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    const float det0 = a * A + b * B + c * C, det = det0 != 0.0f ? 1.0f / det0 : 0.0f;
    Mat4 r = Identity();
    r.m[0][0] = A * det; r.m[0][1] = (c * h - b * i) * det; r.m[0][2] = (b * f - c * e) * det;
    r.m[1][0] = B * det; r.m[1][1] = (a * i - c * g) * det; r.m[1][2] = (c * d - a * f) * det;
    r.m[2][0] = C * det; r.m[2][1] = (b * g - a * h) * det; r.m[2][2] = (a * e - b * d) * det;
    const float tx = m.m[0][3], ty = m.m[1][3], tz = m.m[2][3];
    for (int k = 0; k < 3; ++k) r.m[k][3] = -(r.m[k][0] * tx + r.m[k][1] * ty + r.m[k][2] * tz);
    return r;
}

struct Scene {
    std::vector<Node> nodes; std::vector<Mesh> meshes; std::vector<Channel> channels; std::vector<Camera> cameras; std::vector<Image> images; std::vector<Skin> skins;
    std::vector<std::vector<uint8_t>> sparseData; // materialised sparse accessors (morph deltas)
    std::vector<int> roots; int cameraNode = -1; float animationEnd = 0.0f; std::string error;
    bool Loaded() const { return !nodes.empty() && error.empty(); }

    // World matrix of a node at time t (seconds), animations applied to translation / rotation / scale.
    // Morph weights of a node at time t (its "weights" channel, else the defaults).
    void WeightsAt(int index, float t, std::vector<float> &out) const
    {
        out = nodes[index].weights;
        for (const Channel &c : channels) {
            if (c.node != index || c.path != 3) continue;
            out.assign((size_t) c.sampler.components, 0.0f);
            c.sampler.Sample(t, out.data());
        }
    }
    Mat4 WorldAt(int index, float t) const
    {
        const Node &n = nodes[index];
        Vec3 tr = n.translation, sc = n.scale; Quat q = n.rotation;
        for (const Channel &c : channels) {
            if (c.node != index || c.path == 3) continue; // morph weights are read by WeightsAt
            float v[4] = {};
            c.sampler.Sample(t, v);
            if (c.path == 0) tr = {v[0], v[1], v[2]};
            else if (c.path == 1) q = {v[0], v[1], v[2], v[3]};
            else if (c.path == 2) sc = {v[0], v[1], v[2]};
        }
        Mat4 local = TRS(tr, q, sc);
        return n.parent >= 0 ? Mul(WorldAt(n.parent, t), local) : local;
    }
    // Local TRS of one node at time t (animation applied).
    Mat4 LocalAt(int index, float t) const
    {
        const Node &n = nodes[index];
        Vec3 tr = n.translation, sc = n.scale; Quat q = n.rotation;
        for (const Channel &c : channels) {
            if (c.node != index || c.path == 3) continue;
            float v[4] = {};
            c.sampler.Sample(t, v);
            if (c.path == 0) tr = {v[0], v[1], v[2]};
            else if (c.path == 1) q = {v[0], v[1], v[2], v[3]};
            else if (c.path == 2) sc = {v[0], v[1], v[2]};
        }
        return TRS(tr, q, sc);
    }
    // World matrices of every node at time t in one pass (WorldAt is O(depth * channels) per call;
    // skinning needs all 100+ joints, so the whole hierarchy is evaluated once instead).
    void WorldsAt(float t, std::vector<Mat4> &out) const
    {
        const size_t n = nodes.size();
        out.assign(n, Identity());
        std::vector<Mat4> local(n);
        for (size_t i = 0; i < n; ++i) local[i] = LocalAt((int) i, t);
        std::vector<int> stack(roots.begin(), roots.end());
        while (!stack.empty()) {
            const int i = stack.back(); stack.pop_back();
            const int p = nodes[i].parent;
            out[i] = p >= 0 ? Mul(out[p], local[i]) : local[i];
            for (int c : nodes[i].children) if (c >= 0 && (size_t) c < n) stack.push_back(c);
        }
    }
    // Joint matrices of the skin on `index`: J_k = inverse(world(node)) * world(joint_k) * inverseBind_k,
    // i.e. mesh-local -> skinned mesh-local, so the node's own world matrix still applies afterwards.
    void JointMatrices(const std::vector<Mat4> &worlds, int index, std::vector<Mat4> &out) const
    {
        out.clear();
        const int si = nodes[index].skin;
        if (si < 0 || (size_t) si >= skins.size()) return;
        const Skin &sk = skins[si];
        const Mat4 invNode = InvertAffine(worlds[index]);
        out.resize(sk.joints.size(), Identity());
        for (size_t k = 0; k < sk.joints.size(); ++k) {
            const int j = sk.joints[k];
            if (j < 0 || (size_t) j >= worlds.size()) continue;
            out[k] = Mul(invNode, Mul(worlds[j], sk.inverseBind[k]));
        }
    }
    static Mat4 TRS(Vec3 t, Quat q, Vec3 s)
    {
        Mat4 m = Identity();
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        m.m[0][0] = (1 - 2 * (yy + zz)) * s.x; m.m[0][1] = (2 * (xy - wz)) * s.y;     m.m[0][2] = (2 * (xz + wy)) * s.z;     m.m[0][3] = t.x;
        m.m[1][0] = (2 * (xy + wz)) * s.x;     m.m[1][1] = (1 - 2 * (xx + zz)) * s.y; m.m[1][2] = (2 * (yz - wx)) * s.z;     m.m[1][3] = t.y;
        m.m[2][0] = (2 * (xz - wy)) * s.x;     m.m[2][1] = (2 * (yz + wx)) * s.y;     m.m[2][2] = (1 - 2 * (xx + yy)) * s.z; m.m[2][3] = t.z;
        return m;
    }
};

inline void Sampler::Sample(float t, float *out) const
{
    const int c = components;
    if (times.empty()) { for (int i = 0; i < c; ++i) out[i] = 0.0f; return; }
    if (t <= times.front()) { for (int i = 0; i < c; ++i) out[i] = values[i]; return; }
    if (t >= times.back()) { for (int i = 0; i < c; ++i) out[i] = values[(times.size() - 1) * c + i]; return; }
    size_t k = 1; while (k < times.size() && times[k] < t) ++k;
    const float t0 = times[k - 1], t1 = times[k];
    const float f = step ? 0.0f : (t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f);
    for (int i = 0; i < c; ++i) out[i] = values[(k - 1) * c + i] * (1.0f - f) + values[k * c + i] * f;
    if (quaternion && c == 4) { // normalise the interpolated quaternion (nlerp is fine for per-frame keys)
        const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (l > 0) for (int i = 0; i < 4; ++i) out[i] /= l;
    }
}

inline Vec3 TransformPoint(const Mat4 &m, Vec3 p) { return {m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3], m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3], m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3]}; }
inline Vec3 TransformDir(const Mat4 &m, Vec3 p) { return {m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z, m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z, m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z}; }
inline Vec3 ToBench(Vec3 v) { return {v.x, v.y, -v.z}; } // right-handed Y-up -> left-handed Y-up

// ---- GLB loading -------------------------------------------------------------------------------
struct Accessor { const uint8_t *data = nullptr; size_t count = 0; int componentType = 0; int components = 1; size_t stride = 0; };

// Dense view of a buffer view (offset / stride / bounds).
inline bool ViewRange(const Json &doc, const std::vector<uint8_t> &bin, int viewIndex, size_t extraOffset, size_t count, size_t elementSize, const uint8_t **data, size_t *stride)
{
    const Json *views = doc.Get("bufferViews"); if (!views || viewIndex < 0 || (size_t) viewIndex >= views->Size()) return false;
    const Json &v = views->At(viewIndex);
    const size_t offset = (size_t) v.NumField("byteOffset", 0) + extraOffset;
    size_t s = (size_t) v.NumField("byteStride", 0); if (s == 0) s = elementSize;
    if (offset + s * (count ? count - 1 : 0) + elementSize > bin.size()) return false;
    *data = bin.data() + offset; *stride = s;
    return true;
}

// Sparse accessors (Blender writes morph-target deltas this way) are materialised into `owned`
// buffers, which must outlive the Accessor (the loader keeps them in Scene::sparseData).
inline bool ReadAccessor(const Json &doc, const std::vector<uint8_t> &bin, int index, Accessor &out, std::vector<std::vector<uint8_t>> *owned = nullptr)
{
    const Json *acc = doc.Get("accessors"); if (!acc || index < 0 || (size_t) index >= acc->Size()) return false;
    const Json &a = acc->At(index);
    const std::string type = a.StrField("type");
    out.components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 1;
    out.componentType = a.IntField("componentType", 5126);
    out.count = (size_t) a.NumField("count", 0);
    const size_t compSize = out.componentType == 5126 || out.componentType == 5125 ? 4 : out.componentType == 5123 || out.componentType == 5122 ? 2 : 1;
    const size_t elem = compSize * out.components;
    const int viewIndex = a.IntField("bufferView", -1);
    const Json *sparse = a.Get("sparse");
    if (sparse == nullptr) {
        if (viewIndex < 0) return false;
        return ViewRange(doc, bin, viewIndex, (size_t) a.NumField("byteOffset", 0), out.count, elem, &out.data, &out.stride);
    }
    if (owned == nullptr) return false;
    // Dense base (zeros without a bufferView), then the sparse values scattered by index.
    std::vector<uint8_t> dense(out.count * elem, 0);
    if (viewIndex >= 0) {
        const uint8_t *base = nullptr; size_t stride = 0;
        if (ViewRange(doc, bin, viewIndex, (size_t) a.NumField("byteOffset", 0), out.count, elem, &base, &stride))
            for (size_t i = 0; i < out.count; ++i) memcpy(dense.data() + i * elem, base + i * stride, elem);
    }
    const size_t n = (size_t) sparse->NumField("count", 0);
    const Json *sIdx = sparse->Get("indices"); const Json *sVal = sparse->Get("values");
    if (!sIdx || !sVal) return false;
    const int idxType = sIdx->IntField("componentType", 5125);
    const size_t idxSize = idxType == 5125 ? 4 : idxType == 5123 ? 2 : 1;
    const uint8_t *idxData = nullptr, *valData = nullptr; size_t idxStride = 0, valStride = 0;
    if (!ViewRange(doc, bin, sIdx->IntField("bufferView", -1), (size_t) sIdx->NumField("byteOffset", 0), n, idxSize, &idxData, &idxStride)) return false;
    if (!ViewRange(doc, bin, sVal->IntField("bufferView", -1), (size_t) sVal->NumField("byteOffset", 0), n, elem, &valData, &valStride)) return false;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t *ip = idxData + k * idxStride;
        const size_t target = idxSize == 4 ? *(const uint32_t *) ip : idxSize == 2 ? *(const uint16_t *) ip : *ip;
        if (target < out.count) memcpy(dense.data() + target * elem, valData + k * valStride, elem);
    }
    owned->push_back(std::move(dense));
    out.data = owned->back().data(); out.stride = elem;
    return true;
}
inline float AccessorFloat(const Accessor &a, size_t i, int c)
{
    const uint8_t *p = a.data + i * a.stride;
    switch (a.componentType) {
    case 5126: { float f; memcpy(&f, p + 4 * c, 4); return f; }
    case 5125: { uint32_t u; memcpy(&u, p + 4 * c, 4); return (float) u; }
    case 5123: { uint16_t u; memcpy(&u, p + 2 * c, 2); return (float) u; }
    case 5122: { int16_t u; memcpy(&u, p + 2 * c, 2); return (float) u; }
    case 5121: return (float) p[c];
    case 5120: return (float) (int8_t) p[c];
    }
    return 0.0f;
}
inline uint32_t AccessorIndex(const Accessor &a, size_t i) { return (uint32_t) AccessorFloat(a, i, 0); }

// PW_GLTF_TRACE=1 prints the loader's stages (to find where a file fails).
inline void Trace(const char *stage) { static const bool on = getenv("PW_GLTF_TRACE") != nullptr; if (on) { fprintf(stderr, "[gltf] %s\n", stage); fflush(stderr); } }

inline bool Load(const char *path, Scene &scene)
{
    Trace("open");
    FILE *fp = nullptr; fopen_s(&fp, path, "rb");
    if (!fp) { scene.error = "cannot open the file"; return false; }
    std::vector<uint8_t> file; fseek(fp, 0, SEEK_END); file.resize((size_t) ftell(fp)); fseek(fp, 0, SEEK_SET);
    fread(file.data(), 1, file.size(), fp); fclose(fp);
    if (file.size() < 20 || memcmp(file.data(), "glTF", 4) != 0) { scene.error = "not a .glb file"; return false; }
    uint32_t jsonLen = 0; memcpy(&jsonLen, file.data() + 12, 4);
    if (20 + jsonLen > file.size()) { scene.error = "truncated JSON chunk"; return false; }
    const char *jsonBegin = (const char *) file.data() + 20;
    std::vector<uint8_t> bin;
    size_t off = 20 + jsonLen;
    if (off + 8 <= file.size()) {
        uint32_t binLen = 0; memcpy(&binLen, file.data() + off, 4);
        if (off + 8 + binLen <= file.size()) bin.assign(file.begin() + off + 8, file.begin() + off + 8 + binLen);
    }
    Json doc; JsonParser parser{jsonBegin, jsonBegin + jsonLen};
    if (!parser.Parse(doc)) { scene.error = "JSON parse error"; return false; }

    // Materials
    Trace("materials");
    // Images (embedded in the binary chunk) and textures (image index per texture)
    if (const Json *ims = doc.Get("images")) for (size_t i = 0; i < ims->Size(); ++i) {
        Image img; img.mime = ims->At(i).StrField("mimeType");
        const int viewIndex = ims->At(i).IntField("bufferView", -1);
        const Json *views = doc.Get("bufferViews");
        if (views && viewIndex >= 0 && (size_t) viewIndex < views->Size()) {
            const Json &view = views->At(viewIndex);
            const size_t off = (size_t) view.IntField("byteOffset", 0), len = (size_t) view.IntField("byteLength", 0);
            if (off + len <= bin.size()) img.bytes.assign(bin.begin() + off, bin.begin() + off + len);
        }
        scene.images.push_back(std::move(img));
    }
    std::vector<int> textureImage;
    if (const Json *ts = doc.Get("textures")) for (size_t i = 0; i < ts->Size(); ++i) textureImage.push_back(ts->At(i).IntField("source", -1));
    struct Mat { float colour[3] = {1, 1, 1}; std::string name; bool stripes = false; bool hasColour = false; int texture = -1; bool alphaMask = false; float alpha = 1.0f; };
    std::vector<Mat> materials;
    if (const Json *ms = doc.Get("materials")) for (size_t i = 0; i < ms->Size(); ++i) {
        Mat m; m.name = ms->At(i).StrField("name");
        if (const Json *pbr = ms->At(i).Get("pbrMetallicRoughness")) {
            if (const Json *c = pbr->Get("baseColorFactor")) { m.hasColour = true; for (int k = 0; k < 3; ++k) m.colour[k] = (float) c->At(k).Num(1.0); m.alpha = (float) c->At(3).Num(1.0); }
            if (const Json *t = pbr->Get("baseColorTexture")) {
                const int ti = t->IntField("index", -1);
                if (ti >= 0 && (size_t) ti < textureImage.size()) m.texture = textureImage[ti];
                if (m.texture >= 0 && (size_t) m.texture >= scene.images.size()) m.texture = -1;
            }
        }
        const std::string alpha = ms->At(i).StrField("alphaMode");
        m.alphaMask = alpha == "MASK" || alpha == "BLEND"; // cut-outs (eyelashes, hair cards): alpha below 0.5 is discarded
        m.stripes = m.name.rfind("Palette_", 0) == 0 || m.name.find("stripe") != std::string::npos;
        materials.push_back(m);
    }
    // Meshes
    Trace("meshes");
    if (const Json *ms = doc.Get("meshes")) for (size_t i = 0; i < ms->Size(); ++i) {
        Mesh mesh; mesh.minY = 1e30f;
        const Json *prims = ms->At(i).Get("primitives");
        for (size_t k = 0; prims && k < prims->Size(); ++k) {
            const Json &pj = prims->At(k);
            if (pj.IntField("mode", 4) != 4) continue; // triangles only
            Primitive prim;
            const Json *attr = pj.Get("attributes"); if (!attr) continue;
            Accessor pos; if (!ReadAccessor(doc, bin, attr->IntField("POSITION", -1), pos)) continue;
            for (size_t v = 0; v < pos.count; ++v) { Vec3 p{AccessorFloat(pos, v, 0), AccessorFloat(pos, v, 1), AccessorFloat(pos, v, 2)}; prim.positions.push_back(p); mesh.minY = std::min(mesh.minY, p.y); }
            Accessor nrm; if (ReadAccessor(doc, bin, attr->IntField("NORMAL", -1), nrm) && nrm.count == pos.count)
                for (size_t v = 0; v < nrm.count; ++v) prim.normals.push_back({AccessorFloat(nrm, v, 0), AccessorFloat(nrm, v, 1), AccessorFloat(nrm, v, 2)});
            Accessor uv; if (ReadAccessor(doc, bin, attr->IntField("TEXCOORD_0", -1), uv) && uv.count == pos.count)
                for (size_t v = 0; v < uv.count; ++v) { prim.uvs.push_back(AccessorFloat(uv, v, 0)); prim.uvs.push_back(AccessorFloat(uv, v, 1)); }
            Accessor jn, jw;
            if (ReadAccessor(doc, bin, attr->IntField("JOINTS_0", -1), jn) && ReadAccessor(doc, bin, attr->IntField("WEIGHTS_0", -1), jw) && jn.count == pos.count && jw.count == pos.count) {
                prim.joints.resize(pos.count * 4); prim.skinWeights.resize(pos.count * 4);
                const float wScale = jw.componentType == 5121 ? 1.0f / 255.0f : jw.componentType == 5123 ? 1.0f / 65535.0f : 1.0f; // normalized integer weights
                for (size_t v = 0; v < pos.count; ++v) for (int c = 0; c < 4; ++c) {
                    prim.joints[v * 4 + c] = (uint16_t) AccessorFloat(jn, v, c);
                    prim.skinWeights[v * 4 + c] = AccessorFloat(jw, v, c) * wScale;
                }
            }
            if (const Json *targets = pj.Get("targets")) for (size_t ti = 0; ti < targets->Size(); ++ti) {
                std::vector<Vec3> deltas;
                Accessor tp;
                if (ReadAccessor(doc, bin, targets->At(ti).IntField("POSITION", -1), tp, &scene.sparseData) && tp.count == pos.count)
                    for (size_t v = 0; v < tp.count; ++v) deltas.push_back({AccessorFloat(tp, v, 0), AccessorFloat(tp, v, 1), AccessorFloat(tp, v, 2)});
                prim.targets.push_back(std::move(deltas)); // empty = target without positions (kept for indexing)
            }
            Accessor idx; if (ReadAccessor(doc, bin, pj.IntField("indices", -1), idx)) for (size_t v = 0; v < idx.count; ++v) prim.indices.push_back(AccessorIndex(idx, v));
            else for (uint32_t v = 0; v < (uint32_t) pos.count; ++v) prim.indices.push_back(v);
            const int mi = pj.IntField("material", -1);
            if (mi >= 0 && (size_t) mi < materials.size()) { memcpy(prim.colour, materials[mi].colour, sizeof(prim.colour)); prim.stripes = materials[mi].stripes; prim.hasColour = materials[mi].hasColour; prim.materialName = materials[mi].name; prim.texture = prim.uvs.empty() ? -1 : materials[mi].texture; prim.alphaMask = materials[mi].alphaMask;
                // The cut-out tests the texture alpha alone, so fold baseColorFactor.a into the threshold
                // (eye occlusion / tearline are BLEND overlays with a low constant alpha).
                prim.alphaCutoff = materials[mi].alpha > 0.5f ? 0.5f / materials[mi].alpha : 1.01f; }
            mesh.primitives.push_back(std::move(prim));
        }
        if (mesh.minY > 1e29f) mesh.minY = 0.0f;
        scene.meshes.push_back(std::move(mesh));
    }
    // Cameras
    Trace("cameras");
    if (const Json *cs = doc.Get("cameras")) for (size_t i = 0; i < cs->Size(); ++i) {
        Camera c;
        if (const Json *p = cs->At(i).Get("perspective")) { c.yfovDeg = (float) (p->NumField("yfov", 1.0472) * 180.0 / 3.14159265); c.znear = (float) p->NumField("znear", 0.1); c.zfar = (float) p->NumField("zfar", 300.0); }
        scene.cameras.push_back(c);
    }
    // Nodes
    Trace("nodes");
    if (const Json *ns = doc.Get("nodes")) for (size_t i = 0; i < ns->Size(); ++i) {
        const Json &nj = ns->At(i);
        Node n; n.name = nj.StrField("name"); n.mesh = nj.IntField("mesh", -1); n.camera = nj.IntField("camera", -1); n.skin = nj.IntField("skin", -1);
        if (const Json *t = nj.Get("translation")) n.translation = {(float) t->At(0).Num(), (float) t->At(1).Num(), (float) t->At(2).Num()};
        if (const Json *r = nj.Get("rotation")) n.rotation = {(float) r->At(0).Num(), (float) r->At(1).Num(), (float) r->At(2).Num(), (float) r->At(3).Num(1.0)};
        if (const Json *s = nj.Get("scale")) n.scale = {(float) s->At(0).Num(1.0), (float) s->At(1).Num(1.0), (float) s->At(2).Num(1.0)};
        if (const Json *w = nj.Get("weights")) for (size_t k = 0; k < w->Size(); ++k) n.weights.push_back((float) w->At(k).Num());
        if (n.weights.empty() && n.mesh >= 0) {
            const Json *ms = doc.Get("meshes");
            if (ms && (size_t) n.mesh < ms->Size()) if (const Json *w = ms->At(n.mesh).Get("weights")) for (size_t k = 0; k < w->Size(); ++k) n.weights.push_back((float) w->At(k).Num());
        }
        if (const Json *m = nj.Get("matrix")) { // decompose translation only (Blender exports TRS, this is a fallback)
            n.translation = {(float) m->At(12).Num(), (float) m->At(13).Num(), (float) m->At(14).Num()};
        }
        if (const Json *ch = nj.Get("children")) for (size_t k = 0; k < ch->Size(); ++k) n.children.push_back(ch->At(k).Int());
        if (const Json *ex = nj.Get("extras")) n.pwColour = ex->IntField("pw_colour", -1);
        n.isGround = n.name == "Ground" || n.name.rfind("Ground", 0) == 0;
        if (n.camera >= 0 && scene.cameraNode < 0) scene.cameraNode = (int) i;
        scene.nodes.push_back(std::move(n));
    }
    for (size_t i = 0; i < scene.nodes.size(); ++i) for (int c : scene.nodes[i].children) if (c >= 0 && (size_t) c < scene.nodes.size()) scene.nodes[c].parent = (int) i;
    for (size_t i = 0; i < scene.nodes.size(); ++i) if (scene.nodes[i].parent < 0) scene.roots.push_back((int) i);
    // Skins: joint node indices + inverse bind matrices (glTF MAT4 is column-major, Mat4 is row-major)
    Trace("skins");
    if (const Json *ss = doc.Get("skins")) for (size_t i = 0; i < ss->Size(); ++i) {
        const Json &sj = ss->At(i);
        Skin sk; sk.skeleton = sj.IntField("skeleton", -1);
        if (const Json *js = sj.Get("joints")) for (size_t k = 0; k < js->Size(); ++k) sk.joints.push_back(js->At(k).Int());
        sk.inverseBind.assign(sk.joints.size(), Identity());
        Accessor ibm;
        if (ReadAccessor(doc, bin, sj.IntField("inverseBindMatrices", -1), ibm))
            for (size_t k = 0; k < sk.joints.size() && k < ibm.count; ++k)
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) sk.inverseBind[k].m[r][c] = AccessorFloat(ibm, k, c * 4 + r);
        scene.skins.push_back(std::move(sk));
    }
    // Animations (all of them, merged; Blender exports one action per object as one animation)
    Trace("animations");
    if (const Json *as = doc.Get("animations")) for (size_t i = 0; i < as->Size(); ++i) {
        const Json &aj = as->At(i);
        const Json *samplers = aj.Get("samplers"); const Json *channels = aj.Get("channels");
        for (size_t k = 0; channels && k < channels->Size(); ++k) {
            const Json &cj = channels->At(k);
            const Json *target = cj.Get("target"); if (!target) continue;
            Channel ch; ch.node = target->IntField("node", -1);
            const std::string path = target->StrField("path");
            ch.path = path == "translation" ? 0 : path == "rotation" ? 1 : path == "scale" ? 2 : path == "weights" ? 3 : -1;
            if (ch.path < 0 || ch.node < 0) continue;
            const int si = cj.IntField("sampler", -1); if (!samplers || si < 0 || (size_t) si >= samplers->Size()) continue;
            const Json &sj = samplers->At(si);
            Accessor in, out;
            if (!ReadAccessor(doc, bin, sj.IntField("input", -1), in) || !ReadAccessor(doc, bin, sj.IntField("output", -1), out)) continue;
            ch.sampler.step = sj.StrField("interpolation") == "STEP";
            ch.sampler.quaternion = ch.path == 1;
            ch.sampler.components = out.components;
            if (ch.path == 3 && in.count > 0) ch.sampler.components = (int) (out.count / in.count); // weights: one scalar per target per key
            for (size_t v = 0; v < in.count; ++v) ch.sampler.times.push_back(AccessorFloat(in, v, 0));
            for (size_t v = 0; v < out.count; ++v) for (int c = 0; c < out.components; ++c) ch.sampler.values.push_back(AccessorFloat(out, v, c));
            if (!ch.sampler.times.empty()) scene.animationEnd = std::max(scene.animationEnd, ch.sampler.times.back());
            scene.channels.push_back(std::move(ch));
        }
    }
    if (scene.nodes.empty()) { scene.error = "no nodes"; return false; }
    Trace("done");
    return true;
}

// Morph + skin one primitive into `pos` / `nrm`, per unique vertex, still in mesh-local space
// (glTF order: morph targets first, skinning after).  `jm` = joint matrices of the node's skin.
inline void DeformPrimitive(const Primitive &prim, const std::vector<float> &w, const std::vector<Mat4> &jm,
                            std::vector<Vec3> &pos, std::vector<Vec3> &nrm)
{
    const size_t n = prim.positions.size();
    pos.resize(n); nrm.resize(n);
    const bool morph = !prim.targets.empty() && !w.empty();
    const bool skin = !prim.joints.empty() && !jm.empty();
    for (size_t v = 0; v < n; ++v) {
        Vec3 p = prim.positions[v];
        Vec3 q = v < prim.normals.size() ? prim.normals[v] : Vec3{0, 1, 0};
        if (morph) for (size_t k = 0; k < prim.targets.size() && k < w.size(); ++k) {
            if (w[k] == 0.0f || v >= prim.targets[k].size()) continue;
            const Vec3 &d = prim.targets[k][v];
            p.x += d.x * w[k]; p.y += d.y * w[k]; p.z += d.z * w[k];
        }
        if (skin) {
            Vec3 sp{0, 0, 0}, sn{0, 0, 0}; float total = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float wk = prim.skinWeights[v * 4 + k];
                if (wk == 0.0f) continue;
                const size_t j = prim.joints[v * 4 + k];
                if (j >= jm.size()) continue;
                sp = sp + TransformPoint(jm[j], p) * wk;
                sn = sn + TransformDir(jm[j], q) * wk;
                total += wk;
            }
            if (total > 0.0f) { p = sp * (1.0f / total); q = Normalize(sn); }
        }
        pos[v] = p; nrm[v] = q;
    }
}

// Builds the vertex list for one instant: world positions at time t and t - dt (previous frame),
// already converted to the bench's coordinate system.  Morph targets and CPU skinning are applied
// per unique vertex, then expanded through the index buffer.
inline void BuildVertices(const Scene &scene, float t, float dt, std::vector<Vertex> &out, std::vector<Vec3> &prevOut, std::vector<DrawRange> *ranges = nullptr)
{
    Trace("build vertices");
    out.clear(); prevOut.clear();
    if (ranges) ranges->clear();
    std::vector<Mat4> worlds, worldsPrev, jm, jmPrev;
    scene.WorldsAt(t, worlds); scene.WorldsAt(t - dt, worldsPrev);
    std::vector<Vec3> dp, dn, dpPrev, dnPrev; // deform scratch, reused by every primitive
    std::vector<float> wNow, wPrev;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const Node &n = scene.nodes[i];
        if (n.mesh < 0 || (size_t) n.mesh >= scene.meshes.size()) continue;
        const Mat4 world = worlds[i], worldPrev = worldsPrev[i];
        const Mesh &mesh = scene.meshes[n.mesh];
        scene.WeightsAt((int) i, t, wNow); scene.WeightsAt((int) i, t - dt, wPrev);
        scene.JointMatrices(worlds, (int) i, jm); scene.JointMatrices(worldsPrev, (int) i, jmPrev);
        if (getenv("PW_GLTF_TRACE")) { fprintf(stderr, "[gltf] node %zu mesh %d prims %zu weights %zu joints %zu\n", i, n.mesh, mesh.primitives.size(), wNow.size(), jm.size()); fflush(stderr); }
        for (const Primitive &prim : mesh.primitives) {
            if (ranges) {
                // Consecutive primitives with the same texture and cut-out flag share one range.
                if (ranges->empty() || ranges->back().texture != prim.texture || ranges->back().alphaMask != prim.alphaMask) {
                    DrawRange r; r.start = (uint32_t) out.size(); r.texture = prim.texture; r.alphaMask = prim.alphaMask; ranges->push_back(r);
                }
            }
            const bool deformed = (!prim.targets.empty() && !wNow.empty()) || (!prim.joints.empty() && !jm.empty());
            DeformPrimitive(prim, wNow, jm, dp, dn);
            if (deformed) DeformPrimitive(prim, wPrev, jmPrev, dpPrev, dnPrev);
            const std::vector<Vec3> &pp = deformed ? dpPrev : dp;
            for (uint32_t index : prim.indices) {
                if (index >= prim.positions.size()) continue;
                Vertex v{};
                if (index * 2 + 1 < prim.uvs.size()) { v.uv[0] = prim.uvs[index * 2]; v.uv[1] = prim.uvs[index * 2 + 1]; }
                v.texture = prim.texture; v.alphaMask = prim.alphaMask; v.alphaCutoff = prim.alphaCutoff;
                v.pos = ToBench(TransformPoint(world, dp[index]));
                v.normal = Normalize(ToBench(TransformDir(world, dn[index])));
                v.material = n.isGround ? -1.0f : -2.0f;
                v.localY = (dp[index].y - mesh.minY) * n.scale.y;
                v.stripe = prim.stripes ? 1.0f : 0.0f;
                if (!prim.hasColour && n.pwColour >= 0) memcpy(v.colour, kPalette[n.pwColour % 6], sizeof(v.colour));
                else memcpy(v.colour, prim.colour, sizeof(v.colour));
                out.push_back(v);
                prevOut.push_back(ToBench(TransformPoint(worldPrev, pp[index])));
                if (ranges) ++ranges->back().count;
            }
        }
    }
}

// World-space bounds (bench coordinates) of the meshes in the subtree of `anchor` at time t,
// deformation included -- what the face camera frames.  Returns false if the subtree has no mesh.
inline bool MeshBounds(const Scene &scene, float t, int anchor, Vec3 &lo, Vec3 &hi)
{
    std::vector<Mat4> worlds, jm; scene.WorldsAt(t, worlds);
    std::vector<Vec3> dp, dn; std::vector<float> w;
    lo = {1e30f, 1e30f, 1e30f}; hi = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const Node &n = scene.nodes[i];
        if (n.mesh < 0 || (size_t) n.mesh >= scene.meshes.size()) continue;
        bool under = anchor < 0 || (int) i == anchor;
        for (int p = n.parent; p >= 0 && !under; p = scene.nodes[p].parent) under = p == anchor;
        if (!under) continue;
        scene.WeightsAt((int) i, t, w); scene.JointMatrices(worlds, (int) i, jm);
        for (const Primitive &prim : scene.meshes[n.mesh].primitives) {
            DeformPrimitive(prim, w, jm, dp, dn);
            for (const Vec3 &p : dp) {
                const Vec3 q = ToBench(TransformPoint(worlds[i], p));
                lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y); lo.z = std::min(lo.z, q.z);
                hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y); hi.z = std::max(hi.z, q.z);
                any = true;
            }
        }
    }
    return any;
}

// Camera pose at time t: eye and target in bench coordinates (glTF cameras look down -Z).
inline bool CameraAt(const Scene &scene, float t, Vec3 &eye, Vec3 &target, float &fovYDeg)
{
    if (scene.cameraNode < 0) return false;
    const Mat4 world = scene.WorldAt(scene.cameraNode, t);
    eye = ToBench(TransformPoint(world, {0, 0, 0}));
    const Vec3 fwd = Normalize(ToBench(TransformDir(world, {0, 0, -1})));
    target = eye + fwd;
    const Node &n = scene.nodes[scene.cameraNode];
    fovYDeg = (n.camera >= 0 && (size_t) n.camera < scene.cameras.size()) ? scene.cameras[n.camera].yfovDeg : 60.0f;
    return true;
}

} // namespace pwgltf
