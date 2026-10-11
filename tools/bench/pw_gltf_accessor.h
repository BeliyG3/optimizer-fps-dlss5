// Buffer-view and accessor reading for the glTF loader: bounds-checked dense views, sparse accessors, typed element access.
// Part of pw_gltf.h; include that header.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pw_gltf_json.h"

namespace pwgltf {

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

} // namespace pwgltf
