#pragma once

#include <nlohmann/json.hpp>

struct aiNode;
struct aiScene;
struct aiMetadata;

namespace ts {

// aiString caps at 1 KB, so the JSON is split over "<key>.0", "<key>.1", ...
void putJsonMeta(aiMetadata*& meta, const char* key, const nlohmann::json& value);
// Looks on the scene, its root node and the root's direct children (glTF may wrap the root).
bool findJsonMeta(const aiScene& scene, const char* key, nlohmann::json& value);

} // namespace ts
