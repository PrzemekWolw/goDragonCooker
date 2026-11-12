#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "model/assimp/ai_bridge.h"
#include "model/ts/ts_shape.h"

struct aiNode;
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
    int object = -1;   // index into Summary::objects
    int group = 0;     // detail size when summarizing per detail
};

struct Influence {
    std::string bone;
    float weight = 0;
};

// Nodes and Assimp mesh holders (mesh-only leaves) in one list; the TS builder makes both shape nodes.
struct NodePose {
    std::string name;
    std::string parent; // nearest ancestor in Summary::nodes, empty for a top-level node
    ts::Mat4 world;     // scale-free (rigid) part
    bool meshHolder = false;
    bool mirrored = false; // negative determinant; the TS builder splits the mirror off the rotation
};

struct ObjectInfo {
    std::string name;
    std::string node; // owning node, empty for skins
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
    std::vector<ObjectInfo> objects;
    std::vector<std::pair<std::string, float>> details; // Assimp: trailing-number sizes, no names
    bool tsObjects = false; // objects carry TS object names (TS shape, or a scene read the way the TS builder reads it)
    std::vector<std::pair<std::string, std::string>> renamed; // TS uniqueness renames of a repeated name
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

// How an Assimp scene's names are read. Raw: mesh node names, as the game's Assimp loader sees them.
// TS: objects, detail sizes and unique node names as assimpToShape assigns them (for comparing with a TSShape).
struct SceneNames {
    bool tsNames = false;
    ts::AiToTsOptions options;
    // Format::meshNameOf of the format the scene was read from: a mesh-only leaf whose name decodes to its parent's
    // name gives its meshes to the parent (3DS writes "<node>_<mesh>_<index>" children).
    std::function<std::string(const std::string&, const std::string&)> meshNameOf;
    // Generated nodes of a TS export written to a format that drops the gdc_tsshape metadata.
    std::vector<std::string> collapse;
};

Summary summarize(const ts::Shape& shape, const DetailFilter& filter);
Summary summarize(const aiScene& scene, const SceneNames& names = {});

} // namespace model
