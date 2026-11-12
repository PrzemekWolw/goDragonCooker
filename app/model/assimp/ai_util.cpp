#include "model/assimp/ai_util.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <assimp/SceneCombiner.h>

#include "core/strings.h"

namespace ts {

Mat4 toMat4(const aiMatrix4x4& a)
{
    Mat4 m;
    const float src[16] = {a.a1, a.a2, a.a3, a.a4, a.b1, a.b2, a.b3, a.b4, a.c1, a.c2, a.c3, a.c4, a.d1, a.d2, a.d3, a.d4};
    std::memcpy(m.m, src, sizeof(src));
    return m;
}

aiMatrix4x4 toAi(const Mat4& m)
{
    return aiMatrix4x4(m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5], m.m[6], m.m[7], m.m[8], m.m[9], m.m[10], m.m[11], m.m[12],
                       m.m[13], m.m[14], m.m[15]);
}

Color8 toColor8(const aiColor4D& c)
{
    auto b = [](float x) { return uint8_t(std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f)); };
    return {b(c.r), b(c.g), b(c.b), b(c.a)};
}

std::unique_ptr<aiScene> cloneScene(const aiScene& scene)
{
    aiScene* copy = nullptr;
    Assimp::SceneCombiner::CopyScene(&copy, &scene);
    return std::unique_ptr<aiScene>(copy);
}

bool isBaseStart(const aiNode* node)
{
    if (!node || !hasPrefixI(node->mName.C_Str(), "base")) {
        return false;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (hasPrefixI(node->mChildren[i]->mName.C_Str(), "start")) {
            return true;
        }
    }
    return false;
}

bool lodAnchor(const aiNode* node)
{
    for (unsigned i = 0; node && i < node->mNumChildren; ++i) {
        if (isBaseStart(node->mChildren[i])) {
            return true;
        }
    }
    return false;
}

void vertexColorUse(const aiScene& scene, std::vector<char>& colored, std::vector<char>& plain)
{
    colored.assign(scene.mNumMaterials, 0);
    plain.assign(scene.mNumMaterials, 0);
    for (unsigned i = 0; i < scene.mNumMeshes; ++i) {
        const aiMesh* m = scene.mMeshes[i];
        if (m->mMaterialIndex < scene.mNumMaterials) {
            (m->HasVertexColors(0) ? colored : plain)[m->mMaterialIndex] = 1;
        }
    }
}

namespace {

constexpr size_t kChunk = 1000;

bool readChunks(const aiMetadata* meta, const char* key, std::string& out)
{
    if (!meta) {
        return false;
    }
    out.clear();
    for (int i = 0;; ++i) {
        aiString part;
        if (!meta->Get(std::string(key) + "." + std::to_string(i), part)) {
            return i > 0;
        }
        out.append(part.C_Str(), part.length);
    }
}

} // namespace

void putJsonMeta(aiMetadata*& meta, const char* key, const nlohmann::json& value)
{
    const std::string text = value.dump();
    if (!meta) {
        meta = new aiMetadata;
    }
    for (size_t off = 0, i = 0; off < text.size() || i == 0; off += kChunk, ++i) {
        meta->Add(std::string(key) + "." + std::to_string(i), aiString(text.substr(off, kChunk)));
    }
}

bool findJsonMeta(const aiScene& scene, const char* key, nlohmann::json& value)
{
    std::vector<const aiMetadata*> places{scene.mMetaData};
    if (scene.mRootNode) {
        places.push_back(scene.mRootNode->mMetaData);
        for (unsigned i = 0; i < scene.mRootNode->mNumChildren; ++i) {
            places.push_back(scene.mRootNode->mChildren[i]->mMetaData);
        }
    }
    std::string text;
    for (const aiMetadata* meta : places) {
        if (readChunks(meta, key, text)) {
            value = nlohmann::json::parse(text, nullptr, false);
            if (!value.is_discarded()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace ts
