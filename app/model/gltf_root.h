#pragma once

#include <string>

namespace model {

// Rewrites a written glTF 2.0 file (.glb or .gltf) in place. Drops Assimp's empty scene root when node 0 is an
// untransformed container that is the scene's only root (and not a base*/start* detail node, or a node a skin
// or animation targets), so its children are the scene roots, like Blender writes them. Also writes a node's
// original name back from extras.gdc_name: the exporter shares one id for materials, meshes and nodes and
// renames a clash "name_node", and the game looks the mesh up by that node name.
bool gltfDropEmptyRoot(const std::string& path, std::string& error);

} // namespace model
