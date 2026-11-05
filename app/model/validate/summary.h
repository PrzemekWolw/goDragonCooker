#pragma once

#include <string>
#include <utility>
#include <vector>

#include "model/ts/ts_shape.h"

struct aiScene;

// A format-neutral picture of a model for validation: world-space triangles with per-corner
// attributes in TSShape conventions (V down, clockwise front faces), the default pose, skin
// influences by bone name and sampled animation.
namespace model {

struct Corner {
    ts::Vec3 pos;
    ts::Vec3 nrm;
    ts::Vec2 uv0, uv1;
    ts::Color8 color;
    ts::Vec4 tangent{1, 0, 0, 1};
    int skin = -1; // index into Summary::influences
};

struct Tri {
    Corner c[3];
    int material = -1; // index into Summary::materials
    int group = 0;     // detail size when summarizing per detail
};

struct Influence {
    std::string bone;
    float weight = 0;
};

struct NodePose {
    std::string name;
    ts::Mat4 world; // scale-free (rigid) part
};

struct AnimSample {
    std::string name;
    float duration = 0;
    int keyframes = 0;
    std::vector<std::string> nodes;          // nodes that move in this animation
    std::vector<std::vector<ts::Mat4>> rigid; // per node, kSamples rigid world transforms
};

struct Summary {
    std::vector<NodePose> nodes;
    std::vector<std::string> meshHolders; // Assimp mesh-only leaf nodes, kept out of `nodes`
    std::vector<std::string> objects;
    std::vector<std::pair<std::string, float>> details;
    std::vector<std::string> materials;
    std::vector<Tri> tris;
    std::vector<std::vector<Influence>> influences;
    std::vector<AnimSample> anims;
    bool hasNormals = false, hasUv0 = false, hasUv1 = false, hasColors = false, hasTangents = false;
    int skinnedMeshes = 0;
    int morphTargets = 0;
    size_t vertexCount = 0;
    size_t nanValues = 0;
    size_t degenerate = 0;
    ts::Box bounds;
    float diagonal() const { return ts::length(bounds.max - bounds.min); }
};

constexpr int kAnimSamples = 16;

struct DetailFilter {
    bool perDetail = false;   // group triangles by detail size (TS <-> TS)
    bool allDetails = true;   // otherwise the highest visible detail only
    bool collision = true;    // include negative-size details
};

Summary summarize(const ts::Shape& shape, const DetailFilter& filter);
Summary summarize(const aiScene& scene);

} // namespace model
