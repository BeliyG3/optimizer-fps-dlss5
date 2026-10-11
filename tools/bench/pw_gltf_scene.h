// glTF scene model and its evaluation: nodes with TRS, animation samplers, skins, world / joint matrices,
// coordinate conversion, the camera pose and the PW_GLTF_TRACE diagnostics. Part of pw_gltf.h; include that header.
// Requires Vec3 / Mat4 (Identity, Mul, Normalize, Vec3 operators) to be declared before it is included.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pwgltf {

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vertex { Vec3 pos, normal; float material; float localY; float stripe; float colour[3]; float uv[2] = {0, 0}; int texture = -1; bool alphaMask = false; float alphaCutoff = 0.5f;
    int metallicRoughnessTexture = -1, normalTexture = -1; float metallic = 0, normalScale = 1;
    float uv2[2] = {0, 0}; int lightmapAtlas = -1; /* TEXCOORD_1 and the mesh's atlas; -1 = the primitive has no TEXCOORD_1 */ };
// A light the host shades with: every emissive primitive becomes one (its area-weighted centre, its
// radiance times its area), so a scene lit by lamp tubes needs no separate light list.
struct Light { Vec3 pos; float power[3] = {0, 0, 0}; };
struct Primitive { std::vector<Vec3> positions; std::vector<Vec3> normals; std::vector<float> uvs; std::vector<float> uvs2; /* TEXCOORD_1 (lightmap), 2 per vertex, empty = none */ std::vector<uint32_t> indices; float colour[3] = {1, 1, 1}; bool stripes = false; bool hasColour = false; std::string materialName; int texture = -1; bool alphaMask = false; float alphaCutoff = 0.5f;
    float emission[3] = {0, 0, 0}; // emissiveFactor x KHR_materials_emissive_strength: linear radiance, may be far above 1
    float roughness = 1.0f;
    std::vector<std::vector<Vec3>> targets; // morph targets: POSITION deltas per target (facial animation)
    std::vector<uint16_t> joints; std::vector<float> skinWeights; // JOINTS_0 / WEIGHTS_0, 4 per vertex (empty = not skinned)
    int metallicRoughnessTexture = -1, normalTexture = -1; float metallic = 0, normalScale = 1; };
// Embedded image of a .glb (baseColorTexture source): the encoded bytes (PNG / JPEG) for the host to decode.
struct Image { std::vector<uint8_t> bytes; std::string mime; };
// A run of consecutive vertices from BuildVertices that share one texture (draw ranges for the host).
struct DrawRange { uint32_t start = 0, count = 0; int texture = -1; bool alphaMask = false; };
static const float kPalette[6][3] = {{0.85f, 0.25f, 0.2f}, {0.2f, 0.7f, 0.3f}, {0.25f, 0.4f, 0.9f}, {0.9f, 0.8f, 0.2f}, {0.8f, 0.3f, 0.8f}, {0.9f, 0.9f, 0.9f}};
struct Mesh { std::vector<Primitive> primitives; float minY = 0.0f; int lightmapAtlas = 0; /* mesh extras.lightmapAtlas: which lightmap atlas the TEXCOORD_1 of its primitives address */ };
struct Sampler { std::vector<float> times; std::vector<float> values; int components = 3; bool step = false; bool quaternion = false; void Sample(float t, float *out) const; };
struct Channel { int node = -1; int path = 0; /* 0 translation, 1 rotation, 2 scale, 3 morph weights */ Sampler sampler; };
struct Node {
    std::string name; Vec3 translation{0, 0, 0}; Quat rotation; Vec3 scale{1, 1, 1};
    int mesh = -1, camera = -1, parent = -1, skin = -1; std::vector<int> children;
    bool isGround = false; int pwColour = -1;
    bool wind = false; // set by bench12 --wind on veg_ nodes: the node is dynamic (CPU wind displacement); false otherwise, so the D3D11 bench is unchanged
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
    void WorldsAt(float t, std::vector<Mat4> &out, std::vector<Mat4> &local, std::vector<int> &stack) const
    {
        const size_t n = nodes.size();
        out.assign(n, Identity());
        local.resize(n);
        for (size_t i = 0; i < n; ++i) local[i] = LocalAt((int) i, t);
        stack.clear(); stack.reserve(n); stack.insert(stack.end(), roots.begin(), roots.end());
        while (!stack.empty()) {
            const int i = stack.back(); stack.pop_back();
            const int p = nodes[i].parent;
            out[i] = p >= 0 ? Mul(out[p], local[i]) : local[i];
            for (int c : nodes[i].children) if (c >= 0 && (size_t) c < n) stack.push_back(c);
        }
    }
    void WorldsAt(float t, std::vector<Mat4> &out) const
    {
        std::vector<Mat4> local; std::vector<int> stack;
        WorldsAt(t, out, local, stack);
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
    // q and -q are the same rotation and exporters emit either: blending across a sign change takes
    // the long way round and throws the bone sideways for one frame (seen at a walk cycle's seam).
    float sign = 1.0f;
    if (quaternion && c == 4) {
        float dot = 0.0f; for (int i = 0; i < 4; ++i) dot += values[(k - 1) * c + i] * values[k * c + i];
        if (dot < 0.0f) sign = -1.0f;
    }
    for (int i = 0; i < c; ++i) out[i] = values[(k - 1) * c + i] * (1.0f - f) + sign * values[k * c + i] * f;
    if (quaternion && c == 4) { // normalise the interpolated quaternion (nlerp is fine for per-frame keys)
        const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (l > 0) for (int i = 0; i < 4; ++i) out[i] /= l;
    }
}

inline Vec3 TransformPoint(const Mat4 &m, Vec3 p) { return {m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3], m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3], m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3]}; }
inline Vec3 TransformDir(const Mat4 &m, Vec3 p) { return {m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z, m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z, m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z}; }
inline Vec3 ToBench(Vec3 v) { return {v.x, v.y, -v.z}; } // right-handed Y-up -> left-handed Y-up

// PW_GLTF_TRACE=1 prints the loader's stages (to find where a file fails).
inline void Trace(const char *stage) { static const bool on = getenv("PW_GLTF_TRACE") != nullptr; if (on) { fprintf(stderr, "[gltf] %s\n", stage); fflush(stderr); } }

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
