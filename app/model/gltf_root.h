#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace model {

// A .gltf or .glb file: its JSON and, for a GLB, the binary chunk.
struct GltfFile {
    nlohmann::json json;
    std::vector<uint8_t> bin;
    bool glb = false;
};

bool readGltfFile(const std::string& path, GltfFile& file, std::string& error);

// Writes .gltf text, or a GLB with `bin` as its binary chunk (none when empty).
bool writeGltfFile(const std::string& path, const GltfFile& file, std::string& error);

// Rewrites a written glTF 2.0 file (.glb or .gltf) in place. Drops Assimp's empty scene root when node 0 is an
// untransformed container that is the scene's only root (and not a base*/start* detail node, or a node a skin
// or animation targets), so its children are the scene roots, like Blender writes them. Also writes a node's
// original name back from extras.gdc_name: the exporter shares one id for materials, meshes and nodes and
// renames a clash "name_node", and the game looks the mesh up by that node name.
bool gltfDropEmptyRoot(const std::string& path, std::string& error);

} // namespace model
