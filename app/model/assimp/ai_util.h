#pragma once

#include <algorithm>
#include <memory>
#include <type_traits>
#include <vector>

#include <assimp/scene.h>
#include <nlohmann/json.hpp>

#include "model/ts/ts_shape.h"

// Small helpers shared by the Assimp bridge, the converter and the validator.
namespace ts {

Mat4 toMat4(const aiMatrix4x4& a);
aiMatrix4x4 toAi(const Mat4& m);
inline Vec3 toVec3(const aiVector3D& v) { return {v.x, v.y, v.z}; }
Color8 toColor8(const aiColor4D& c);
std::unique_ptr<aiScene> cloneScene(const aiScene& scene);

// Interpolated key value at time t (seconds); `absent` when there are no keys.
template <class Key, class V>
V sampleKeys(const Key* keys, unsigned count, double tps, double t, const V& absent)
{
    if (!count) {
        return absent;
    }
    const Key* next = std::lower_bound(keys + 1, keys + count, t, [tps](const Key& k, double time) { return k.mTime / tps < time; });
    if (next == keys + count) {
        return keys[count - 1].mValue;
    }
    const Key* prev = next - 1;
    const double t0 = prev->mTime / tps, t1 = next->mTime / tps;
    if (t <= t0 || t1 <= t0) {
        return prev->mValue;
    }
    const float f = float((t - t0) / (t1 - t0));
    if constexpr (std::is_same_v<V, aiQuaternion>) {
        aiQuaternion out;
        aiQuaternion::Interpolate(out, prev->mValue, next->mValue, f);
        out.Normalize();
        return out;
    } else {
        return prev->mValue + (next->mValue - prev->mValue) * f;
    }
}

// A base* node with a start* child: the shape the game reads detail levels from. When it is the scene root it
// is still a node (the importer keeps it), unlike an empty root.
bool isBaseStart(const aiNode* node);
// A child of `node` is a base*/start* pair.
bool lodAnchor(const aiNode* node);

// Per material: used by a mesh with vertex colors / by one without. Blender's glTF importer splits a material
// used both ways into "name" and "name.001".
void vertexColorUse(const aiScene& scene, std::vector<char>& colored, std::vector<char>& plain);

// aiString caps at 1 KB, so the JSON is split over "<key>.0", "<key>.1", ...
void putJsonMeta(aiMetadata*& meta, const char* key, const nlohmann::json& value);
// Looks on the scene, its root node and the root's direct children (glTF may wrap the root).
bool findJsonMeta(const aiScene& scene, const char* key, nlohmann::json& value);

} // namespace ts
