#include "model/assimp/ai_meta.h"

#include <string>
#include <vector>

#include <assimp/scene.h>

namespace ts {
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
