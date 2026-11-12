#include "model/validate/summary.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "core/strings.h"
#include "model/assimp/ai_util.h"

namespace model {
namespace {

void foldMeshNodes(aiScene& scene, const std::function<std::string(const std::string&, const std::string&)>& rule)
{
    auto walk = [&](auto& self, aiNode* n) -> void {
        std::vector<aiNode*> kept;
        std::vector<unsigned> meshes(n->mMeshes, n->mMeshes + n->mNumMeshes);
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            aiNode* c = n->mChildren[i];
            self(self, c);
            const std::string parent = n->mName.C_Str();
            if (!c->mNumChildren && c->mNumMeshes && rule(c->mName.C_Str(), parent) == parent && parent != c->mName.C_Str()) {
                meshes.insert(meshes.end(), c->mMeshes, c->mMeshes + c->mNumMeshes);
                delete c;
            } else {
                kept.push_back(c);
            }
        }
        if (kept.size() == n->mNumChildren) {
            return;
        }
        delete[] n->mMeshes;
        n->mNumMeshes = unsigned(meshes.size());
        n->mMeshes = meshes.empty() ? nullptr : new unsigned[meshes.size()];
        std::copy(meshes.begin(), meshes.end(), n->mMeshes);
        n->mNumChildren = unsigned(kept.size());
        std::copy(kept.begin(), kept.end(), n->mChildren);
        if (kept.empty()) {
            delete[] n->mChildren;
            n->mChildren = nullptr;
        }
    };
    walk(walk, scene.mRootNode);
}

using namespace ts;

Mat4 blend(const std::vector<std::pair<Mat4, float>>& parts, const Mat4& fallback)
{
    float total = 0;
    Mat4 out;
    for (float& v : out.m) {
        v = 0;
    }
    for (const auto& [m, w] : parts) {
        for (int i = 0; i < 16; ++i) {
            out.m[i] += m.m[i] * w;
        }
        total += w;
    }
    if (total <= 0) {
        return fallback;
    }
    for (float& v : out.m) {
        v /= total;
    }
    return out;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void finishTri(Summary& s, Tri& t, bool& firstBound)
{
    for (const Corner& c : t.c) {
        if (!finite(c.pos) || !finite(c.nrm) || !std::isfinite(c.uv0.x) || !std::isfinite(c.uv0.y)) {
            ++s.nanValues;
        }
        s.bounds.extend(c.pos, firstBound);
    }
    if (length(cross(t.c[1].pos - t.c[0].pos, t.c[2].pos - t.c[0].pos)) <= 0.0f) {
        ++s.degenerate;
    }
    s.tris.push_back(t);
}

bool moved(const Mat4& a, const Mat4& b)
{
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > 1e-5f) {
            return true;
        }
    }
    return false;
}

// ---- TSShape animation ----

Quat seqRot(const Shape& s, const Sequence& q, int node, int k)
{
    if (!q.rotationMatters.test(node)) {
        return toQuat(s.defaultRotations[node]);
    }
    const size_t i = size_t(q.baseRotation) + size_t(q.rotationMatters.rank(node)) * q.numKeyframes + k;
    return i < s.nodeRotations.size() ? toQuat(s.nodeRotations[i]) : toQuat(s.defaultRotations[node]);
}

Vec3 seqTrans(const Shape& s, const Sequence& q, int node, int k)
{
    if (!q.translationMatters.test(node)) {
        return s.defaultTranslations[node];
    }
    const size_t i = size_t(q.baseTranslation) + size_t(q.translationMatters.rank(node)) * q.numKeyframes + k;
    return i < s.nodeTranslations.size() ? s.nodeTranslations[i] : s.defaultTranslations[node];
}

} // namespace

Summary summarize(const Shape& shape, const DetailFilter& filter)
{
    Summary s;
    const std::vector<Mat4> world = shape.defaultWorldTransforms();
    s.tsObjects = true;
    for (size_t i = 0; i < shape.nodes.size(); ++i) {
        const int p = shape.nodes[i].parentIndex;
        NodePose n;
        n.name = shape.name(shape.nodes[i].nameIndex);
        n.parent = p >= 0 && p < int(shape.nodes.size()) ? shape.name(shape.nodes[p].nameIndex) : "";
        n.world = world[i];
        s.nodes.push_back(std::move(n));
    }
    for (const Object& o : shape.objects) {
        const bool owned = o.nodeIndex >= 0 && o.nodeIndex < int(shape.nodes.size());
        s.objects.push_back({shape.name(o.nameIndex), owned ? shape.name(shape.nodes[o.nodeIndex].nameIndex) : ""});
    }
    for (const Detail& d : shape.details) {
        if (d.subShapeNum >= 0) { // billboard details hold no meshes
            s.details.push_back({shape.name(d.nameIndex), d.size});
        }
    }
    for (const Material& m : shape.materials) {
        s.materials.push_back(m.name);
    }

    const int highestVisible = shape.highestVisibleDetail();
    bool firstBound = true;
    for (int o = 0; o < int(shape.objects.size()); ++o) {
        const Object& obj = shape.objects[o];
        for (int d = 0; d < int(shape.details.size()); ++d) {
            const Detail& det = shape.details[d];
            if ((!filter.allDetails && d != highestVisible) || (det.size < 0 && !filter.collision)) {
                continue;
            }
            const Mesh* found = shape.detailMesh(o, d);
            if (!found) {
                continue;
            }
            Mesh m = *found;
            convertToTriangles(m);
            const bool skin = m.isSkin();
            const std::vector<Vec3>& verts = m.restVerts();
            const std::vector<Vec3>& norms = m.restNorms();
            const size_t nv = m.vertsPerFrame > 0 ? std::min(size_t(m.vertsPerFrame), verts.size()) : verts.size();
            s.vertexCount += nv;
            s.morphTargets += std::max(0, m.numFrames - 1);
            s.hasNormals = s.hasNormals || norms.size() >= nv;
            s.hasUv0 = s.hasUv0 || m.tverts.size() >= nv;
            s.hasUv1 = s.hasUv1 || (!m.tverts2.empty() && m.tverts2.size() >= nv);
            s.hasColors = s.hasColors || (!m.colors.empty() && m.colors.size() >= nv);
            s.hasTangents = s.hasTangents || (!m.tangents.empty() && m.tangents.size() >= nv);

            std::vector<Mat4> vertexXf(nv, obj.nodeIndex >= 0 && obj.nodeIndex < int(world.size()) ? world[obj.nodeIndex] : Mat4{});
            std::vector<int> skinIndex(nv, -1);
            if (skin) {
                ++s.skinnedMeshes;
                std::vector<Mat4> bones(m.initialTransforms.size());
                for (size_t b = 0; b < bones.size(); ++b) {
                    const int node = b < m.nodeIndex.size() ? m.nodeIndex[b] : -1;
                    bones[b] = (node >= 0 && node < int(world.size()) ? world[node] : Mat4{}) * toMat4(m.initialTransforms[b]);
                }
                std::vector<std::vector<std::pair<Mat4, float>>> parts(nv);
                std::vector<std::map<std::string, float>> byName(nv);
                const size_t n = std::min({m.vertexIndex.size(), m.boneIndex.size(), m.weight.size()});
                for (size_t i = 0; i < n; ++i) {
                    const int v = m.vertexIndex[i], b = m.boneIndex[i];
                    if (v < 0 || v >= int(nv) || b < 0 || b >= int(bones.size())) {
                        continue;
                    }
                    parts[v].push_back({bones[b], m.weight[i]});
                    const int node = b < int(m.nodeIndex.size()) ? m.nodeIndex[b] : -1;
                    const std::string bone = node >= 0 && node < int(shape.nodes.size()) ? shape.name(shape.nodes[node].nameIndex) : "?";
                    byName[v][bone] += m.weight[i];
                }
                for (size_t v = 0; v < nv; ++v) {
                    vertexXf[v] = blend(parts[v], Mat4{});
                    skinIndex[v] = int(s.influences.size());
                    std::vector<Influence> list;
                    for (const auto& [bone, w] : byName[v]) {
                        list.push_back({bone, w});
                    }
                    s.influences.push_back(std::move(list));
                }
            }

            for (const Primitive& p : m.primitives) {
                const int material = p.hasMaterial() && p.material() < shape.materials.size() ? int(p.material()) : -1;
                for (int i = p.start; i + 2 < p.start + p.numElements; i += 3) {
                    Tri t;
                    t.material = material;
                    t.object = o;
                    t.group = filter.perDetail ? int(det.size) : 0;
                    bool valid = true;
                    for (int k = 0; k < 3; ++k) {
                        const uint32_t v = m.indices[size_t(i) + k];
                        if (v >= nv) {
                            valid = false;
                            break;
                        }
                        const Mat4& xf = vertexXf[v];
                        Corner& c = t.c[k];
                        c.pos = mulPoint(xf, verts[v]);
                        c.nrm = v < norms.size() ? normalize(mulVector(normalMatrix(xf), norms[v])) : Vec3{};
                        c.uv0 = v < m.tverts.size() ? m.tverts[v] : Vec2{};
                        c.uv1 = v < m.tverts2.size() ? m.tverts2[v] : Vec2{};
                        c.color = v < m.colors.size() ? m.colors[v] : Color8{};
                        if (v < m.tangents.size()) {
                            const Vec4& tg = m.tangents[v];
                            const Vec3 tv = normalize(mulVector(xf, {tg.x, tg.y, tg.z}));
                            c.tangent = {tv.x, tv.y, tv.z, tg.w < 0 ? -1.0f : 1.0f};
                        }
                        c.skin = skinIndex[v];
                    }
                    if (valid) {
                        finishTri(s, t, firstBound);
                    }
                }
            }
        }
    }

    for (const Sequence& q : shape.sequences) {
        AnimSample a;
        a.name = shape.name(q.nameIndex);
        a.duration = q.duration;
        a.keyframes = q.numKeyframes;
        const int K = std::max(1, q.numKeyframes);
        std::vector<std::vector<Mat4>> perNode(shape.nodes.size());
        for (int smp = 0; smp < kAnimSamples; ++smp) {
            const float pos = K > 1 ? float(smp) * float(K - 1) / float(kAnimSamples - 1) : 0.0f;
            const int k0 = std::min(int(pos), K - 1), k1 = std::min(k0 + 1, K - 1);
            const float f = pos - float(k0);
            const Mat4 ground = groundTransform(shape, q, float(smp) * q.duration / float(kAnimSamples - 1));
            std::vector<Mat4> w(shape.nodes.size());
            for (size_t n = 0; n < shape.nodes.size(); ++n) {
                const Quat r = slerp(seqRot(shape, q, int(n), k0), seqRot(shape, q, int(n), k1), f);
                const Vec3 t0 = seqTrans(shape, q, int(n), k0), t1 = seqTrans(shape, q, int(n), k1);
                const Mat4 local = composeTRS(t0 + (t1 - t0) * f, r, {1, 1, 1});
                const int p = shape.nodes[n].parentIndex;
                w[n] = p >= 0 && p < int(n) ? w[p] * local : ground * local;
                perNode[n].push_back(w[n]);
            }
        }
        for (size_t n = 0; n < shape.nodes.size(); ++n) {
            bool any = false;
            for (const Mat4& m : perNode[n]) {
                any = any || moved(m, world[n]);
            }
            if (any) {
                a.nodes.push_back(shape.name(shape.nodes[n].nameIndex));
                a.rigid.push_back(std::move(perNode[n]));
            }
        }
        s.anims.push_back(std::move(a));
    }
    return s;
}

Summary summarize(const aiScene& scene, const SceneNames& names)
{
    if (names.meshNameOf && scene.mRootNode) {
        const std::unique_ptr<aiScene> folded = cloneScene(scene);
        foldMeshNodes(*folded, names.meshNameOf);
        SceneNames plain = names;
        plain.meshNameOf = nullptr;
        return summarize(*folded, plain);
    }
    Summary s;
    if (!scene.mRootNode) {
        return s;
    }
    for (unsigned i = 0; i < scene.mNumMaterials; ++i) {
        aiString name;
        scene.mMaterials[i]->Get(AI_MATKEY_NAME, name);
        s.materials.push_back(name.length || !names.tsNames ? name.C_Str() : "material" + std::to_string(i));
    }

    // Detail sizes always follow the TS builder's reading; object names only in TS mode.
    const ts::ObjectNaming naming = ts::objectNaming(scene, names.options);
    std::unordered_set<std::string> collapse;
    if (names.tsNames) {
        collapse.insert(naming.collapse.begin(), naming.collapse.end());
        collapse.insert(names.collapse.begin(), names.collapse.end());
    }
    s.tsObjects = names.tsNames;

    std::unordered_map<std::string, const aiNode*> byName;
    std::unordered_map<const aiNode*, Mat4> worldOf;
    std::unordered_map<const aiNode*, std::string> keptName; // node -> its name in `nodes`
    std::vector<const aiNode*> order;
    std::unordered_set<std::string> taken;
    auto walk = [&](auto& self, const aiNode* n, const Mat4& parent, const std::string& parentName) -> void {
        const Mat4 w = parent * toMat4(n->mTransformation);
        worldOf[n] = w;
        byName.emplace(n->mName.C_Str(), n);
        order.push_back(n);
        // A meshless root is an importer container; a root that holds meshes is a part (the TS builder keeps it too).
        const bool isRoot = n == scene.mRootNode;
        bool kept = !isRoot || isBaseStart(n) || n->mNumMeshes;
        kept = kept && !ts::generatedNode(*n, collapse);
        std::string childParent = parentName;
        if (kept) {
            NodePose pose;
            pose.name = n->mName.C_Str();
            if (names.tsNames) {
                const std::string unique = ts::uniqueName(pose.name.empty() ? "node" : pose.name, taken);
                if (unique != pose.name) {
                    s.renamed.push_back({pose.name, unique});
                }
                pose.name = unique;
            }
            pose.parent = parentName;
            pose.world = rigidPart(w);
            pose.meshHolder = n->mNumMeshes && !n->mNumChildren;
            pose.mirrored = determinant3(w) < 0.0f;
            keptName[n] = pose.name;
            childParent = pose.name;
            s.nodes.push_back(std::move(pose));
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i], w, childParent);
        }
    };
    walk(walk, scene.mRootNode, Mat4{}, "");

    // Object per mesh node: TS mode groups detail meshes into one object owned by its highest detail's node.
    auto ownerName = [&](const aiNode* n) {
        for (; n; n = n->mParent) {
            auto it = keptName.find(n);
            if (it != keptName.end()) {
                return it->second;
            }
        }
        return std::string();
    };
    std::unordered_map<const aiNode*, int> objectOf;
    std::set<float> sizes;
    {
        std::map<std::string, int> byObject;
        std::vector<int> ownerSize;
        std::set<int> skinned;
        for (const ts::MeshNodeObject& mn : naming.meshNodes) {
            if (naming.trailing) {
                sizes.insert(float(mn.size));
            }
            int index = int(s.objects.size());
            if (names.tsNames) {
                auto it = byObject.emplace(toLower(mn.object), index).first;
                index = it->second;
            }
            if (index == int(s.objects.size())) {
                s.objects.push_back({names.tsNames ? mn.object : std::string(mn.node->mName.C_Str()), ""});
                ownerSize.push_back(std::numeric_limits<int>::min());
            }
            objectOf[mn.node] = index;
            if (mn.size > ownerSize[index]) {
                ownerSize[index] = mn.size;
                s.objects[index].node = names.tsNames ? ownerName(mn.node) : std::string(mn.node->mName.C_Str());
            }
            for (unsigned k = 0; k < mn.node->mNumMeshes; ++k) {
                if (scene.mMeshes[mn.node->mMeshes[k]]->mNumBones > 0) {
                    skinned.insert(index);
                }
            }
        }
        // The TS builder leaves skinned objects without a node.
        for (int index : names.tsNames ? skinned : std::set<int>{}) {
            s.objects[index].node.clear();
        }
    }
    for (auto it = sizes.rbegin(); it != sizes.rend(); ++it) {
        s.details.push_back({"", *it});
    }

    bool firstBound = true;
    for (const aiNode* n : order) {
        if (!n->mNumMeshes) {
            continue;
        }
        const Mat4 nodeWorld = worldOf[n];
        auto obj = objectOf.find(n);
        const int objectIndex = obj != objectOf.end() ? obj->second : -1;
        for (unsigned k = 0; k < n->mNumMeshes; ++k) {
            const aiMesh* am = scene.mMeshes[n->mMeshes[k]];
            if (!(am->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
                continue;
            }
            const unsigned nv = am->mNumVertices;
            s.vertexCount += nv;
            s.morphTargets += int(am->mNumAnimMeshes);
            s.hasNormals = s.hasNormals || am->mNormals;
            s.hasUv0 = s.hasUv0 || am->HasTextureCoords(0);
            s.hasUv1 = s.hasUv1 || am->HasTextureCoords(1);
            s.hasColors = s.hasColors || am->HasVertexColors(0);
            s.hasTangents = s.hasTangents || am->HasTangentsAndBitangents();

            std::vector<Mat4> xf(nv, nodeWorld);
            std::vector<int> skinIndex(nv, -1);
            if (am->mNumBones) {
                ++s.skinnedMeshes;
                std::vector<std::vector<std::pair<Mat4, float>>> parts(nv);
                std::vector<std::map<std::string, float>> byBone(nv);
                for (unsigned b = 0; b < am->mNumBones; ++b) {
                    const aiBone* bone = am->mBones[b];
                    auto it = byName.find(bone->mName.C_Str());
                    const Mat4 boneWorld = it != byName.end() ? worldOf[it->second] : Mat4{};
                    const Mat4 m = boneWorld * toMat4(bone->mOffsetMatrix);
                    for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                        const aiVertexWeight& vw = bone->mWeights[w];
                        if (vw.mVertexId < nv && vw.mWeight > 0) {
                            parts[vw.mVertexId].push_back({m, vw.mWeight});
                            byBone[vw.mVertexId][bone->mName.C_Str()] += vw.mWeight;
                        }
                    }
                }
                for (unsigned v = 0; v < nv; ++v) {
                    xf[v] = blend(parts[v], nodeWorld);
                    float total = 0;
                    for (const auto& [b, w] : byBone[v]) {
                        total += w;
                    }
                    std::vector<Influence> list;
                    for (const auto& [b, w] : byBone[v]) {
                        list.push_back({b, total > 0 ? w / total : 0.0f});
                    }
                    skinIndex[v] = int(s.influences.size());
                    s.influences.push_back(std::move(list));
                }
            }

            auto corner = [&](unsigned v, Corner& c) {
                const Mat4& m = xf[v];
                c.pos = mulPoint(m, {am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z});
                const Mat4 nm = normalMatrix(m);
                if (am->mNormals) {
                    c.nrm = normalize(mulVector(nm, {am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z}));
                }
                if (am->HasTextureCoords(0)) {
                    c.uv0 = {am->mTextureCoords[0][v].x, 1.0f - am->mTextureCoords[0][v].y};
                }
                if (am->HasTextureCoords(1)) {
                    c.uv1 = {am->mTextureCoords[1][v].x, 1.0f - am->mTextureCoords[1][v].y};
                }
                if (am->HasVertexColors(0)) {
                    c.color = toColor8(am->mColors[0][v]);
                }
                if (am->HasTangentsAndBitangents()) {
                    const Vec3 t = normalize(mulVector(m, {am->mTangents[v].x, am->mTangents[v].y, am->mTangents[v].z}));
                    const Vec3 bt = normalize(mulVector(m, {am->mBitangents[v].x, am->mBitangents[v].y, am->mBitangents[v].z}));
                    c.tangent = {t.x, t.y, t.z, dot(cross(c.nrm, t), bt) < 0 ? -1.0f : 1.0f};
                }
                c.skin = skinIndex[v];
            };
            for (unsigned f = 0; f < am->mNumFaces; ++f) {
                const aiFace& face = am->mFaces[f];
                for (unsigned c = 1; c + 1 < face.mNumIndices; ++c) {
                    const unsigned a = face.mIndices[0], b = face.mIndices[c], d = face.mIndices[c + 1];
                    if (a >= nv || b >= nv || d >= nv) {
                        continue;
                    }
                    // TS winding: reversed, unless a mirrored transform already reverses it.
                    const bool mirrored = determinant3(xf[a]) < 0.0f;
                    Tri t;
                    t.material = int(am->mMaterialIndex);
                    t.object = objectIndex;
                    corner(a, t.c[0]);
                    corner(mirrored ? b : d, t.c[1]);
                    corner(mirrored ? d : b, t.c[2]);
                    finishTri(s, t, firstBound);
                }
            }
        }
    }

    for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
        const aiAnimation* anim = scene.mAnimations[a];
        const double tps = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 1.0;
        AnimSample smp;
        smp.name = anim->mName.C_Str();
        smp.duration = float(anim->mDuration / tps);
        std::unordered_map<const aiNode*, const aiNodeAnim*> channels;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            auto it = byName.find(anim->mChannels[c]->mNodeName.C_Str());
            if (it != byName.end()) {
                channels[it->second] = anim->mChannels[c];
                smp.keyframes = std::max<int>(smp.keyframes, int(std::max({anim->mChannels[c]->mNumPositionKeys,
                                                                            anim->mChannels[c]->mNumRotationKeys,
                                                                            anim->mChannels[c]->mNumScalingKeys})));
            }
        }
        std::vector<std::vector<Mat4>> perNode(order.size());
        for (int k = 0; k < kAnimSamples; ++k) {
            const double t = double(smp.duration) * k / (kAnimSamples - 1);
            std::unordered_map<const aiNode*, Mat4> w;
            for (size_t i = 0; i < order.size(); ++i) {
                const aiNode* n = order[i];
                Mat4 local = toMat4(n->mTransformation);
                auto ch = channels.find(n);
                if (ch != channels.end()) {
                    aiVector3D bs, bp;
                    aiQuaternion br;
                    n->mTransformation.Decompose(bs, br, bp);
                    const aiNodeAnim* c = ch->second;
                    const aiVector3D sc = sampleKeys(c->mScalingKeys, c->mNumScalingKeys, tps, t, bs);
                    const aiVector3D p = sampleKeys(c->mPositionKeys, c->mNumPositionKeys, tps, t, bp);
                    const aiQuaternion r = sampleKeys(c->mRotationKeys, c->mNumRotationKeys, tps, t, br);
                    local = composeTRS({p.x, p.y, p.z}, Quat{r.x, r.y, r.z, r.w}, {sc.x, sc.y, sc.z});
                }
                w[n] = n->mParent ? w[n->mParent] * local : local;
                perNode[i].push_back(rigidPart(w[n]));
            }
        }
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] == scene.mRootNode) {
                continue;
            }
            const Mat4 rest = rigidPart(worldOf[order[i]]);
            bool any = false;
            for (const Mat4& m : perNode[i]) {
                any = any || moved(m, rest);
            }
            if (any) {
                smp.nodes.push_back(order[i]->mName.C_Str());
                smp.rigid.push_back(std::move(perNode[i]));
            }
        }
        s.anims.push_back(std::move(smp));
    }
    return s;
}

} // namespace model
