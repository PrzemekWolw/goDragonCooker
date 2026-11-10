#include "model/axis.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "core/platform.h"
#include "core/strings.h"
#include "model/formats.h"
#include "model/validate/summary.h"

namespace model {

const char* upAxisName(UpAxis up)
{
    switch (up) {
    case UpAxis::X:
        return "X";
    case UpAxis::Y:
        return "Y";
    default:
        return "Z";
    }
}

aiMatrix4x4 zUpToYUp() { return aiMatrix4x4(1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1); }

aiMatrix4x4 yUpToZUp() { return aiMatrix4x4(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1); }

aiMatrix4x4 colladaUpToZUp(UpAxis up)
{
    switch (up) {
    case UpAxis::Y:
        return aiMatrix4x4(-1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    case UpAxis::X:
        return aiMatrix4x4(0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    default:
        return aiMatrix4x4();
    }
}

void rotateScene(aiScene& scene, const aiMatrix4x4& rot)
{
    const aiMatrix3x3 r3(rot);
    aiMatrix4x4 inv = rot;
    inv.Inverse();
    const auto rotateAll = [&](aiVector3D* v, unsigned count) {
        for (unsigned i = 0; v && i < count; ++i) {
            v[i] = r3 * v[i];
        }
    };
    auto nodes = [&](auto& self, aiNode* n) -> void {
        n->mTransformation = rot * n->mTransformation * inv;
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i]);
        }
    };
    nodes(nodes, scene.mRootNode);
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        aiMesh* mesh = scene.mMeshes[m];
        rotateAll(mesh->mVertices, mesh->mNumVertices);
        rotateAll(mesh->mNormals, mesh->mNumVertices);
        rotateAll(mesh->mTangents, mesh->mNumVertices);
        rotateAll(mesh->mBitangents, mesh->mNumVertices);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            mesh->mBones[b]->mOffsetMatrix = rot * mesh->mBones[b]->mOffsetMatrix * inv;
        }
        for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a) {
            aiAnimMesh* am = mesh->mAnimMeshes[a];
            rotateAll(am->mVertices, am->mNumVertices);
            rotateAll(am->mNormals, am->mNumVertices);
            rotateAll(am->mTangents, am->mNumVertices);
            rotateAll(am->mBitangents, am->mNumVertices);
        }
    }
    const auto absRow = [&](const aiVector3D& s, unsigned row) {
        return std::abs(r3[row][0]) * s.x + std::abs(r3[row][1]) * s.y + std::abs(r3[row][2]) * s.z;
    };
    for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
        for (unsigned c = 0; c < scene.mAnimations[a]->mNumChannels; ++c) {
            aiNodeAnim* ch = scene.mAnimations[a]->mChannels[c];
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k) {
                ch->mPositionKeys[k].mValue = r3 * ch->mPositionKeys[k].mValue;
            }
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k) {
                ch->mRotationKeys[k].mValue = aiQuaternion(r3 * ch->mRotationKeys[k].mValue.GetMatrix() * aiMatrix3x3(inv));
            }
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k) {
                const aiVector3D s = ch->mScalingKeys[k].mValue;
                ch->mScalingKeys[k].mValue = aiVector3D(absRow(s, 0), absRow(s, 1), absRow(s, 2));
            }
        }
    }
}

UpAxis importedUp(const std::string& ext)
{
    static const std::set<std::string> zUp = {".stl", ".ply", ".3mf", ".smd", ".vta"};
    return zUp.count(toLower(ext)) ? UpAxis::Z : UpAxis::Y;
}

WriterFrame writerFrame(const std::string& formatId)
{
    // Z-up formats keep game coordinates. COLLADA writes its own <up_axis>Z_UP</up_axis>.
    static const std::set<std::string> zUp = {"collada", "stl", "ply", "3ds", "3mf", "stp"};
    WriterFrame f;
    f.up = zUp.count(formatId) ? UpAxis::Z : UpAxis::Y;
    return f;
}

namespace {

std::optional<UpAxis> colladaDeclared(const std::string& path)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t p = text.find("<up_axis>");
    if (p == std::string::npos) {
        return UpAxis::Y; // COLLADA's default when the element is missing
    }
    const std::string v = text.substr(p + 9, 8);
    return v.find('X') == 0 ? UpAxis::X : v.find('Y') == 0 ? UpAxis::Y : UpAxis::Z;
}

} // namespace

RawFrame readRawFrame(const std::string& path)
{
    RawFrame r;
    const std::string ext = modelExtension(path);
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_COLLADA_IGNORE_UP_DIRECTION, true);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, true);
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_SortByPType);
    if (!scene || !scene->mRootNode) {
        r.error = importer.GetErrorString();
        return r;
    }
    if (ext == ".dae") {
        r.declared = colladaDeclared(path);
    } else if (ext == ".fbx" && scene->mMetaData) {
        int32_t axis = 0, sign = 1;
        if (scene->mMetaData->Get("UpAxis", axis)) {
            r.declared = axis == 0 ? UpAxis::X : axis == 1 ? UpAxis::Y : UpAxis::Z;
            scene->mMetaData->Get("UpAxisSign", sign);
            r.declaredNegative = sign < 0;
        }
    } else if (ext == ".3ds") {
        // Assimp's 3DS reader turns the file's Z-up into Y-up on its own.
        aiMatrix4x4& root = const_cast<aiMatrix4x4&>(scene->mRootNode->mTransformation);
        root = yUpToZUp() * root;
    }
    const Summary s = summarize(*scene);
    r.hasGeometry = !s.tris.empty();
    r.bounds = s.bounds;
    r.ok = true;
    return r;
}

} // namespace model
