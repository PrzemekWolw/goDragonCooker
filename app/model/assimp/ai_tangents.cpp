#include "model/assimp/ai_tangents.h"

#include <cmath>

namespace model {

bool gameTangents(const aiMesh& mesh, std::vector<aiVector3D>& tangents, std::vector<float>& handedness)
{
    if (!mesh.mNormals || !mesh.HasTextureCoords(0) || !mesh.mNumFaces) {
        return false;
    }
    const unsigned count = mesh.mNumVertices;
    const aiVector3D* uv = mesh.mTextureCoords[0];
    std::vector<aiVector3D> sdir(count), tdir(count);
    for (unsigned f = 0; f < mesh.mNumFaces; ++f) {
        const aiFace& face = mesh.mFaces[f];
        if (face.mNumIndices != 3) {
            continue;
        }
        const unsigned a = face.mIndices[0], b = face.mIndices[1], c = face.mIndices[2];
        const aiVector3D e1 = mesh.mVertices[b] - mesh.mVertices[a];
        const aiVector3D e2 = mesh.mVertices[c] - mesh.mVertices[a];
        const float s1 = uv[b].x - uv[a].x, s2 = uv[c].x - uv[a].x;
        const float t1 = uv[a].y - uv[b].y, t2 = uv[a].y - uv[c].y;
        float denom = s1 * t2 - s2 * t1;
        if (std::fabs(denom) < 1e-4f) {
            denom = denom < 0 ? -1e-4f : 1e-4f;
        }
        const float r = 1.0f / denom;
        const aiVector3D s = (e1 * t2 - e2 * t1) * r;
        const aiVector3D t = (e2 * s1 - e1 * s2) * r;
        for (unsigned v : {a, b, c}) {
            sdir[v] += s;
            tdir[v] += t;
        }
    }
    tangents.resize(count);
    handedness.resize(count);
    for (unsigned v = 0; v < count; ++v) {
        const aiVector3D& n = mesh.mNormals[v];
        aiVector3D t = sdir[v] - n * (n * sdir[v]);
        const float length = t.Length();
        if (length < 1e-12f) {
            t = std::fabs(n.x) < 0.9f ? aiVector3D(1, 0, 0) : aiVector3D(0, 1, 0);
            t = (t - n * (n * t)).NormalizeSafe();
            handedness[v] = 0;
        } else {
            t /= length;
            const aiVector3D side = n ^ sdir[v];
            const float norm = side.Length() * tdir[v].Length();
            handedness[v] = norm > 0.0f ? (side * tdir[v]) / norm : 0.0f;
        }
        tangents[v] = t;
    }
    return true;
}

unsigned addGameTangents(aiScene& scene)
{
    unsigned changed = 0;
    std::vector<aiVector3D> tangents;
    std::vector<float> handedness;
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        aiMesh& mesh = *scene.mMeshes[m];
        if (mesh.mTangents || mesh.mBitangents || !gameTangents(mesh, tangents, handedness)) {
            continue;
        }
        mesh.mTangents = new aiVector3D[mesh.mNumVertices];
        mesh.mBitangents = new aiVector3D[mesh.mNumVertices];
        for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
            mesh.mTangents[v] = tangents[v];
            mesh.mBitangents[v] = (mesh.mNormals[v] ^ tangents[v]) * (handedness[v] < 0 ? -1.0f : 1.0f);
        }
        ++changed;
    }
    return changed;
}

} // namespace model
