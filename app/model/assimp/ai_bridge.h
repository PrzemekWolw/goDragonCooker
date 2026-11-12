#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "model/ts/ts_shape.h"

struct aiNode;
struct aiScene;

// Conversion between Assimp scenes and TSShapes. Coordinates map one to one (the game's Assimp
// loader uses aiScene space as shape space); winding is reversed and V is flipped, matching the
// FlipWindingOrder | FlipUVs post-process the game applies.
namespace ts {

enum class LodMode {
    Auto,           // trailing-number LODs when the scene has the base*/start* layout, else one detail
    TrailingNumber, // "body200" -> object "body" at detail 200, "Collision-1" -> collision detail
    SingleDetail,   // every mesh node is one object at singleDetailSize
};

enum class ScaleMode { Auto, Uniform, Aligned, Arbitrary };

struct AiToTsOptions {
    LodMode lod = LodMode::Auto;
    int singleDetailSize = 2;
    ScaleMode scale = ScaleMode::Auto;
    int minFps = 15;
    int maxFps = 60;
    int exporterVersion = kExporterCollada;
};

bool assimpToShape(const aiScene& scene, const AiToTsOptions& options, Shape& shape, std::vector<std::string>& warnings,
                   std::string& error);

struct TsToAiOptions {
    // Name objects with their detail size (body200, Colmesh-1) and add base01/start01 so a
    // trailing-number import restores the LODs; off writes only the highest visible detail.
    bool lodNames = true;
    bool includeCollision = true;
    bool includeAnimations = true;
};

// The returned scene is owned by the caller (delete with `delete`).
aiScene* shapeToAssimp(const Shape& shape, const TsToAiOptions& options, std::vector<std::string>& warnings);

// Scene/root metadata key holding TSShape-only data (details, material flags, sequence flags,
// triggers) as JSON so DTS -> glTF -> DTS keeps them where the format preserves metadata.
constexpr const char* kTsMetadataKey = "gdc_tsshape";

// Parses "name123" / "name-1": returns the base and sets size when a trailing number exists.
std::string splitTrailingNumber(const std::string& name, int& size);

// The TS object and detail size each mesh node of a scene becomes, as assimpToShape assigns them.
struct MeshNodeObject {
    const aiNode* node = nullptr;
    std::string object; // after the uniqueness suffix
    int size = 0;
};

struct ObjectNaming {
    bool trailing = false; // detail sizes come from trailing numbers
    std::vector<MeshNodeObject> meshNodes; // pre-order, the bounds node excluded
    std::vector<std::pair<std::string, std::string>> renamed; // derived name -> unique name (a second mesh at a taken slot)
    std::vector<std::string> collapse; // generated nodes from gdc_tsshape metadata, not shape nodes
    const aiNode* boundsNode = nullptr;
};

ObjectNaming objectNaming(const aiScene& scene, const AiToTsOptions& options);

// TS node and object names are unique (case-insensitive); a repeat gets B, C, ... appended.
// keySuffix scopes uniqueness, e.g. per detail size.
std::string uniqueName(const std::string& base, std::unordered_set<std::string>& taken, const std::string& keySuffix = "");

// A node listed in `collapse` that the TS export generated: a mesh-only leaf holder or the base01/start01 wrapper.
// A shape node may share a holder's name, so the name alone does not decide.
bool generatedNode(const aiNode& node, const std::unordered_set<std::string>& collapse);

} // namespace ts
