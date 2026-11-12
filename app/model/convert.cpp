#include "model/convert.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>
#include <vector>

#include <assimp/Exporter.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/strings.h"
#include "model/assimp/ai_util.h"
#include "model/axis.h"
#include "model/draco/draco_gltf.h"
#include "model/gltf_root.h"
#include "model/ts/ts_io.h"
#include "model/validate/summary.h"

namespace model {
namespace {

using ts::cloneScene;
using ts::isBaseStart;
using ts::lodAnchor;
using ScenePtr = std::unique_ptr<aiScene>;

// The COLLADA exporter writes key times as seconds without applying mTicksPerSecond.
void animationsToSeconds(aiScene& scene)
{
    for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
        aiAnimation* anim = scene.mAnimations[a];
        const double tps = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0;
        if (tps == 1.0) {
            continue;
        }
        auto scale = [tps](auto* keys, unsigned count) {
            for (unsigned k = 0; k < count; ++k) {
                keys[k].mTime /= tps;
            }
        };
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            aiNodeAnim* ch = anim->mChannels[c];
            scale(ch->mPositionKeys, ch->mNumPositionKeys);
            scale(ch->mRotationKeys, ch->mNumRotationKeys);
            scale(ch->mScalingKeys, ch->mNumScalingKeys);
        }
        for (unsigned c = 0; c < anim->mNumMorphMeshChannels; ++c) {
            scale(anim->mMorphMeshChannels[c]->mKeys, anim->mMorphMeshChannels[c]->mNumKeys);
        }
        anim->mDuration /= tps;
        anim->mTicksPerSecond = 1.0;
    }
}

// Puts a new root above the scene's root. Metadata lives on the scene root; it moves where the reader looks.
void wrapRoot(aiScene& scene, const char* name, const aiMatrix4x4& transform)
{
    aiNode* root = scene.mRootNode;
    aiNode* top = new aiNode(name);
    top->mTransformation = transform;
    top->mChildren = new aiNode*[1]{root};
    top->mNumChildren = 1;
    root->mParent = top;
    scene.mRootNode = top;
    std::swap(top->mMetaData, root->mMetaData);
}

// The COLLADA exporter writes <up_axis>Z_UP</up_axis> and drops the root when the root holds exactly the
// Z-up-to-Y-up rotation, so the file keeps the game's Z-up coordinates unchanged.
ScenePtr colladaScene(const aiScene& in)
{
    ScenePtr copy = cloneScene(in);
    animationsToSeconds(*copy);
    aiNode* root = copy->mRootNode;
    if (root->mTransformation.IsIdentity() && root->mNumMeshes == 0 && root->mNumChildren > 0) {
        root->mTransformation = zUpToYUp();
    } else {
        wrapRoot(*copy, "Scene", zUpToYUp());
    }
    return copy;
}

// The glTF exporters give materials, meshes and nodes one id namespace and rename clashing nodes
// ("x" -> "x_node"). The original name goes into the node's extras; gltfDropEmptyRoot writes it back
// into the file, and Model::load restores it when reading a file that still has the renamed node.
ScenePtr gltfScene(const aiScene& in)
{
    ScenePtr copy = cloneScene(in);
    std::set<std::string> taken;
    for (unsigned i = 0; i < copy->mNumMaterials; ++i) {
        aiString name;
        if (copy->mMaterials[i]->Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS) {
            taken.insert(name.C_Str());
        }
    }
    for (unsigned i = 0; i < copy->mNumMeshes; ++i) {
        taken.insert(copy->mMeshes[i]->mName.C_Str());
    }
    auto walk = [&](auto& self, aiNode* n) -> void {
        const std::string name = n->mName.C_Str();
        if (!name.empty() && !taken.insert(name).second) {
            if (!n->mMetaData) {
                n->mMetaData = new aiMetadata();
            }
            n->mMetaData->Add(kOriginalNameKey, aiString(name));
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i]);
        }
    };
    walk(walk, copy->mRootNode);
    // A material used with and without vertex colors would split on import; give the plain uses white colors.
    std::vector<char> colored, plain;
    ts::vertexColorUse(*copy, colored, plain);
    for (unsigned i = 0; i < copy->mNumMeshes; ++i) {
        aiMesh* m = copy->mMeshes[i];
        if (m->mMaterialIndex >= copy->mNumMaterials || m->HasVertexColors(0)) {
            continue;
        }
        if (colored[m->mMaterialIndex] && plain[m->mMaterialIndex]) {
            m->mColors[0] = new aiColor4D[m->mNumVertices];
            std::fill(m->mColors[0], m->mColors[0] + m->mNumVertices, aiColor4D(1, 1, 1, 1));
        }
    }
    return copy;
}

bool baseOrStart(const aiNode* node) { return hasPrefixI(node->mName.C_Str(), "base") || hasPrefixI(node->mName.C_Str(), "start"); }

std::set<std::string> referencedNodeNames(const aiScene& scene)
{
    std::set<std::string> names;
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        const aiMesh* mesh = scene.mMeshes[m];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            names.insert(mesh->mBones[b]->mName.C_Str());
        }
    }
    for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
        const aiAnimation* anim = scene.mAnimations[a];
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            names.insert(anim->mChannels[c]->mNodeName.C_Str());
        }
    }
    return names;
}

bool collapseLodWrapper(aiNode* parent, const std::set<std::string>& referenced, std::vector<std::string>& folded)
{
    std::vector<aiNode*> kept;
    bool changed = false;
    for (unsigned i = 0; i < parent->mNumChildren; ++i) {
        aiNode* child = parent->mChildren[i];
        const bool wrapper = !child->mNumMeshes && child->mNumChildren && lodAnchor(child) && !baseOrStart(child) &&
                             !referenced.count(child->mName.C_Str());
        if (!wrapper) {
            kept.push_back(child);
            continue;
        }
        folded.push_back(child->mName.C_Str());
        const aiMatrix4x4 xf = child->mTransformation;
        const unsigned n = child->mNumChildren;
        aiNode** grand = child->mChildren;
        child->mChildren = nullptr;
        child->mNumChildren = 0;
        for (unsigned g = 0; g < n; ++g) {
            grand[g]->mParent = parent;
            grand[g]->mTransformation = xf * grand[g]->mTransformation;
            kept.push_back(grand[g]);
        }
        delete[] grand;
        delete child;
        changed = true;
    }
    if (!changed) {
        return false;
    }
    delete[] parent->mChildren;
    parent->mNumChildren = unsigned(kept.size());
    parent->mChildren = kept.empty() ? nullptr : new aiNode*[kept.size()];
    for (size_t i = 0; i < kept.size(); ++i) {
        parent->mChildren[i] = kept[i];
        kept[i]->mParent = parent;
    }
    return true;
}

bool lodAnchorNeeded(const aiScene& scene)
{
    const aiNode* root = scene.mRootNode;
    if (!root || lodAnchor(root) || isBaseStart(root)) {
        return false;
    }
    for (unsigned i = 0; i < root->mNumChildren; ++i) {
        const aiNode* child = root->mChildren[i];
        if (!child->mNumMeshes && lodAnchor(child) && !baseOrStart(child)) {
            return true;
        }
    }
    return false;
}

void anchorGameLod(aiScene& scene, ConvertResult& r)
{
    aiNode* root = scene.mRootNode;
    if (!root) {
        return;
    }
    const std::set<std::string> referenced = referencedNodeNames(scene);
    int folded = 0;
    while (!lodAnchor(root) && !isBaseStart(root) && collapseLodWrapper(root, referenced, r.foldedNodes)) {
        ++folded;
    }
    if (folded) {
        r.log.push_back(format(tr("gdc.model.note.lodFold"), folded));
    }
}

// The FBX exporter merges all meshes of a node into one geometry and writes UV and color layers
// per polygon vertex, so a mesh missing a channel the others have shifts every later value.
ScenePtr fbxScene(const aiScene& in)
{
    ScenePtr copy = cloneScene(in);
    aiMesh** meshes = copy->mMeshes;
    auto walk = [&](auto& self, aiNode* n) -> void {
        if (n->mNumMeshes > 1) {
            unsigned uvs = 0;
            bool colors = false;
            for (unsigned i = 0; i < n->mNumMeshes; ++i) {
                const aiMesh* m = meshes[n->mMeshes[i]];
                uvs = std::max(uvs, m->GetNumUVChannels());
                colors = colors || m->HasVertexColors(0);
            }
            for (unsigned i = 0; i < n->mNumMeshes; ++i) {
                aiMesh* m = meshes[n->mMeshes[i]];
                for (unsigned c = 0; c < uvs; ++c) {
                    if (!m->mTextureCoords[c]) {
                        m->mTextureCoords[c] = new aiVector3D[m->mNumVertices]();
                        m->mNumUVComponents[c] = 2;
                    }
                }
                if (colors && !m->mColors[0]) {
                    m->mColors[0] = new aiColor4D[m->mNumVertices];
                    std::fill(m->mColors[0], m->mColors[0] + m->mNumVertices, aiColor4D(1, 1, 1, 1));
                }
            }
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i]);
        }
    };
    walk(walk, copy->mRootNode);
    // The exporter drops the root node itself, so the real root moves under a fresh one.
    wrapRoot(*copy, "gdc_root", aiMatrix4x4());
    return copy;
}

Capabilities targetCaps(const Format& target, const nlohmann::json& settings)
{
    Capabilities c = target.caps;
    if (target.id == "dts") {
        const int version = ts::kMaxDtsWriteVersion - settingValue(target, settings, "version");
        if (version < 28) {
            c.tangents = false;
        }
        if (version < 26) {
            c.uv2 = c.colors = false;
        }
    }
    return c;
}

// Writers without skins store skinned meshes as plain geometry under their node, so pose them the way the skin
// places them (bones at their current transforms) and drop the bones.
void bakeSkins(aiScene& scene)
{
    std::map<const aiNode*, aiMatrix4x4> worldOf;
    std::map<std::string, const aiNode*> byName;
    std::map<unsigned, const aiNode*> holder;
    auto walk = [&](auto& self, const aiNode* n, const aiMatrix4x4& parent) -> void {
        worldOf[n] = parent * n->mTransformation;
        byName.emplace(n->mName.C_Str(), n);
        for (unsigned i = 0; i < n->mNumMeshes; ++i) {
            holder.emplace(n->mMeshes[i], n);
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i], worldOf[n]);
        }
    };
    walk(walk, scene.mRootNode, aiMatrix4x4());
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        aiMesh* mesh = scene.mMeshes[m];
        if (!mesh->mNumBones || !holder.count(m)) {
            continue;
        }
        const aiMatrix4x4 nodeWorld = worldOf[holder[m]];
        const aiMatrix4x4 toLocal = aiMatrix4x4(nodeWorld).Inverse();
        std::vector<aiMatrix4x4> sum(mesh->mNumVertices, aiMatrix4x4(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
        std::vector<float> total(mesh->mNumVertices, 0.0f);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            auto it = byName.find(bone->mName.C_Str());
            const aiMatrix4x4 skin = (it != byName.end() ? worldOf[it->second] : aiMatrix4x4()) * bone->mOffsetMatrix;
            for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                const aiVertexWeight& vw = bone->mWeights[w];
                if (vw.mVertexId >= mesh->mNumVertices || vw.mWeight <= 0) {
                    continue;
                }
                aiMatrix4x4& s = sum[vw.mVertexId];
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        s[r][c] += skin[r][c] * vw.mWeight;
                    }
                }
                total[vw.mVertexId] += vw.mWeight;
            }
        }
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            if (total[v] <= 0) {
                continue;
            }
            aiMatrix4x4 xf = sum[v];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    xf[r][c] /= total[v];
                }
            }
            xf = toLocal * xf;
            const aiMatrix3x3 rot(xf);
            const aiMatrix3x3 nrm = aiMatrix3x3(rot).Inverse().Transpose();
            mesh->mVertices[v] = xf * mesh->mVertices[v];
            if (mesh->mNormals) {
                mesh->mNormals[v] = (nrm * mesh->mNormals[v]).NormalizeSafe();
            }
            if (mesh->mTangents && mesh->mBitangents) {
                mesh->mTangents[v] = (rot * mesh->mTangents[v]).NormalizeSafe();
                mesh->mBitangents[v] = (rot * mesh->mBitangents[v]).NormalizeSafe();
            }
        }
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            delete mesh->mBones[b];
        }
        delete[] mesh->mBones;
        mesh->mBones = nullptr;
        mesh->mNumBones = 0;
    }
}

// A face pointing past its mesh's vertices means the file is corrupt (or read past its end).
std::string badFaceIndex(const aiScene& scene)
{
    for (unsigned m = 0; m < scene.mNumMeshes; ++m) {
        const aiMesh* mesh = scene.mMeshes[m];
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned i = 0; i < face.mNumIndices; ++i) {
                if (face.mIndices[i] >= mesh->mNumVertices) {
                    return format("mesh '%s' face %u uses vertex %u of %u", mesh->mName.C_Str(), f, face.mIndices[i], mesh->mNumVertices);
                }
            }
        }
    }
    return {};
}

uintmax_t fileSize(const std::string& path)
{
    std::error_code ec;
    const uintmax_t n = fs::file_size(u8path(path), ec);
    return ec ? 0 : n;
}

std::string sizeText(uintmax_t bytes)
{
    if (bytes >= 1024u * 1024u) {
        return format("%.2f MB", double(bytes) / (1024.0 * 1024.0));
    }
    return format("%.1f KB", double(bytes) / 1024.0);
}

bool writeTs(const ts::Shape& shape, const Format& target, const nlohmann::json& settings, const std::string& path, ConvertResult& r)
{
    std::vector<uint8_t> bytes;
    if (target.id == "cdae") {
        ts::CdaeWriteOptions o;
        o.compression = ts::CdaeCompression(settingValue(target, settings, "compression"));
        o.zstdLevel = settingValue(target, settings, "zstdLevel");
        bool compressed = false;
        if (!ts::writeCdae(shape, o, bytes, r.error, &compressed)) {
            return false;
        }
        r.log.push_back(compressed ? "body zstd-compressed" : "body uncompressed");
    } else {
        const int version = ts::kMaxDtsWriteVersion - settingValue(target, settings, "version");
        for (const std::string& loss : ts::dtsVersionLosses(version)) {
            r.log.push_back(format("v%d: %s", version, loss.c_str()));
        }
        if (!ts::writeDts(shape, version, bytes, r.error, &r.warnings)) {
            return false;
        }
    }
    return writeFileBytes(path, bytes, r.error);
}

bool writeAssimp(const aiScene& scene, const Format& target, const nlohmann::json& settings, const std::string& path, ConvertResult& r)
{
    Assimp::Exporter exporter;
    Assimp::ExportProperties props;
    if (target.id == "gltf2" || target.id == "gltf1") {
        // The glTF exporters' default (0.01) drops node transforms close to identity, such as a 0.3 mm offset.
        props.SetPropertyFloat(AI_CONFIG_CHECK_IDENTITY_MATRIX_EPSILON, 0.0f);
    }
    auto setBool = [&](const char* prop, const char* key) {
        if (std::any_of(target.settings.begin(), target.settings.end(), [&](const SettingDef& s) { return s.key == key; })) {
            props.SetPropertyBool(prop, settingBool(target, settings, key));
        }
    };
    setBool(AI_CONFIG_EXPORT_GLTF_UNLIMITED_SKINNING_BONES_PER_VERTEX, "unlimitedBones");
    setBool(AI_CONFIG_USE_GLTF_PBR_SPECULAR_GLOSSINESS, "specGloss");
    setBool(AI_CONFIG_EXPORT_FBX_TRANSPARENCY_FACTOR_REFER_TO_OPACITY, "transparencyFactor");
    setBool(AI_CONFIG_EXPORT_XFILE_64BIT, "x64");
    setBool(AI_CONFIG_EXPORT_POINT_CLOUDS, "pointClouds");

    ScenePtr owned = target.id == "collada"                          ? colladaScene(scene)
                     : target.id == "gltf2" || target.id == "gltf1" ? gltfScene(scene)
                     : target.id == "fbx"                            ? fbxScene(scene)
                                                                     : nullptr;
    auto edit = [&]() -> aiScene& {
        if (!owned) {
            owned = cloneScene(scene);
        }
        return *owned;
    };
    auto current = [&]() -> const aiScene& { return owned ? *owned : scene; };
    if (std::any_of(scene.mMeshes, scene.mMeshes + scene.mNumMeshes, [](const aiMesh* m) { return m->mNumBones > 0; }) &&
        !targetCaps(target, settings).skins) {
        bakeSkins(edit());
        r.warnings.push_back(tr("gdc.model.note.bakedSkin"));
    }
    if (writerUp(target.id) == UpAxis::Y) {
        rotateScene(edit(), zUpToYUp());
    }
    if (lodAnchorNeeded(current())) {
        anchorGameLod(edit(), r);
    }
    if (exporter.Export(&current(), target.exporterId(settings), path, 0, &props) != aiReturn_SUCCESS) {
        r.error = exporter.GetErrorString();
        if (r.error.empty()) {
            r.error = "Assimp export failed";
        }
        return false;
    }
    if (target.id == "gltf2" && !gltfDropEmptyRoot(path, r.error)) {
        return false;
    }
    if (target.id == "gltf2" && settingBool(target, settings, "draco")) {
        DracoOptions d;
        d.level = settingValue(target, settings, "dracoLevel");
        d.qNormal = settingValue(target, settings, "qNormal");
        d.qUv = settingValue(target, settings, "qUv");
        d.qColor = settingValue(target, settings, "qColor");
        d.qGeneric = settingValue(target, settings, "qGeneric");
        const uintmax_t before = fileSize(path);
        std::string info;
        if (!dracoCompressGltf(path, d, r.error, info)) {
            r.error = "Draco: " + r.error;
            return false;
        }
        r.log.push_back(format("draco: %s -> %s, %s", sizeText(before).c_str(), sizeText(fileSize(path)).c_str(), info.c_str()));
    }
    return true;
}

} // namespace

ts::AiToTsOptions aiToTsOptions(const Format& target, const nlohmann::json& settings)
{
    ts::AiToTsOptions o;
    o.lod = ts::LodMode(settingValue(target, settings, "lod"));
    o.singleDetailSize = settingValue(target, settings, "detailSize");
    o.scale = ts::ScaleMode(settingValue(target, settings, "scale"));
    o.exporterVersion = settingValue(target, settings, "exporter") == 1 ? ts::kExporterAssimp : ts::kExporterCollada;
    return o;
}

ts::TsToAiOptions tsToAiOptions(const Format& target, const nlohmann::json& settings)
{
    ts::TsToAiOptions o;
    o.lodNames = settingBool(target, settings, "lodNames");
    o.includeCollision = o.lodNames && settingBool(target, settings, "collision");
    o.includeAnimations = settingBool(target, settings, "animations");
    return o;
}

ConvertResult convertModel(Model& source, const std::string& outputPath, const ConvertOptions& options)
try {
    ConvertResult r;
    if (!options.target || !options.target->canWrite) {
        r.error = "no target format";
        return r;
    }
    const Format& target = *options.target;
    const nlohmann::json settings = withDefaults(target, options.settings);
    r.log.push_back(target.label + ": " + describeSettings(target, settings));

    std::error_code ec;
    fs::create_directories(u8path(outputPath).parent_path(), ec);

    const ts::Shape* shape = nullptr;
    const aiScene* scene = nullptr;
    if (target.isTs) {
        shape = source.shape(aiToTsOptions(target, settings), r.warnings, r.error);
        if (!shape || !writeTs(*shape, target, settings, outputPath, r)) {
            return r;
        }
    } else {
        scene = source.scene(tsToAiOptions(target, settings), r.warnings);
        if (!scene) {
            r.error = r.error.empty() ? "could not build a scene" : r.error;
            return r;
        }
        if (!writeAssimp(*scene, target, settings, outputPath, r)) {
            return r;
        }
    }
    r.ok = true;
    r.log.push_back(format("wrote %s", sizeText(fileSize(outputPath)).c_str()));
    r.log.push_back(format("up axis: source %s, written %s-up", source.upAxisNote().c_str(),
                           target.isTs ? "Z" : upAxisName(writerUp(target.id))));

    if (!options.validate) {
        return r;
    }
    // Assimp has no reader for these outputs (its .stp reader only reads IFC), so there is nothing to compare.
    if (!target.isTs && (!readerForExt(modelExtension(outputPath)) || target.id == "stp")) {
        r.report.verdict = Verdict::Warn;
        r.report.lines.push_back(std::string("WARN ") + tr("gdc.model.warn.notChecked"));
        r.report.json["verdict"] = verdictName(r.report.verdict);
        r.validated = true;
        return r;
    }

    // Re-read what was written and compare it with what the source holds.
    Summary before, after;
    StructureHints hints;
    const aiScene* writtenScene = nullptr;
    Model reread;
    const ts::TsToAiOptions exportOpts = tsToAiOptions(target, settings);
    auto rereadFailed = [&r](const std::string& why) {
        r.report.verdict = Verdict::Fail;
        r.report.lines.push_back(std::string("FAIL ") + format(tr("gdc.model.warn.reread"), why.c_str()));
        r.validated = true;
        return r;
    };
    if (target.isTs) {
        ts::Shape written;
        std::string err;
        if (!ts::readShapeFile(outputPath, written, err)) {
            return rereadFailed(err);
        }
        const ts::AiToTsOptions importOpts = aiToTsOptions(target, settings);
        hints.singleDetail = importOpts.lod == ts::LodMode::SingleDetail;
        if (source.isTs()) {
            DetailFilter f;
            f.perDetail = true;
            before = summarize(*shape, f);
            after = summarize(written, f);
        } else {
            std::vector<std::string> ignored;
            before = summarize(*source.scene(exportOpts, ignored), SceneNames{true, importOpts});
            after = summarize(written, DetailFilter{});
        }
    } else {
        std::string err;
        std::vector<std::string> ignored;
        if (!reread.load(outputPath, err, ignored)) {
            return rereadFailed(err);
        }
        writtenScene = reread.scene(exportOpts, ignored);
        if (const std::string bad = badFaceIndex(*writtenScene); !bad.empty()) {
            return rereadFailed(bad);
        }
        // FBX, COLLADA and 3DS write the scene's own root container as a node.
        if (scene && scene->mRootNode) {
            hints.inserted.insert(scene->mRootNode->mName.C_Str());
        }
        hints.dropped.insert(r.foldedNodes.begin(), r.foldedNodes.end());
        // glTF readers make a lone top node the scene root, which every reader treats as a container.
        if (writtenScene->mRootNode && !writtenScene->mRootNode->mNumMeshes && !isBaseStart(writtenScene->mRootNode)) {
            hints.dropped.insert(writtenScene->mRootNode->mName.C_Str());
        }
        if (source.isTs()) {
            std::string e2;
            const ts::Shape* src = source.shape({}, ignored, e2);
            DetailFilter f;
            f.allDetails = exportOpts.lodNames;
            f.collision = exportOpts.includeCollision;
            before = summarize(*src, f);
            hints.singleDetail = !exportOpts.lodNames;
            SceneNames names{true, {}, target.meshNameOf, ts::objectNaming(*scene, {}).collapse};
            names.options.lod = exportOpts.lodNames ? ts::LodMode::TrailingNumber : ts::LodMode::SingleDetail;
            after = summarize(*writtenScene, names);
        } else {
            before = summarize(*scene);
            after = summarize(*writtenScene, SceneNames{false, {}, target.meshNameOf});
        }
    }
    const Capabilities caps = targetCaps(target, settings);
    r.report = compareSummaries(before, after, caps, toleranceFor(target, settings), hints);
    if (!target.isTs) {
        checkUpAxis(r.report, writerUp(target.id), readRawFrame(outputPath), before);
        if (writtenScene && scene) {
            checkGameLod(r.report, *writtenScene, *scene, caps.hierarchy);
            if (caps.meshNames) {
                checkStoredNames(r.report, reread.restoredNames(), *writtenScene);
                checkImportNames(r.report, *writtenScene, target.id == "gltf2" || target.id == "gltf1");
            }
        }
    }
    r.validated = true;
    return r;
} catch (const std::exception& e) {
    ConvertResult r;
    r.error = std::string("internal error: ") + e.what();
    return r;
}

std::vector<std::string> planOutputPaths(const std::vector<OutputRequest>& requests, const std::string& srcRoot, const std::string& outRoot)
{
    auto dirFor = [&](const fs::path& src) {
        if (outRoot.empty()) {
            return src.parent_path();
        }
        std::error_code ec;
        fs::path rel = fs::relative(src.parent_path(), u8path(srcRoot), ec);
        if (ec || rel.empty() || *rel.begin() == "..") {
            rel.clear();
        }
        return u8path(outRoot) / rel;
    };
    std::vector<fs::path> plain(requests.size());
    std::map<std::string, int> uses;
    for (size_t i = 0; i < requests.size(); ++i) {
        const fs::path src = u8path(requests[i].source);
        std::string stem = u8str(src.filename());
        const std::string ext = modelExtension(requests[i].source);
        stem = stem.substr(0, stem.size() - ext.size());
        plain[i] = dirFor(src) / u8path(stem + requests[i].ext);
        ++uses[toLower(u8str(plain[i]))];
    }
    std::vector<std::string> out(requests.size());
    std::map<std::string, int> taken;
    for (size_t i = 0; i < requests.size(); ++i) {
        const fs::path src = u8path(requests[i].source);
        const std::string key = toLower(u8str(plain[i]));
        fs::path p = plain[i];
        if (uses[key] > 1 || key == toLower(u8str(src))) {
            p = dirFor(src) / u8path(u8str(src.filename()) + requests[i].ext);
        }
        // Two targets with the same extension for one source (glTF 1 and 2).
        if (taken[toLower(u8str(p))]++) {
            p = dirFor(src) / u8path(u8str(src.filename()) + "." + requests[i].targetId + requests[i].ext);
        }
        out[i] = u8str(p);
    }
    return out;
}

} // namespace model
