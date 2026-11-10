#pragma once

#include <string>

namespace model {

// The Assimp glTF exporter always writes the scene root as node 0. When that node is an empty, untransformed
// container that is the scene's only root (and not a base*/start* detail node, or a node a skin or animation
// targets), rewrites the glTF 2.0 file (.glb or .gltf) in place so its children are the scene's root nodes,
// like Blender writes them.
bool gltfDropEmptyRoot(const std::string& path, std::string& error);

} // namespace model
