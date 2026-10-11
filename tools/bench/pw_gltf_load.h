// .glb file -> pwgltf::Scene: chunks, JSON, materials, meshes (positions / normals / uvs / skin / morph targets),
// cameras, nodes, skins and animations. Part of pw_gltf.h; include that header.
// Requires Vec3 / Mat4 (Identity, Mul, Normalize, Vec3 operators) to be declared before it is included.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pw_gltf_accessor.h"
#include "pw_gltf_json.h"
#include "pw_gltf_scene.h"

namespace pwgltf {

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
    struct Mat { float colour[3] = {1, 1, 1}; std::string name; bool stripes = false; bool hasColour = false; int texture = -1; bool alphaMask = false; float alpha = 1.0f; float emission[3] = {0, 0, 0}; float roughness = 1.0f;
        int metallicRoughnessTexture = -1, normalTexture = -1; float metallic = 0, normalScale = 1; };
    std::vector<Mat> materials;
    if (const Json *ms = doc.Get("materials")) for (size_t i = 0; i < ms->Size(); ++i) {
        Mat m; m.name = ms->At(i).StrField("name");
        auto imageIndex = [&](const Json *slot) {
            const int ti = slot ? slot->IntField("index", -1) : -1;
            const int image = ti >= 0 && (size_t) ti < textureImage.size() ? textureImage[ti] : -1;
            return image >= 0 && (size_t) image < scene.images.size() ? image : -1;
        };
        m.normalTexture = imageIndex(ms->At(i).Get("normalTexture"));
        if (const Json *normal = ms->At(i).Get("normalTexture")) m.normalScale = (float) normal->NumField("scale", 1.0);
        if (const Json *pbr = ms->At(i).Get("pbrMetallicRoughness")) {
            // Preserve the legacy bench's dielectric default when the factor is absent.
            m.metallic = (float) pbr->NumField("metallicFactor", 0.0);
            m.metallicRoughnessTexture = imageIndex(pbr->Get("metallicRoughnessTexture"));
            if (const Json *c = pbr->Get("baseColorFactor")) { m.hasColour = true; for (int k = 0; k < 3; ++k) m.colour[k] = (float) c->At(k).Num(1.0); m.alpha = (float) c->At(3).Num(1.0); }
            if (const Json *t = pbr->Get("baseColorTexture")) {
                const int ti = t->IntField("index", -1);
                if (ti >= 0 && (size_t) ti < textureImage.size()) m.texture = textureImage[ti];
                if (m.texture >= 0 && (size_t) m.texture >= scene.images.size()) m.texture = -1;
            }
        }
        if (const Json *pbr = ms->At(i).Get("pbrMetallicRoughness")) if (const Json *r = pbr->Get("roughnessFactor")) m.roughness = (float) r->Num(1.0);
        if (const Json *e = ms->At(i).Get("emissiveFactor")) {
            float strength = 1.0f;
            if (const Json *ext = ms->At(i).Get("extensions")) if (const Json *es = ext->Get("KHR_materials_emissive_strength")) if (const Json *s = es->Get("emissiveStrength")) strength = (float) s->Num(1.0);
            for (int k = 0; k < 3; ++k) m.emission[k] = (float) e->At(k).Num(0.0) * strength;
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
        if (const Json *ex = ms->At(i).Get("extras")) mesh.lightmapAtlas = ex->IntField("lightmapAtlas", 0); // a mesh with TEXCOORD_1 and no extras uses atlas 0
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
            Accessor uv2; if (ReadAccessor(doc, bin, attr->IntField("TEXCOORD_1", -1), uv2) && uv2.count == pos.count)
                for (size_t v = 0; v < uv2.count; ++v) { prim.uvs2.push_back(AccessorFloat(uv2, v, 0)); prim.uvs2.push_back(AccessorFloat(uv2, v, 1)); }
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
                prim.alphaCutoff = materials[mi].alpha > 0.5f ? 0.5f / materials[mi].alpha : 1.01f;
                memcpy(prim.emission, materials[mi].emission, sizeof(prim.emission)); prim.roughness = materials[mi].roughness;
                prim.metallic = materials[mi].metallic; prim.normalScale = materials[mi].normalScale;
                prim.metallicRoughnessTexture = prim.uvs.empty() ? -1 : materials[mi].metallicRoughnessTexture;
                prim.normalTexture = prim.uvs.empty() ? -1 : materials[mi].normalTexture; }
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

} // namespace pwgltf
