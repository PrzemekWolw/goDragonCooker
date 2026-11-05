#pragma once

#include <string>
#include <vector>

#include "model/ts/ts_shape.h"

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
    // Fold every aiAnimation into one "ambient" sequence (COLLADA without <animation_clip>).
    bool mergeAnimations = false;
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

} // namespace ts
