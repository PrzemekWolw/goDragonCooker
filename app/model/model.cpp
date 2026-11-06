#include "model/model.h"

#include <fstream>
#include <map>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "core/platform.h"
#include "core/strings.h"
#include "model/axis.h"
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

// Assimp's scene copy stops at the first empty UV or colour set, so a mesh whose only set is TEXCOORD set="2"
// shares that array with the copy, which then frees it. Pack the used sets into 0..n-1.
template <typename Channels, typename Move>
std::vector<int> packChannels(Channels& channels, unsigned count, Move move)
{
    std::vector<int> remap(count, -1);
    unsigned next = 0;
    for (unsigned c = 0; c < count; ++c) {
        if (channels[c]) {
            remap[c] = int(next);
            if (next != c) {
                move(c, next);
            }
            ++next;
        }
    }
    return remap;
}

void compactChannels(aiScene& scene)
{
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        aiMesh* mesh = scene.mMeshes[m];
        const std::vector<int> uvMap = packChannels(mesh->mTextureCoords, AI_MAX_NUMBER_OF_TEXTURECOORDS, [&](unsigned from, unsigned to) {
            mesh->mTextureCoords[to] = mesh->mTextureCoords[from];
            mesh->mTextureCoords[from] = nullptr;
            mesh->mNumUVComponents[to] = mesh->mNumUVComponents[from];
            mesh->mNumUVComponents[from] = 0;
            if (mesh->mTextureCoordsNames) {
                std::swap(mesh->mTextureCoordsNames[to], mesh->mTextureCoordsNames[from]);
            }
        });
        packChannels(mesh->mColors, AI_MAX_NUMBER_OF_COLOR_SETS, [&](unsigned from, unsigned to) {
            mesh->mColors[to] = mesh->mColors[from];
            mesh->mColors[from] = nullptr;
        });
        for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a) {
            aiAnimMesh* am = mesh->mAnimMeshes[a];
            packChannels(am->mTextureCoords, AI_MAX_NUMBER_OF_TEXTURECOORDS, [&](unsigned from, unsigned to) {
                am->mTextureCoords[to] = am->mTextureCoords[from];
                am->mTextureCoords[from] = nullptr;
            });
            packChannels(am->mColors, AI_MAX_NUMBER_OF_COLOR_SETS, [&](unsigned from, unsigned to) {
                am->mColors[to] = am->mColors[from];
                am->mColors[from] = nullptr;
            });
        }
        if (mesh->mMaterialIndex >= scene.mNumMaterials) {
            continue;
        }
        aiMaterial* mat = scene.mMaterials[mesh->mMaterialIndex];
        for (unsigned p = 0; p < mat->mNumProperties; ++p) {
            aiMaterialProperty* prop = mat->mProperties[p];
            if (prop->mType == aiPTI_Integer && prop->mDataLength >= sizeof(int) && std::strcmp(prop->mKey.C_Str(), "$tex.uvwsrc") == 0) {
                int* v = reinterpret_cast<int*>(prop->mData);
                if (*v >= 0 && unsigned(*v) < uvMap.size() && uvMap[*v] >= 0) {
                    *v = uvMap[*v];
                }
            }
        }
    }
}

// The game's COLLADA loader shrugs off a few exporter mistakes that Assimp's strict XML parser rejects.
// Fixes them in place and returns a description of each repair.
std::vector<std::string> repairCollada(std::string& text)
{
    std::vector<std::string> fixes;

    // Empty or missing attributes in the XML declaration (<?xml version="" encoding=""?>).
    if (text.compare(0, 5, "<?xml") == 0) {
        const size_t end = text.find("?>");
        if (end != std::string::npos) {
            const std::string decl = text.substr(0, end + 2);
            if (decl.find("version=\"\"") != std::string::npos || decl.find("encoding=\"\"") != std::string::npos) {
                text.replace(0, end + 2, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
                fixes.push_back("empty XML declaration");
            }
        }
    }

    // Further declarations inside the document (a second <?xml ...?> pasted in by a merge).
    size_t stray = 1;
    bool strayFound = false;
    while ((stray = text.find("<?xml", stray)) != std::string::npos) {
        const size_t end = text.find("?>", stray);
        if (end == std::string::npos) {
            break;
        }
        text.erase(stray, end + 2 - stray);
        strayFound = true;
    }
    if (strayFound) {
        fixes.push_back("removed stray XML declaration");
    }

    // <instance_visual_scene url="#X"/> naming a scene that does not exist: use the file's first visual_scene.
    const size_t inst = text.find("<instance_visual_scene");
    if (inst != std::string::npos) {
        const size_t q1 = text.find("url=\"#", inst);
        const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 6);
        if (q2 != std::string::npos) {
            const std::string id = text.substr(q1 + 6, q2 - q1 - 6);
            if (text.find("<visual_scene id=\"" + id + "\"") == std::string::npos) {
                const size_t vs = text.find("<visual_scene id=\"");
                const size_t ve = vs == std::string::npos ? vs : text.find('"', vs + 18);
                if (ve != std::string::npos) {
                    const std::string actual = text.substr(vs + 18, ve - vs - 18);
                    text.replace(q1 + 6, q2 - q1 - 6, actual);
                    fixes.push_back("scene reference #" + id + " pointed at nothing, using #" + actual);
                }
            }
        }
    }
    return fixes;
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
        upNote = "Z-up (game)";
        return true;
    }

    // COLLADA: keep the document's axes and apply the engine's up-axis conversion ourselves.
    UpAxis up = UpAxis::Z;
    const bool collada = ext == ".dae";
    std::string repaired;
    if (collada) {
        std::ifstream f(fs::u8path(path), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::vector<std::string> fixes = repairCollada(text);
        if (!fixes.empty()) {
            for (const std::string& fix : fixes) {
                warnings.push_back("COLLADA file repaired: " + fix);
            }
            repaired = text;
        }
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
    const aiScene* imported = repaired.empty() ? importer.ReadFile(path, flags)
                                               : importer.ReadFileFromMemory(repaired.data(), repaired.size(), flags, "dae");
    if (!imported) {
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
    compactChannels(*loadedScene);
    if (collada) {
        if (up != UpAxis::Z) {
            loadedScene->mRootNode->mTransformation = colladaUpToZUp(up) * loadedScene->mRootNode->mTransformation;
            warnings.push_back(format("COLLADA %s converted to Z-up like the game", up == UpAxis::Y ? "Y_UP" : "X_UP"));
        }
        upNote = format("%s-up (<up_axis>) -> Z-up", upAxisName(up));
    } else {
        // Assimp scenes are Y up unless the format is natively Z up; the game and the TS writers are Z up.
        const UpAxis frame = importedUp(ext);
        if (frame == UpAxis::Y) {
            loadedScene->mRootNode->mTransformation = yUpToZUp() * loadedScene->mRootNode->mTransformation;
        }
        upNote = format("%s-up (%s) -> Z-up", upAxisName(frame), ext.c_str());
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
