#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

#include <assimp/scene.h>

#include "core/i18n.h"
#include "core/strings.h"
#include "model/assimp/ai_bridge.h"
#include "model/assimp/ai_util.h"

namespace ts {
namespace {

// Copies src into a new[] array (null when empty) and returns the count.
template <class T>
unsigned fillArray(T*& dst, const std::vector<T>& src)
{
    dst = src.empty() ? nullptr : new T[src.size()];
    std::copy(src.begin(), src.end(), dst);
    return unsigned(src.size());
}

aiNode* newNode(const std::string& name, aiNode* parent)
{
    aiNode* n = new aiNode(name);
    n->mParent = parent;
    return n;
}

void attachChildren(aiNode* parent, const std::vector<aiNode*>& children)
{
    parent->mNumChildren = fillArray(parent->mChildren, children);
    for (aiNode* child : children) {
        child->mParent = parent;
    }
}

aiMaterial* namedMaterial(const std::string& name)
{
    aiMaterial* m = new aiMaterial;
    aiString value(name);
    m->AddProperty(&value, AI_MATKEY_NAME);
    return m;
}

struct Exporter {
    const Shape& shape;
    const TsToAiOptions& opt;
    std::vector<std::string>& warnings;

    std::unique_ptr<aiScene> scene = std::make_unique<aiScene>();
    std::vector<aiMesh*> meshes;
    std::vector<aiMaterial*> materials;
    std::vector<aiAnimation*> animations;
    int noMaterialIndex = -1;
    nlohmann::json collapse = nlohmann::json::array();   // generated nodes that are not shape nodes
    nlohmann::json meshNodes = nlohmann::json::object(); // mesh node name -> [object name, detail size]
    std::vector<aiNode*> tsNodes;
    std::vector<std::vector<aiNode*>> meshChildren; // generated mesh nodes per shape node
    std::vector<aiNode*> skinNodes;
    // TS object index -> generated aiNode name of each exported detail mesh (for morph channels)
    std::map<int, std::vector<std::pair<std::string, unsigned>>> objectMeshNodes;

    void warn(const std::string& w) { warnings.push_back(w); }

    unsigned materialFor(const Primitive& p)
    {
        if (p.hasMaterial() && p.material() < materials.size()) {
            return p.material();
        }
        if (noMaterialIndex < 0) {
            noMaterialIndex = int(materials.size());
            materials.push_back(namedMaterial("NoMaterial"));
        }
        return unsigned(noMaterialIndex);
    }

    void buildMaterials()
    {
        for (const Material& mat : shape.materials) {
            aiMaterial* m = namedMaterial(mat.name);
            const int twoSided = 0;
            m->AddProperty(&twoSided, 1, AI_MATKEY_TWOSIDED);
            if (mat.flags & Translucent) {
                const float opacity = 0.5f;
                m->AddProperty(&opacity, 1, AI_MATKEY_OPACITY);
            }
            materials.push_back(m);
        }
    }

    bool lodLayoutNeeded() const
    {
        int meshDetails = 0;
        for (const Detail& d : shape.details) {
            if (d.subShapeNum >= 0) {
                ++meshDetails;
            }
        }
        if (meshDetails <= 1 || !opt.lodNames) {
            return false;
        }
        for (const Node& n : shape.nodes) {
            if (n.parentIndex < 0 && hasPrefix(toLower(shape.name(n.nameIndex)), "base")) {
                for (const Node& c : shape.nodes) {
                    if (&c != &n && c.parentIndex == int(&n - shape.nodes.data()) &&
                        hasPrefix(toLower(shape.name(c.nameIndex)), "start")) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // Converts one TS mesh into aiMeshes (one per material group).
    std::vector<unsigned> convertMesh(const Mesh& src, const std::string& name)
    {
        std::vector<unsigned> out;
        Mesh m = src;
        convertToTriangles(m);
        const bool skin = m.isSkin();
        const std::vector<Vec3>& verts = m.restVerts();
        const std::vector<Vec3>& norms = m.restNorms();
        const int perFrame = m.vertsPerFrame > 0 ? m.vertsPerFrame : int(verts.size());
        const int frames = skin ? 1 : std::max(1, std::min(m.numFrames, perFrame ? int(verts.size()) / perFrame : 1));
        if (m.numMatFrames > 1) {
            warn(format(tr("gdc.model.note.uvFrames"), name.c_str(), m.numMatFrames));
        }

        // Group primitives by material.
        std::map<unsigned, std::vector<uint32_t>> byMaterial;
        for (const Primitive& p : m.primitives) {
            auto& list = byMaterial[materialFor(p)];
            list.insert(list.end(), m.indices.begin() + p.start, m.indices.begin() + p.start + p.numElements);
        }

        // Per-vertex bone lists for skins.
        std::vector<std::vector<std::pair<int, float>>> influences;
        if (skin) {
            influences.resize(verts.size());
            const size_t n = std::min({m.vertexIndex.size(), m.boneIndex.size(), m.weight.size()});
            for (size_t i = 0; i < n; ++i) {
                if (m.vertexIndex[i] >= 0 && m.vertexIndex[i] < int(verts.size())) {
                    influences[m.vertexIndex[i]].push_back({m.boneIndex[i], m.weight[i]});
                }
            }
        }

        for (const auto& [material, tris] : byMaterial) {
            if (tris.empty()) {
                continue;
            }
            std::unordered_map<uint32_t, uint32_t> remap;
            std::vector<uint32_t> used;
            for (uint32_t i : tris) {
                if (i >= uint32_t(perFrame)) {
                    continue;
                }
                if (remap.emplace(i, uint32_t(used.size())).second) {
                    used.push_back(i);
                }
            }
            aiMesh* am = new aiMesh;
            am->mName = aiString(name);
            am->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
            am->mMaterialIndex = material;
            const unsigned nv = unsigned(used.size());
            am->mNumVertices = nv;
            am->mVertices = new aiVector3D[nv];
            am->mNormals = new aiVector3D[nv];
            const bool hasTangents = m.tangents.size() >= size_t(perFrame);
            if (hasTangents) {
                am->mTangents = new aiVector3D[nv];
                am->mBitangents = new aiVector3D[nv];
            }
            am->mTextureCoords[0] = new aiVector3D[nv];
            am->mNumUVComponents[0] = 2;
            const bool uv2 = m.tverts2.size() >= size_t(perFrame);
            if (uv2) {
                am->mTextureCoords[1] = new aiVector3D[nv];
                am->mNumUVComponents[1] = 2;
            }
            const bool colors = m.colors.size() >= size_t(perFrame);
            if (colors) {
                am->mColors[0] = new aiColor4D[nv];
            }
            for (unsigned k = 0; k < nv; ++k) {
                const uint32_t v = used[k];
                const Vec3 p = v < verts.size() ? verts[v] : Vec3{};
                const Vec3 n = v < norms.size() ? norms[v] : Vec3{0, 0, 1};
                am->mVertices[k] = {p.x, p.y, p.z};
                am->mNormals[k] = {n.x, n.y, n.z};
                if (hasTangents) {
                    const Vec4& t4 = m.tangents[v];
                    const Vec3 t{t4.x, t4.y, t4.z};
                    const Vec3 b = cross(n, t) * (t4.w < 0 ? -1.0f : 1.0f);
                    am->mTangents[k] = {t.x, t.y, t.z};
                    am->mBitangents[k] = {b.x, b.y, b.z};
                }
                const Vec2 uv = v < m.tverts.size() ? m.tverts[v] : Vec2{};
                am->mTextureCoords[0][k] = {uv.x, 1.0f - uv.y, 0};
                if (uv2) {
                    am->mTextureCoords[1][k] = {m.tverts2[v].x, 1.0f - m.tverts2[v].y, 0};
                }
                if (colors) {
                    const Color8& c = m.colors[v];
                    am->mColors[0][k] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
                }
            }
            const unsigned nf = unsigned(tris.size() / 3);
            am->mNumFaces = nf;
            am->mFaces = new aiFace[nf];
            unsigned written = 0;
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                if (tris[i] >= uint32_t(perFrame) || tris[i + 1] >= uint32_t(perFrame) || tris[i + 2] >= uint32_t(perFrame)) {
                    continue;
                }
                aiFace& f = am->mFaces[written++];
                f.mNumIndices = 3;
                f.mIndices = new unsigned[3]{remap[tris[i]], remap[tris[i + 2]], remap[tris[i + 1]]};
            }
            am->mNumFaces = written;

            // Mesh frames become morph targets.
            if (frames > 1) {
                am->mNumAnimMeshes = unsigned(frames - 1);
                am->mAnimMeshes = new aiAnimMesh*[frames - 1];
                for (int f = 1; f < frames; ++f) {
                    aiAnimMesh* morph = new aiAnimMesh;
                    morph->mName = aiString(format("frame%d", f));
                    morph->mNumVertices = nv;
                    morph->mVertices = new aiVector3D[nv];
                    morph->mNormals = new aiVector3D[nv];
                    for (unsigned k = 0; k < nv; ++k) {
                        const size_t v = size_t(f) * perFrame + used[k];
                        const Vec3 p = v < m.verts.size() ? m.verts[v] : verts[used[k]];
                        const Vec3 n = v < m.norms.size() ? m.norms[v] : norms[used[k]];
                        morph->mVertices[k] = {p.x, p.y, p.z};
                        morph->mNormals[k] = {n.x, n.y, n.z};
                    }
                    morph->mWeight = 0.0f;
                    am->mAnimMeshes[f - 1] = morph;
                }
            }

            if (skin) {
                // One aiBone per shape node used by this submesh.
                std::map<int, std::vector<aiVertexWeight>> perBone;
                std::map<int, int> slotOfNode;
                for (unsigned k = 0; k < nv; ++k) {
                    for (const auto& [slot, w] : influences[used[k]]) {
                        if (slot < 0 || slot >= int(m.nodeIndex.size()) || w <= 0.0f) {
                            continue;
                        }
                        const int node = m.nodeIndex[slot];
                        auto [it, inserted] = slotOfNode.emplace(node, slot);
                        if (!inserted && it->second != slot &&
                            std::memcmp(m.initialTransforms[it->second].m, m.initialTransforms[slot].m, sizeof(Mat4Raw)) != 0) {
                            warn(format(tr("gdc.model.note.twoBinds"), name.c_str(),
                                        shape.name(shape.nodes[node].nameIndex).c_str()));
                        }
                        perBone[node].push_back({k, w});
                    }
                }
                am->mNumBones = unsigned(perBone.size());
                am->mBones = new aiBone*[std::max<size_t>(1, perBone.size())];
                unsigned b = 0;
                for (auto& [node, weights] : perBone) {
                    aiBone* bone = new aiBone;
                    bone->mName = aiString(node >= 0 && node < int(shape.nodes.size()) ? shape.name(shape.nodes[node].nameIndex) : "bone");
                    bone->mOffsetMatrix = toAi(toMat4(m.initialTransforms[slotOfNode[node]]));
                    bone->mNumWeights = fillArray(bone->mWeights, weights);
                    am->mBones[b++] = bone;
                }
            }
            out.push_back(unsigned(meshes.size()));
            meshes.push_back(am);
        }
        return out;
    }

    static void setMeshes(aiNode* node, const std::vector<unsigned>& ids) { node->mNumMeshes = fillArray(node->mMeshes, ids); }

    void addMeshNode(int parentTs, const std::string& name, const std::string& objName, int size, const std::vector<unsigned>& ids,
                     int objectIndex)
    {
        if (ids.empty()) {
            return;
        }
        meshNodes[name] = {objName, size};
        objectMeshNodes[objectIndex].push_back({name, ids.front()});
        // The usual layout: the object's node is itself named "<object><size>", so it carries the mesh.
        if (parentTs >= 0 && !tsNodes[parentTs]->mNumMeshes && toLower(tsNodes[parentTs]->mName.C_Str()) == toLower(name)) {
            setMeshes(tsNodes[parentTs], ids);
            return;
        }
        aiNode* node = newNode(name, nullptr);
        setMeshes(node, ids);
        collapse.push_back(name);
        if (parentTs >= 0) {
            meshChildren[parentTs].push_back(node);
        } else {
            skinNodes.push_back(node);
        }
    }

    void buildObjects()
    {
        // Which details to export: all mesh details (or only the highest visible one).
        const int highestVisible = shape.highestVisibleDetail();
        for (int o = 0; o < int(shape.objects.size()); ++o) {
            const Object& obj = shape.objects[o];
            const std::string objName = shape.name(obj.nameIndex);
            for (int d = 0; d < int(shape.details.size()); ++d) {
                const Detail& det = shape.details[d];
                if ((!opt.lodNames && d != highestVisible) || (det.size < 0 && !opt.includeCollision)) {
                    continue;
                }
                const Mesh* found = shape.detailMesh(o, d);
                if (!found) {
                    continue;
                }
                const Mesh& mesh = *found;
                const int size = int(det.size);
                const std::string nodeName = opt.lodNames ? objName + std::to_string(size) : objName;
                const std::vector<unsigned> ids = convertMesh(mesh, nodeName);
                const int parentTs = mesh.isSkin() || obj.nodeIndex >= int(tsNodes.size()) ? -1 : obj.nodeIndex;
                addMeshNode(parentTs, nodeName, objName, size, ids, o);
            }
        }
    }

    void buildAnimations()
    {
        if (!opt.includeAnimations) {
            return;
        }
        for (const Sequence& seq : shape.sequences) {
            const std::string seqName = shape.name(seq.nameIndex);
            const int K = std::max(1, seq.numKeyframes);
            const bool moveGround = groundMoves(shape, seq);
            const int M = moveGround ? std::max(K, std::max(seq.numGroundFrames, 1)) : K;
            const double tps = seq.duration > 0 && M > 1 ? double(M - 1) / seq.duration : 30.0;
            aiAnimation* anim = new aiAnimation;
            anim->mName = aiString(seqName);
            anim->mTicksPerSecond = tps;
            anim->mDuration = double(M - 1);
            std::vector<aiNodeAnim*> channels;
            const bool arbitrary = seq.has(SequenceFlag::ArbitraryScale);
            bool arbitraryRotated = false;
            bool rooted = false;
            auto poseAt = [&](int n, int f, bool r, bool t, bool s, int rr, int tr, int sr, Vec3& p, Quat& q, Vec3& sc) {
                const Quat16 q16 = r ? shape.nodeRotations[size_t(seq.baseRotation) + size_t(rr) * K + f] : shape.defaultRotations[n];
                p = t ? shape.nodeTranslations[size_t(seq.baseTranslation) + size_t(tr) * K + f] : shape.defaultTranslations[n];
                sc = {1, 1, 1};
                if (s) {
                    const size_t i = size_t(seq.baseScale) + size_t(sr) * K + f;
                    if (seq.has(SequenceFlag::UniformScale) && i < shape.nodeUniformScales.size()) {
                        sc = {shape.nodeUniformScales[i], shape.nodeUniformScales[i], shape.nodeUniformScales[i]};
                    } else if (seq.has(SequenceFlag::AlignedScale) && i < shape.nodeAlignedScales.size()) {
                        sc = shape.nodeAlignedScales[i];
                    } else if (arbitrary && i < shape.nodeArbitraryScaleFactors.size()) {
                        sc = shape.nodeArbitraryScaleFactors[i];
                        if (i < shape.nodeArbitraryScaleRots.size() && !(shape.nodeArbitraryScaleRots[i] == Quat16{})) {
                            arbitraryRotated = true;
                        }
                    }
                }
                q = toQuat(q16);
            };
            if (arbitrary) {
                for (int n = 0; n < int(shape.nodes.size()); ++n) {
                    if (!seq.scaleMatters.test(n)) {
                        continue;
                    }
                    const int sr = seq.scaleMatters.rank(n);
                    for (int f = 0; f < K; ++f) {
                        const size_t i = size_t(seq.baseScale) + size_t(sr) * K + f;
                        if (i < shape.nodeArbitraryScaleRots.size() && !(shape.nodeArbitraryScaleRots[i] == Quat16{})) {
                            arbitraryRotated = true;
                        }
                    }
                }
            }
            for (int n = 0; n < int(shape.nodes.size()); ++n) {
                const bool r = seq.rotationMatters.test(n), t = seq.translationMatters.test(n), s = seq.scaleMatters.test(n);
                const int parent = shape.nodes[n].parentIndex;
                const bool root = parent < 0 || parent >= int(shape.nodes.size());
                rooted = rooted || root;
                if (!r && !t && !s && !(moveGround && root)) {
                    continue;
                }
                aiNodeAnim* ch = new aiNodeAnim;
                ch->mNodeName = aiString(shape.name(shape.nodes[n].nameIndex));
                ch->mNumPositionKeys = ch->mNumRotationKeys = ch->mNumScalingKeys = unsigned(M);
                ch->mPositionKeys = new aiVectorKey[M];
                ch->mRotationKeys = new aiQuatKey[M];
                ch->mScalingKeys = new aiVectorKey[M];
                const int rr = seq.rotationMatters.rank(n), tr = seq.translationMatters.rank(n), sr = seq.scaleMatters.rank(n);
                for (int frame = 0; frame < M; ++frame) {
                    const float nodeU = M == K ? float(frame) : (M > 1 && K > 1 ? float(frame) * float(K - 1) / float(M - 1) : 0.0f);
                    const float u = K <= 1 ? 0.0f : std::clamp(nodeU, 0.0f, float(K - 1));
                    const int k0 = std::min(int(u), K - 1);
                    const int k1 = std::min(k0 + 1, K - 1);
                    const float blend = u - float(k0);
                    Vec3 p, sc;
                    Quat q;
                    poseAt(n, k0, r, t, s, rr, tr, sr, p, q, sc);
                    if (blend > 0.0f && k1 != k0) {
                        Vec3 p1, sc1;
                        Quat q1;
                        poseAt(n, k1, r, t, s, rr, tr, sr, p1, q1, sc1);
                        p = p + (p1 - p) * blend;
                        sc = sc + (sc1 - sc) * blend;
                        q = slerp(q, q1, blend);
                    }
                    if (moveGround && root) {
                        const float time = M > 1 ? seq.duration * float(frame) / float(M - 1) : 0.0f;
                        decompose(groundTransform(shape, seq, time) * composeTRS(p, q, sc), p, q, sc);
                    }
                    ch->mRotationKeys[frame] = aiQuatKey(double(frame), aiQuaternion(q.w, q.x, q.y, q.z));
                    ch->mPositionKeys[frame] = aiVectorKey(double(frame), aiVector3D(p.x, p.y, p.z));
                    ch->mScalingKeys[frame] = aiVectorKey(double(frame), aiVector3D(sc.x, sc.y, sc.z));
                }
                channels.push_back(ch);
            }
            if (moveGround && !rooted) {
                warn(format(tr("gdc.model.note.ground"), seqName.c_str(), seq.numGroundFrames));
            }
            if (arbitraryRotated) {
                warn(format(tr("gdc.model.note.scaleRot"), seqName.c_str()));
            }
            if (seq.visMatters.any()) {
                warn(format(tr("gdc.model.note.vis"), seqName.c_str()));
            }
            if (seq.matFrameMatters.any()) {
                warn(format(tr("gdc.model.note.matAnim"), seqName.c_str()));
            }

            std::vector<aiMeshMorphAnim*> morphs;
            if (seq.frameMatters.any()) {
                for (int o = 0; o < int(shape.objects.size()); ++o) {
                    if (!seq.frameMatters.test(o)) {
                        continue;
                    }
                    BitUnion u{seq};
                    const int stateRank = u.rank(o);
                    for (const auto& [nodeName, meshId] : objectMeshNodes[o]) {
                        const aiMesh* am = meshes[meshId];
                        if (!am->mNumAnimMeshes) {
                            continue;
                        }
                        aiMeshMorphAnim* mc = new aiMeshMorphAnim;
                        mc->mName = aiString(nodeName);
                        mc->mNumKeys = unsigned(M);
                        mc->mKeys = new aiMeshMorphKey[M];
                        for (int frame = 0; frame < M; ++frame) {
                            const float nodeU = M == K ? float(frame) : (M > 1 && K > 1 ? float(frame) * float(K - 1) / float(M - 1) : 0.0f);
                            const int src = K <= 1 ? 0 : std::min(int(std::clamp(nodeU, 0.0f, float(K - 1))), K - 1);
                            const size_t si = size_t(seq.baseObjectState) + size_t(stateRank) * K + src;
                            const int shapeFrame = si < shape.objectStates.size() ? shape.objectStates[si].frameIndex : 0;
                            aiMeshMorphKey& key = mc->mKeys[frame];
                            key.mTime = double(frame);
                            key.mNumValuesAndWeights = am->mNumAnimMeshes;
                            key.mValues = new unsigned[am->mNumAnimMeshes];
                            key.mWeights = new double[am->mNumAnimMeshes];
                            for (unsigned t = 0; t < am->mNumAnimMeshes; ++t) {
                                key.mValues[t] = t;
                                key.mWeights[t] = int(t) + 1 == shapeFrame ? 1.0 : 0.0;
                            }
                        }
                        morphs.push_back(mc);
                    }
                }
            }

            anim->mNumChannels = fillArray(anim->mChannels, channels);
            anim->mNumMorphMeshChannels = fillArray(anim->mMorphMeshChannels, morphs);
            if (channels.empty() && morphs.empty()) {
                delete anim;
                continue;
            }
            animations.push_back(anim);
        }
    }

    struct BitUnion {
        const Sequence& seq;
        int rank(int o) const
        {
            int r = 0;
            for (int i = 0; i < o; ++i) {
                r += seq.frameMatters.test(i) || seq.matFrameMatters.test(i) || seq.visMatters.test(i);
            }
            return r;
        }
    };

    nlohmann::json metadata() const
    {
        nlohmann::json j;
        j["version"] = 1;
        j["exporter"] = shape.exporterVersion;
        j["lodNames"] = opt.lodNames;
        j["collapse"] = collapse;
        j["meshNodes"] = meshNodes;
        nlohmann::json details = nlohmann::json::array();
        for (const Detail& d : shape.details) {
            nlohmann::json jd{{"name", shape.name(d.nameIndex)}, {"size", d.size}};
            if (d.subShapeNum < 0) {
                jd["bb"] = {{"equatorSteps", d.bbEquatorSteps}, {"polarSteps", d.bbPolarSteps}, {"polarAngle", d.bbPolarAngle},
                            {"detailLevel", d.bbDetailLevel},   {"dimension", d.bbDimension},   {"includePoles", d.bbIncludePoles}};
            }
            details.push_back(jd);
        }
        j["details"] = details;
        nlohmann::json mats = nlohmann::json::array();
        for (const Material& m : shape.materials) {
            mats.push_back({{"name", m.name}, {"flags", m.flags}, {"detailScale", m.detailScale}, {"reflectionAmount", m.reflectionAmount}});
        }
        j["materials"] = mats;
        nlohmann::json seqs = nlohmann::json::array();
        for (const Sequence& q : shape.sequences) {
            nlohmann::json js{{"name", shape.name(q.nameIndex)}, {"flags", q.flags}, {"priority", q.priority}, {"toolBegin", q.toolBegin}};
            nlohmann::json trig = nlohmann::json::array();
            for (int t = 0; t < q.numTriggers; ++t) {
                const size_t i = size_t(q.firstTrigger) + t;
                if (i < shape.triggers.size()) {
                    trig.push_back({{"state", shape.triggers[i].state}, {"pos", shape.triggers[i].pos}});
                }
            }
            js["triggers"] = trig;
            seqs.push_back(js);
        }
        j["sequences"] = seqs;
        return j;
    }

    aiScene* run()
    {
        buildMaterials();
        aiNode* root = newNode("root", nullptr);
        scene->mRootNode = root;
        aiNode* container = root;
        std::vector<aiNode*> wrapper;
        if (lodLayoutNeeded()) {
            aiNode* base = newNode("base01", root);
            aiNode* start = newNode("start01", base);
            attachChildren(base, {start});
            wrapper = {base};
            container = start;
            collapse.push_back("base01");
            collapse.push_back("start01");
        }

        tsNodes.resize(shape.nodes.size());
        meshChildren.resize(shape.nodes.size());
        for (size_t i = 0; i < shape.nodes.size(); ++i) {
            const Node& n = shape.nodes[i];
            aiNode* node = newNode(shape.name(n.nameIndex), nullptr);
            node->mTransformation = toAi(shape.nodeDefaultLocal(int(i)));
            tsNodes[i] = node;
        }
        buildObjects();

        std::vector<std::vector<aiNode*>> children(shape.nodes.size());
        std::vector<aiNode*> containerChildren;
        for (size_t i = 0; i < shape.nodes.size(); ++i) {
            const int p = shape.nodes[i].parentIndex;
            (p >= 0 && p < int(shape.nodes.size()) ? children[p] : containerChildren).push_back(tsNodes[i]);
        }
        for (size_t i = 0; i < shape.nodes.size(); ++i) {
            children[i].insert(children[i].end(), meshChildren[i].begin(), meshChildren[i].end());
            attachChildren(tsNodes[i], children[i]);
        }
        containerChildren.insert(containerChildren.end(), skinNodes.begin(), skinNodes.end());
        attachChildren(container, containerChildren);
        if (!wrapper.empty()) {
            attachChildren(root, wrapper);
        }

        buildAnimations();

        if (materials.empty()) {
            materials.push_back(namedMaterial("NoMaterial"));
        }
        scene->mNumMaterials = fillArray(scene->mMaterials, materials);
        scene->mNumMeshes = fillArray(scene->mMeshes, meshes);
        scene->mNumAnimations = fillArray(scene->mAnimations, animations);
        const nlohmann::json meta = metadata();
        putJsonMeta(root->mMetaData, kTsMetadataKey, meta);
        putJsonMeta(scene->mMetaData, kTsMetadataKey, meta);
        return scene.release();
    }
};

} // namespace

aiScene* shapeToAssimp(const Shape& shape, const TsToAiOptions& options, std::vector<std::string>& warnings)
{
    Exporter e{shape, options, warnings};
    return e.run();
}

} // namespace ts
