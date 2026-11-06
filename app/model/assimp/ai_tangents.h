#pragma once

#include <vector>

#include <assimp/scene.h>

namespace model {

bool gameTangents(const aiMesh& mesh, std::vector<aiVector3D>& tangents, std::vector<float>& handedness);

unsigned addGameTangents(aiScene& scene);

} // namespace model
