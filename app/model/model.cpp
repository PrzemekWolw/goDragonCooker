#include "model/model.h"

#include <fstream>
#include <map>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "core/platform.h"
#include "core/strings.h"
#include "model/formats.h"
#include "model/ts/ts_io.h"

namespace model {
namespace {

bool sameOptions(const ts::AiToTsOptions& a, const ts::AiToTsOptions& b)
{
    return a.lod == b.lod && a.singleDetailSize == b.singleDetailSize && a.scale == b.scale && a.mergeAnimations == b.mergeAnimations &&
           a.minFps == b.minFps && a.maxFps == b.maxFps && a.exporterVersion == b.exporterVersion;
}

bool sameOptions(const ts::TsToAiOptions& a, const ts::TsToAiOptions& b)
{
    return a.lodNames == b.lodNames && a.includeCollision == b.includeCollision && a.includeAnimations == b.includeAnimations;
}

enum class UpAxis { X, Y, Z };

// The engine's COLLADA up-axis conversion (Y_UP: (-x, z, y), X_UP: (z, x, y)) as a matrix.
aiMatrix4x4 upAxisMatrix(UpAxis up)
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

// Undoes writer renames recorded under kOriginalNameKey, in nodes, bones and animation channels.
void restoreOriginalNames(aiScene& scene)
{
    std::map<std::string, std::string> renamed;
    auto walk = [&](auto& self, aiNode* n) -> void {
        aiString original;
        if (n->mMetaData && n->mMetaData->Get(kOriginalNameKey, original) && original.length) {
            renamed[n->mName.C_Str()] = original.C_Str();
            n->mName = original;
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i]);
        }
    };
    walk(walk, scene.mRootNode);
    if (renamed.empty()) {
        return;
    }
    auto fix = [&](aiString& s) {
        auto it = renamed.find(s.C_Str());
        if (it != renamed.end()) {
            s = aiString(it->second);
        }
    };
    for (unsigned i = 0; i < scene.mNumMeshes; ++i) {
        for (unsigned b = 0; b < scene.mMeshes[i]->mNumBones; ++b) {
            fix(scene.mMeshes[i]->mBones[b]->mName);
        }
    }
    for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
        aiAnimation* anim = scene.mAnimations[a];
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            fix(anim->mChannels[c]->mNodeName);
        }
        for (unsigned c = 0; c < anim->mNumMorphMeshChannels; ++c) {
            fix(anim->mMorphMeshChannels[c]->mName);
        }
    }
}

} // namespace

Model::Model() = default;
Model::~Model() = default;

bool Model::load(const std::string& path, std::string& error, std::vector<std::string>& warnings)
{
    sourcePath = path;
    const std::string ext = modelExtension(path);
    if (ext == ".dts" || ext == ".cdae") {
        ts::Shape shape;
        if (!ts::readShapeFile(path, shape, error, &warnings)) {
            return false;
        }
        loadedShape = std::move(shape);
        fromTs = true;
        return true;
    }

    // COLLADA: keep the document's axes and apply the engine's up-axis conversion ourselves.
    UpAxis up = UpAxis::Z;
    const bool collada = ext == ".dae";
    if (collada) {
        std::ifstream f(fs::u8path(path), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const size_t p = text.find("<up_axis>");
        if (p != std::string::npos) {
            const std::string v = text.substr(p + 9, 16);
            up = v.find("Y_UP") != std::string::npos ? UpAxis::Y : v.find("X_UP") != std::string::npos ? UpAxis::X : UpAxis::Z;
        }
        mergeAnimations = text.find("<animation_clip") == std::string::npos;
    }

    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_IMPORT_COLLADA_IGNORE_UP_DIRECTION, collada ? 1 : 0);
    const unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType;
    if (!importer.ReadFile(path, flags)) {
        error = importer.GetErrorString();
        if (error.empty()) {
            error = "Assimp could not read the file";
        }
        return false;
    }
    loadedScene.reset(importer.GetOrphanedScene());
    if (!loadedScene || !loadedScene->mRootNode) {
        error = "empty scene";
        return false;
    }
    restoreOriginalNames(*loadedScene);
    if (collada && up != UpAxis::Z) {
        loadedScene->mRootNode->mTransformation = upAxisMatrix(up) * loadedScene->mRootNode->mTransformation;
        warnings.push_back(format("COLLADA %s converted to Z-up like the game", up == UpAxis::Y ? "Y_UP" : "X_UP"));
    }
    fromTs = false;
    return true;
}

const ts::Shape* Model::shape(const ts::AiToTsOptions& options, std::vector<std::string>& warnings, std::string& error)
{
    if (loadedShape) {
        return &*loadedShape;
    }
    if (!loadedScene) {
        error = "no model loaded";
        return nullptr;
    }
    if (convertedShape && sameOptions(options, convertedShapeOptions)) {
        warnings.insert(warnings.end(), convertedShapeWarnings.begin(), convertedShapeWarnings.end());
        return &*convertedShape;
    }
    ts::Shape shape;
    std::vector<std::string> w;
    if (!ts::assimpToShape(*loadedScene, options, shape, w, error)) {
        return nullptr;
    }
    convertedShape = std::move(shape);
    convertedShapeOptions = options;
    convertedShapeWarnings = w;
    warnings.insert(warnings.end(), w.begin(), w.end());
    return &*convertedShape;
}

const aiScene* Model::scene(const ts::TsToAiOptions& options, std::vector<std::string>& warnings)
{
    if (loadedScene) {
        return loadedScene.get();
    }
    if (!loadedShape) {
        return nullptr;
    }
    if (convertedScene && sameOptions(options, convertedSceneOptions)) {
        warnings.insert(warnings.end(), convertedSceneWarnings.begin(), convertedSceneWarnings.end());
        return convertedScene.get();
    }
    std::vector<std::string> w;
    convertedScene.reset(ts::shapeToAssimp(*loadedShape, options, w));
    convertedSceneOptions = options;
    convertedSceneWarnings = w;
    warnings.insert(warnings.end(), w.begin(), w.end());
    return convertedScene.get();
}

} // namespace model
