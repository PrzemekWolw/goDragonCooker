#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <assimp/scene.h>

#include "core/strings.h"
#include "model/assimp/ai_bridge.h"
#include "model/assimp/ai_meta.h"

namespace ts {
namespace {

constexpr int kMaxBonesPerVertex = 16;

Mat4 toMat4(const aiMatrix4x4& a)
{
    Mat4 m;
    const float src[16] = {a.a1, a.a2, a.a3, a.a4, a.b1, a.b2, a.b3, a.b4, a.c1, a.c2, a.c3, a.c4, a.d1, a.d2, a.d3, a.d4};
    std::memcpy(m.m, src, sizeof(src));
    return m;
}

Vec3 toVec3(const aiVector3D& v) { return {v.x, v.y, v.z}; }

// Scale-free, de-reflected rotation + translation of a node world matrix, as the engine stores nodes.
Mat4 rigidPart(const Mat4& world, bool* mirrored = nullptr)
{
    Vec3 t, s;
    Quat r;
    decompose(world, t, r, s);
    if (mirrored) {
        *mirrored = determinant3(world) < 0.0f;
    }
    return composeTRS(t, r, {1, 1, 1});
}

Mat4 normalMatrix(const Mat4& m)
{
    Mat4 n = inverse(m);
    Mat4 t;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            t.at(r, c) = n.at(c, r);
        }
    }
    return t;
}

std::string uniqueName(const std::string& base, std::unordered_set<std::string>& taken)
{
    for (int attempt = 0; attempt < 0x10000; ++attempt) {
        std::string candidate = base;
        for (int v = attempt; v != 0; v >>= 4) {
            candidate += char('A' + (v & 0xF));
        }
        if (taken.insert(lowerAscii(candidate)).second) {
            return candidate;
        }
    }
    return base;
}

bool startsWithI(const std::string& s, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    return s.size() >= n && lowerAscii(s.substr(0, n)) == lowerAscii(prefix);
}

struct NodeInfo {
    const aiNode* ai = nullptr;
    Mat4 world;
    Mat4 rigid;
    bool mirrored = false;
    int ts = -1;     // shape node index, -1 when the node collapses into its parent
    int parent = -1; // index into the NodeInfo list
};

struct MeshSource {
    int node = -1; // NodeInfo index
    int size = 0;
};

struct ObjectBuild {
    std::string name;
    std::vector<MeshSource> meshes;
    int owner = -1; // NodeInfo index of the highest detail
    int ownerSize = std::numeric_limits<int>::min();
    bool hasSkin = false;
    int tsIndex = -1;
};

struct Converter {
    const aiScene& scene;
    const AiToTsOptions& opt;
    Shape& shape;
    std::vector<std::string>& warnings;

    std::vector<NodeInfo> nodes;
    std::unordered_map<const aiNode*, int> infoOf;
    std::unordered_map<std::string, int> infoByName; // authored aiNode name -> first NodeInfo
    std::vector<ObjectBuild> objects;
    std::unordered_map<unsigned, uint32_t> materialMap;
    std::vector<Mat4> tsWorld;
    const aiNode* boundsNode = nullptr;
    bool trailing = false;
    nlohmann::json meta;
    bool haveMeta = false;
    std::map<int, std::string> markerDetailNames; // size -> authored marker name
    std::map<int, std::string> billboardMarkers;
    std::unordered_set<std::string> collapseNames;                          // generated nodes, not shape nodes
    std::unordered_map<std::string, std::pair<std::string, int>> meshNodes; // node -> (object, size)

    void warn(const std::string& w) { warnings.push_back(w); }

    bool hasBaseStartLayout() const
    {
        const aiNode* root = scene.mRootNode;
        for (unsigned i = 0; i < root->mNumChildren; ++i) {
            const aiNode* base = root->mChildren[i];
            if (!startsWithI(base->mName.C_Str(), "base")) {
                continue;
            }
            for (unsigned j = 0; j < base->mNumChildren; ++j) {
                if (startsWithI(base->mChildren[j]->mName.C_Str(), "start")) {
                    return true;
                }
            }
        }
        return false;
    }

    void collectNodes(const aiNode* n, int parent, const Mat4& parentWorld)
    {
        NodeInfo info;
        info.ai = n;
        info.parent = parent;
        info.world = parentWorld * toMat4(n->mTransformation);
        info.rigid = rigidPart(info.world, &info.mirrored);
        const int self = int(nodes.size());
        nodes.push_back(info);
        infoOf[n] = self;
        infoByName.emplace(n->mName.C_Str(), self);
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            collectNodes(n->mChildren[i], self, info.world);
        }
    }

    // Object name and detail size a mesh node contributes (engine deriveObjectDetail semantics).
    void objectDetail(const NodeInfo& info, std::string& name, int& size) const
    {
        const aiNode* n = info.ai;
        std::string detailNode = n->mName.C_Str();
        auto known = meshNodes.find(detailNode);
        if (known != meshNodes.end()) {
            name = known->second.first;
            size = trailing ? known->second.second : opt.singleDetailSize;
            return;
        }
        if (!trailing) {
            name = detailNode.empty() ? "mesh" : detailNode;
            size = opt.singleDetailSize;
            return;
        }
        if ((detailNode == "null" || hasSuffix(detailNode, "PIVOT") || hasPrefix(detailNode, "$ColladaAutoName$")) && n->mParent) {
            detailNode = n->mParent->mName.C_Str();
        }
        size = 2;
        name = splitTrailingNumber(detailNode, size);
        if (name.empty()) {
            name = "mesh";
        }
        if (size > 0 && (hasPrefix(detailNode, "Collision") || hasPrefix(detailNode, "LOSCol"))) {
            size = -size;
        }
    }

    void buildNodes()
    {
        std::unordered_set<std::string> taken;
        for (NodeInfo& info : nodes) {
            const aiNode* n = info.ai;
            const bool isRoot = n == scene.mRootNode;
            if ((isRoot && n->mNumMeshes == 0) || collapseNames.count(n->mName.C_Str())) {
                continue;
            }
            int parentTs = -1;
            const Mat4* parentRigid = nullptr;
            for (int p = info.parent; p >= 0; p = nodes[p].parent) {
                if (nodes[p].ts >= 0) {
                    parentTs = nodes[p].ts;
                    parentRigid = &nodes[p].rigid;
                    break;
                }
            }
            const Mat4 local = parentRigid ? inverse(*parentRigid) * info.rigid : info.rigid;
            Vec3 t, s;
            Quat r;
            decompose(local, t, r, s);
            Node node;
            std::string name = n->mName.length ? n->mName.C_Str() : "node";
            node.nameIndex = shape.addName(uniqueName(name, taken));
            node.parentIndex = parentTs;
            info.ts = int(shape.nodes.size());
            shape.nodes.push_back(node);
            shape.defaultRotations.push_back(toQuat16(r));
            shape.defaultTranslations.push_back(t);
        }
        tsWorld = shape.defaultWorldTransforms();
    }

    void collectMarkers()
    {
        if (!trailing) {
            return;
        }
        for (const NodeInfo& info : nodes) {
            const aiNode* n = info.ai;
            if (n->mNumMeshes || n->mNumChildren || n == scene.mRootNode) {
                continue;
            }
            const std::string name = n->mName.C_Str();
            int size = std::numeric_limits<int>::min();
            const std::string base = splitTrailingNumber(name, size);
            if (size == std::numeric_limits<int>::min()) {
                continue;
            }
            if (startsWithI(name, "bb_") || startsWithI(name, "bb::") || startsWithI(name, "bbz_") || startsWithI(name, "bbz::")) {
                if (size > 0) {
                    billboardMarkers[size] = name;
                }
            } else if (!base.empty()) {
                markerDetailNames.emplace(size, name);
            }
        }
    }

    void collectObjects()
    {
        std::unordered_map<std::string, int> byName;
        std::unordered_set<std::string> takenObjectDetails;
        for (int i = 0; i < int(nodes.size()); ++i) {
            const NodeInfo& info = nodes[i];
            if (!info.ai->mNumMeshes) {
                continue;
            }
            if (boundsNode && info.ai == boundsNode) {
                continue;
            }
            std::string name;
            int size = 2;
            objectDetail(info, name, size);
            // A second mesh at an occupied (name, size) is a different object, not a LOD.
            std::string unique = name;
            for (int attempt = 0; attempt < 0x10000; ++attempt) {
                std::string candidate = name;
                for (int v = attempt; v != 0; v >>= 4) {
                    candidate += char('A' + (v & 0xF));
                }
                if (takenObjectDetails.insert(lowerAscii(candidate) + '\x1f' + std::to_string(size)).second) {
                    unique = candidate;
                    break;
                }
            }
            const std::string key = lowerAscii(unique);
            auto it = byName.find(key);
            if (it == byName.end()) {
                it = byName.emplace(key, int(objects.size())).first;
                objects.push_back({});
                objects.back().name = unique;
            }
            ObjectBuild& ob = objects[it->second];
            ob.meshes.push_back({i, size});
            if (size > ob.ownerSize) {
                ob.ownerSize = size;
                ob.owner = i;
            }
            for (unsigned m = 0; m < info.ai->mNumMeshes; ++m) {
                ob.hasSkin = ob.hasSkin || scene.mMeshes[info.ai->mMeshes[m]]->mNumBones > 0;
            }
        }
    }

    uint32_t materialIndex(unsigned aiIndex)
    {
        auto it = materialMap.find(aiIndex);
        if (it != materialMap.end()) {
            return it->second;
        }
        Material mat;
        if (aiIndex < scene.mNumMaterials) {
            aiString name;
            if (scene.mMaterials[aiIndex]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
                mat.name = name.C_Str();
            }
        }
        if (mat.name.empty()) {
            mat.name = format("material%u", aiIndex);
        }
        const uint32_t index = uint32_t(shape.materials.size());
        shape.materials.push_back(mat);
        materialMap.emplace(aiIndex, index);
        return index;
    }

    int tsNodeOf(int info) const
    {
        for (int p = info; p >= 0; p = nodes[p].parent) {
            if (nodes[p].ts >= 0) {
                return nodes[p].ts;
            }
        }
        return -1;
    }

    static void computeNormals(Mesh& m, size_t first, size_t count, const std::vector<uint32_t>& tris)
    {
        // Area-weighted face normals shared by vertices at the same position (GenSmoothNormals).
        std::map<std::tuple<float, float, float>, Vec3> byPos;
        auto key = [&](uint32_t v) { return std::make_tuple(m.verts[v].x, m.verts[v].y, m.verts[v].z); };
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const Vec3 a = m.verts[tris[i]], b = m.verts[tris[i + 1]], c = m.verts[tris[i + 2]];
            // TS winding is clockwise seen from the front.
            const Vec3 n = cross(c - a, b - a);
            for (int k = 0; k < 3; ++k) {
                byPos[key(tris[i + k])] += n;
            }
        }
        for (size_t v = first; v < first + count; ++v) {
            m.norms[v] = normalize(byPos[key(uint32_t(v))]);
            if (dot(m.norms[v], m.norms[v]) == 0.0f) {
                m.norms[v] = {0, 0, 1};
            }
        }
    }

    static void computeTangents(Mesh& m, size_t first, size_t count, const std::vector<uint32_t>& tris)
    {
        std::vector<Vec3> tan(count), bit(count);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            const uint32_t ia = tris[i], ib = tris[i + 1], ic = tris[i + 2];
            if (ia < first || ia >= first + count) {
                continue; // another submesh
            }
            const Vec3 e1 = m.verts[ib] - m.verts[ia], e2 = m.verts[ic] - m.verts[ia];
            const float du1 = m.tverts[ib].x - m.tverts[ia].x, dv1 = m.tverts[ib].y - m.tverts[ia].y;
            const float du2 = m.tverts[ic].x - m.tverts[ia].x, dv2 = m.tverts[ic].y - m.tverts[ia].y;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-20f) {
                continue;
            }
            const float r = 1.0f / det;
            const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
            const Vec3 b = (e2 * du1 - e1 * du2) * r;
            for (uint32_t v : {ia, ib, ic}) {
                tan[v - first] += t;
                bit[v - first] += b;
            }
        }
        for (size_t k = 0; k < count; ++k) {
            const Vec3 n = m.norms[first + k];
            Vec3 t = tan[k] - n * dot(n, tan[k]);
            if (dot(t, t) < 1e-24f) {
                m.tangents[first + k] = {1, 0, 0, 1};
                continue;
            }
            t = normalize(t);
            const float w = dot(cross(n, t), bit[k]) < 0.0f ? -1.0f : 1.0f;
            m.tangents[first + k] = {t.x, t.y, t.z, w};
        }
    }

    // One TS mesh from every aiMesh on a node.
    void buildMesh(const ObjectBuild& ob, const MeshSource& src, Mesh& m, int objectIndex)
    {
        const NodeInfo& info = nodes[src.node];
        const aiNode* n = info.ai;
        bool skin = false, anyUv2 = false, anyColor = false;
        for (unsigned k = 0; k < n->mNumMeshes; ++k) {
            const aiMesh* am = scene.mMeshes[n->mMeshes[k]];
            skin = skin || am->mNumBones > 0;
            anyUv2 = anyUv2 || am->HasTextureCoords(1);
            anyColor = anyColor || am->HasVertexColors(0);
        }
        m.type = skin ? MeshType::Skin : MeshType::Standard;
        if (startsWithI(ob.name, "BB_") || startsWithI(ob.name, "BB::")) {
            m.flags |= Billboard;
        } else if (startsWithI(ob.name, "BBZ_") || startsWithI(ob.name, "BBZ::")) {
            m.flags |= Billboard | BillboardZAxis;
        }

        // Rigid meshes are baked into their object node's frame (node scale stays in the verts);
        // skins stay in mesh space and reach the shape through their bone matrices.
        Mat4 offset;
        if (!skin) {
            if (ob.hasSkin) {
                offset = info.world;
            } else {
                offset = inverse(nodes[ob.owner].rigid) * info.world;
            }
        }
        const bool mirrored = determinant3(offset) < 0.0f;
        const Mat4 nrmMat = normalMatrix(offset);

        // Morph targets become mesh frames when every submesh has the same number of targets.
        int morphTargets = -1;
        for (unsigned k = 0; k < n->mNumMeshes; ++k) {
            const aiMesh* am = scene.mMeshes[n->mMeshes[k]];
            const int count = int(am->mNumAnimMeshes);
            if (morphTargets == -1) {
                morphTargets = count;
            } else if (morphTargets != count) {
                morphTargets = -2;
            }
        }
        if (morphTargets < 0 || skin) {
            if (morphTargets > 0 && skin) {
                warn(format("%s: morph targets on a skinned mesh are not supported by TSShape; dropped", ob.name.c_str()));
            } else if (morphTargets == -2) {
                warn(format("%s: submeshes have different morph target counts; morphs dropped", ob.name.c_str()));
            }
            morphTargets = 0;
        }

        std::vector<uint32_t> nodeBones; // TS node per bone slot
        struct Influence {
            int bone;
            float w;
        };
        std::vector<std::vector<Influence>> influences;
        std::vector<Vec3> verts, norms;
        std::vector<std::vector<Vec3>> frameVerts(static_cast<size_t>(morphTargets));
        std::vector<std::vector<Vec3>> frameNorms(static_cast<size_t>(morphTargets));
        bool missingNormals = false, missingTangents = false;
        std::vector<std::pair<size_t, size_t>> needNormals, needTangents;
        std::vector<uint32_t> allTris;

        for (unsigned k = 0; k < n->mNumMeshes; ++k) {
            const aiMesh* am = scene.mMeshes[n->mMeshes[k]];
            if (!(am->mPrimitiveTypes & (aiPrimitiveType_TRIANGLE | aiPrimitiveType_POLYGON))) {
                if (am->mNumFaces) {
                    warn(format("%s: point/line mesh '%s' skipped (TSShape draws triangles only)", ob.name.c_str(), am->mName.C_Str()));
                }
                continue;
            }
            const size_t base = verts.size();
            const size_t count = am->mNumVertices;
            for (size_t v = 0; v < count; ++v) {
                verts.push_back(mulPoint(offset, toVec3(am->mVertices[v])));
                norms.push_back(am->mNormals ? normalize(mulVector(nrmMat, toVec3(am->mNormals[v]))) : Vec3{});
                Vec2 uv0{0, 0}, uv1{0, 0};
                if (am->HasTextureCoords(0)) {
                    uv0 = {am->mTextureCoords[0][v].x, 1.0f - am->mTextureCoords[0][v].y};
                }
                if (am->HasTextureCoords(1)) {
                    uv1 = {am->mTextureCoords[1][v].x, 1.0f - am->mTextureCoords[1][v].y};
                }
                m.tverts.push_back(uv0);
                if (anyUv2) {
                    m.tverts2.push_back(uv1);
                }
                if (anyColor) {
                    Color8 c;
                    if (am->HasVertexColors(0)) {
                        const aiColor4D& vc = am->mColors[0][v];
                        auto b = [](float x) { return uint8_t(std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f)); };
                        c = {b(vc.r), b(vc.g), b(vc.b), b(vc.a)};
                    }
                    m.colors.push_back(c);
                }
                Vec4 tangent{1, 0, 0, 1};
                if (am->HasTangentsAndBitangents() && am->mNormals) {
                    const Vec3 t = normalize(mulVector(offset, toVec3(am->mTangents[v])));
                    const Vec3 b = normalize(mulVector(offset, toVec3(am->mBitangents[v])));
                    const Vec3 nn = norms.back();
                    tangent = {t.x, t.y, t.z, dot(cross(nn, t), b) < 0.0f ? -1.0f : 1.0f};
                }
                m.tangents.push_back(tangent);
            }
            for (int f = 0; f < morphTargets; ++f) {
                const aiAnimMesh* morph = am->mAnimMeshes[f];
                for (size_t v = 0; v < count; ++v) {
                    const bool hasV = morph->mVertices && morph->mNumVertices == count;
                    const bool hasN = morph->mNormals && morph->mNumVertices == count;
                    frameVerts[f].push_back(hasV ? mulPoint(offset, toVec3(morph->mVertices[v])) : verts[base + v]);
                    frameNorms[f].push_back(hasN ? normalize(mulVector(nrmMat, toVec3(morph->mNormals[v]))) : norms[base + v]);
                }
            }
            if (!am->mNormals) {
                missingNormals = true;
            }
            if (!am->HasTangentsAndBitangents() || !am->mNormals) {
                missingTangents = true;
            }

            Primitive prim;
            prim.start = int32_t(m.indices.size());
            std::vector<uint32_t> tris;
            for (unsigned fi = 0; fi < am->mNumFaces; ++fi) {
                const aiFace& face = am->mFaces[fi];
                for (unsigned c = 1; c + 1 < face.mNumIndices; ++c) {
                    const uint32_t a = face.mIndices[0] + uint32_t(base);
                    const uint32_t b = face.mIndices[c] + uint32_t(base);
                    const uint32_t d = face.mIndices[c + 1] + uint32_t(base);
                    tris.push_back(a);
                    tris.push_back(mirrored ? b : d);
                    tris.push_back(mirrored ? d : b);
                }
            }
            m.indices.insert(m.indices.end(), tris.begin(), tris.end());
            allTris.insert(allTris.end(), tris.begin(), tris.end());
            prim.numElements = int32_t(tris.size());
            prim.matIndex = Primitive::Triangles | Primitive::Indexed | materialIndex(am->mMaterialIndex);
            m.primitives.push_back(prim);
            if (!am->mNormals) {
                needNormals.push_back({base, count});
            }
            if (!am->HasTangentsAndBitangents() || !am->mNormals) {
                needTangents.push_back({base, count});
            }

            if (skin) {
                influences.resize(verts.size());
                for (unsigned b = 0; b < am->mNumBones; ++b) {
                    const aiBone* bone = am->mBones[b];
                    auto found = infoByName.find(bone->mName.C_Str());
                    int tsNode = found != infoByName.end() ? tsNodeOf(found->second) : -1;
                    if (tsNode < 0) {
                        warn(format("%s: bone '%s' has no node; bound to node 0", ob.name.c_str(), bone->mName.C_Str()));
                        tsNode = 0;
                    }
                    const Mat4 boneWorld = found != infoByName.end() ? nodes[found->second].world : Mat4{};
                    const Mat4 bind = inverse(tsWorld[tsNode]) * boneWorld * toMat4(bone->mOffsetMatrix);
                    const Mat4Raw raw = toRaw(bind);
                    int slot = 0;
                    while (slot < int(nodeBones.size()) &&
                           (int(nodeBones[slot]) != tsNode || std::memcmp(m.initialTransforms[slot].m, raw.m, sizeof(raw.m)) != 0)) {
                        ++slot;
                    }
                    if (slot == int(nodeBones.size())) {
                        nodeBones.push_back(uint32_t(tsNode));
                        m.initialTransforms.push_back(raw);
                    }
                    for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                        const aiVertexWeight& vw = bone->mWeights[w];
                        if (vw.mWeight <= 0.0f || vw.mVertexId >= count) {
                            continue;
                        }
                        influences[base + vw.mVertexId].push_back({slot, vw.mWeight});
                    }
                }
            }
        }
        (void)missingNormals;
        (void)missingTangents;

        m.verts = verts;
        m.norms = norms;
        for (const auto& [first, count] : needNormals) {
            computeNormals(m, first, count, allTris);
        }
        for (const auto& [first, count] : needTangents) {
            computeTangents(m, first, count, allTris);
        }
        const int numVerts = int(m.verts.size());
        m.vertsPerFrame = numVerts;
        m.numFrames = 1 + morphTargets;
        m.numMatFrames = 1;
        for (int f = 0; f < morphTargets; ++f) {
            m.verts.insert(m.verts.end(), frameVerts[f].begin(), frameVerts[f].end());
            m.norms.insert(m.norms.end(), frameNorms[f].begin(), frameNorms[f].end());
        }
        if (morphTargets > 0) {
            morphObjects[objectIndex] = morphTargets;
        }

        if (skin) {
            influences.resize(size_t(numVerts));
            int rigidSlot = -1;
            const int meshTsNode = tsNodeOf(src.node);
            size_t capped = 0;
            for (int v = 0; v < numVerts; ++v) {
                auto& list = influences[v];
                if (list.empty()) {
                    if (rigidSlot < 0) {
                        rigidSlot = int(nodeBones.size());
                        const int node = meshTsNode >= 0 ? meshTsNode : 0;
                        nodeBones.push_back(uint32_t(node));
                        m.initialTransforms.push_back(toRaw(inverse(tsWorld[node]) * info.world));
                    }
                    list.push_back({rigidSlot, 1.0f});
                }
                if (list.size() > size_t(kMaxBonesPerVertex)) {
                    std::nth_element(list.begin(), list.begin() + kMaxBonesPerVertex, list.end(),
                                     [](const Influence& a, const Influence& b) { return a.w > b.w; });
                    list.resize(kMaxBonesPerVertex);
                    ++capped;
                }
                float total = 0;
                for (const Influence& in : list) {
                    total += in.w;
                }
                for (const Influence& in : list) {
                    m.vertexIndex.push_back(v);
                    m.boneIndex.push_back(in.bone);
                    m.weight.push_back(total > 0 ? in.w / total : 0.0f);
                }
            }
            if (capped) {
                warn(format("%s: %zu vertices had more than %d bone influences; smallest dropped", ob.name.c_str(), capped,
                            kMaxBonesPerVertex));
            }
            m.nodeIndex.assign(nodeBones.begin(), nodeBones.end());
            m.initialVerts = m.verts;
            m.initialNorms = m.norms;
        }
    }

    std::unordered_map<int, int> morphObjects; // object index -> morph target count

    void buildObjectsAndDetails()
    {
        // Every distinct size becomes a detail; visible sizes first, largest first.
        std::vector<int> sizes;
        for (const ObjectBuild& ob : objects) {
            for (const MeshSource& s : ob.meshes) {
                if (std::find(sizes.begin(), sizes.end(), s.size) == sizes.end()) {
                    sizes.push_back(s.size);
                }
            }
        }
        std::sort(sizes.begin(), sizes.end(), std::greater<int>());

        shape.subShapeFirstNode = {0};
        shape.subShapeNumNodes = {int32_t(shape.nodes.size())};
        shape.subShapeFirstObject = {0};
        shape.subShapeNumObjects = {int32_t(objects.size())};

        for (int i = 0; i < int(objects.size()); ++i) {
            ObjectBuild& ob = objects[i];
            Object obj;
            obj.nameIndex = shape.addName(ob.name);
            obj.nodeIndex = ob.hasSkin ? -1 : tsNodeOf(ob.owner);
            obj.startMeshIndex = int32_t(shape.meshes.size());
            int last = -1;
            std::vector<Mesh> slots(sizes.size());
            for (const MeshSource& s : ob.meshes) {
                const int slot = int(std::find(sizes.begin(), sizes.end(), s.size) - sizes.begin());
                buildMesh(ob, s, slots[slot], i);
                last = std::max(last, slot);
            }
            obj.numMeshes = last + 1;
            for (int s = 0; s <= last; ++s) {
                shape.meshes.push_back(std::move(slots[s]));
            }
            ob.tsIndex = int(shape.objects.size());
            shape.objects.push_back(obj);
            shape.objectStates.push_back({});
        }

        // Billboard markers (BB_name<size>) are details with no subshape, before same-size mesh details.
        std::vector<std::pair<int, std::string>> billboards(billboardMarkers.begin(), billboardMarkers.end());
        size_t b = 0;
        std::sort(billboards.begin(), billboards.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (int i = 0; i < int(sizes.size()); ++i) {
            for (; b < billboards.size() && billboards[b].first >= sizes[i]; ++b) {
                addBillboard(billboards[b].first);
            }
            Detail d;
            const int size = sizes[i];
            std::string name;
            auto marker = markerDetailNames.find(size);
            if (marker != markerDetailNames.end()) {
                name = marker->second;
            } else if (size < 0) {
                name = format("%s%d", objectsStartWith("LOSCol") ? "LOS" : "Collision", size);
            } else {
                name = format("detail%d", size);
            }
            d.nameIndex = shape.addName(name);
            d.subShapeNum = 0;
            d.objectDetailNum = i;
            d.size = float(size);
            shape.details.push_back(d);
        }
        for (; b < billboards.size(); ++b) {
            addBillboard(billboards[b].first);
        }
        if (shape.details.empty() && !objects.empty()) {
            Detail d;
            d.nameIndex = shape.addName("detail2");
            d.size = 2;
            shape.details.push_back(d);
        }
    }

    bool objectsStartWith(const char* prefix) const
    {
        for (const ObjectBuild& ob : objects) {
            if (hasPrefix(ob.name, prefix)) {
                return true;
            }
        }
        return false;
    }

    void addBillboard(int size)
    {
        Detail d;
        d.nameIndex = shape.addName("bbDetail");
        d.subShapeNum = -1;
        d.objectDetailNum = 0;
        d.size = float(size);
        d.bbEquatorSteps = 4;
        d.bbPolarSteps = 0;
        d.bbDetailLevel = 0;
        d.bbDimension = 64;
        d.bbIncludePoles = 1;
        shape.details.push_back(d);
    }

    // --- animation ---

    struct Channel {
        const aiNodeAnim* anim = nullptr;
        double tps = 1.0;
    };
    struct Clip {
        std::string name;
        std::unordered_map<const aiNode*, Channel> byNode;
        std::unordered_map<std::string, std::pair<const aiMeshMorphAnim*, double>> morphs; // node name -> channel
        double endTime = 0.0;
        double minKeyDelta = 1000.0;
    };

    std::vector<Clip> collectClips()
    {
        std::vector<Clip> clips;
        std::unordered_map<std::string, const aiNode*> byName;
        for (const NodeInfo& info : nodes) {
            byName.emplace(info.ai->mName.C_Str(), info.ai);
        }
        for (unsigned a = 0; a < scene.mNumAnimations; ++a) {
            const aiAnimation* anim = scene.mAnimations[a];
            const double tps = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 1.0;
            Clip clip;
            clip.name = anim->mName.C_Str();
            auto scanTimes = [&](auto* keys, unsigned count) {
                for (unsigned k = 0; k < count; ++k) {
                    const double t = keys[k].mTime / tps;
                    clip.endTime = std::max(clip.endTime, t);
                    if (k) {
                        const double d = t - keys[k - 1].mTime / tps;
                        if (d > 0) {
                            clip.minKeyDelta = std::min(clip.minKeyDelta, d);
                        }
                    }
                }
            };
            for (unsigned c = 0; c < anim->mNumChannels; ++c) {
                const aiNodeAnim* ch = anim->mChannels[c];
                auto it = byName.find(ch->mNodeName.C_Str());
                if (it == byName.end()) {
                    continue;
                }
                clip.byNode[it->second] = {ch, tps};
                scanTimes(ch->mPositionKeys, ch->mNumPositionKeys);
                scanTimes(ch->mRotationKeys, ch->mNumRotationKeys);
                scanTimes(ch->mScalingKeys, ch->mNumScalingKeys);
            }
            for (unsigned c = 0; c < anim->mNumMorphMeshChannels; ++c) {
                const aiMeshMorphAnim* ch = anim->mMorphMeshChannels[c];
                std::string name = ch->mName.C_Str();
                const size_t star = name.find('*');
                if (star != std::string::npos) {
                    name = name.substr(0, star);
                }
                clip.morphs[name] = {ch, tps};
                scanTimes(ch->mKeys, ch->mNumKeys);
            }
            if (anim->mNumMeshChannels) {
                warn(format("animation '%s': vertex-animation mesh channels are not supported; ignored", clip.name.c_str()));
            }
            if (!clip.byNode.empty() || !clip.morphs.empty()) {
                clips.push_back(std::move(clip));
            }
        }
        return clips;
    }

    template <class Key, class V>
    static V sampleKeys(const Key* keys, unsigned count, double tps, double t, const V& absent)
    {
        if (!count) {
            return absent;
        }
        const Key* next = std::lower_bound(keys + 1, keys + count, t, [tps](const Key& k, double time) { return k.mTime / tps < time; });
        if (next == keys + count) {
            return keys[count - 1].mValue;
        }
        const Key* prev = next - 1;
        const double t0 = prev->mTime / tps, t1 = next->mTime / tps;
        if (t <= t0 || t1 <= t0) {
            return prev->mValue;
        }
        const float f = float((t - t0) / (t1 - t0));
        if constexpr (std::is_same_v<V, aiQuaternion>) {
            aiQuaternion out;
            aiQuaternion::Interpolate(out, prev->mValue, next->mValue, f);
            out.Normalize();
            return out;
        } else {
            return prev->mValue + (next->mValue - prev->mValue) * f;
        }
    }

    Mat4 animatedLocal(const aiNode* n, const Clip& clip, double t) const
    {
        auto it = clip.byNode.find(n);
        if (it == clip.byNode.end()) {
            return toMat4(n->mTransformation);
        }
        aiVector3D bindScale, bindPos;
        aiQuaternion bindRot;
        n->mTransformation.Decompose(bindScale, bindRot, bindPos);
        const Channel& ch = it->second;
        const aiVector3D s = sampleKeys(ch.anim->mScalingKeys, ch.anim->mNumScalingKeys, ch.tps, t, bindScale);
        const aiVector3D p = sampleKeys(ch.anim->mPositionKeys, ch.anim->mNumPositionKeys, ch.tps, t, bindPos);
        const aiQuaternion r = sampleKeys(ch.anim->mRotationKeys, ch.anim->mNumRotationKeys, ch.tps, t, bindRot);
        return composeTRS({p.x, p.y, p.z}, Quat{r.x, r.y, r.z, r.w}, {s.x, s.y, s.z});
    }

    void importSequences()
    {
        std::vector<Clip> clips = collectClips();
        if (clips.empty()) {
            return;
        }
        // Authored world scale per node: animated scale is stored relative to it, like the default pose.
        std::vector<Vec3> authoredScale(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            Vec3 t, s;
            Quat r;
            decompose(nodes[i].world, t, r, s);
            authoredScale[i] = s;
        }
        std::vector<int> order; // NodeInfo indices of shape nodes, by shape node index
        order.resize(shape.nodes.size(), -1);
        for (int i = 0; i < int(nodes.size()); ++i) {
            if (nodes[i].ts >= 0) {
                order[nodes[i].ts] = i;
            }
        }

        for (const Clip& clip : clips) {
            const int fps = std::clamp(int(1.0 / clip.minKeyDelta + 0.5), opt.minFps, opt.maxFps);
            const float duration = float(clip.endTime);
            const int numKeyframes = int(duration * fps + 0.5f) + 1;

            const size_t numNodes = shape.nodes.size();
            std::vector<std::vector<Quat>> rots(numNodes);
            std::vector<std::vector<Vec3>> trans(numNodes), scales(numNodes);
            std::vector<Mat4> world(nodes.size());
            for (int frame = 0; frame < numKeyframes; ++frame) {
                const double t = numKeyframes > 1 ? double(duration) * frame / (numKeyframes - 1) : 0.0;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    const Mat4 local = animatedLocal(nodes[i].ai, clip, t);
                    world[i] = nodes[i].parent >= 0 ? world[nodes[i].parent] * local : local;
                }
                auto rel = [&](int info) {
                    Mat4 m = world[info];
                    if (determinant3(m) < 0.0f) {
                        for (int r = 0; r < 3; ++r) {
                            m.at(r, 2) = -m.at(r, 2);
                        }
                    }
                    const Vec3& s = authoredScale[info];
                    const float inv[3] = {s.x ? 1.0f / std::fabs(s.x) : 0.0f, s.y ? 1.0f / std::fabs(s.y) : 0.0f,
                                          s.z ? 1.0f / std::fabs(s.z) : 0.0f};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) {
                            m.at(r, c) *= inv[c];
                        }
                    }
                    return m;
                };
                for (size_t tsn = 0; tsn < numNodes; ++tsn) {
                    const int info = order[tsn];
                    const int parentTs = shape.nodes[tsn].parentIndex;
                    Mat4 local = rel(info);
                    if (parentTs >= 0) {
                        local = inverse(rel(order[parentTs])) * local;
                    }
                    Vec3 tr, sc;
                    Quat q;
                    decompose(local, tr, q, sc);
                    rots[tsn].push_back(q);
                    trans[tsn].push_back(tr);
                    scales[tsn].push_back({std::fabs(sc.x), std::fabs(sc.y), std::fabs(sc.z)});
                }
            }

            Sequence seq;
            seq.nameIndex = shape.addName(clip.name.empty() ? "ambient" : clip.name);
            seq.flags = uint32_t(SequenceFlag::Cyclic);
            seq.priority = 5;
            seq.duration = duration;
            seq.numKeyframes = numKeyframes;
            bool anyAligned = false, anyUniform = false;
            auto approx = [](float a, float b) { return std::fabs(a - b) <= 1e-4f; };
            if (numKeyframes > 1) {
                for (size_t tsn = 0; tsn < numNodes; ++tsn) {
                    const Quat16 def = shape.defaultRotations[tsn];
                    const Vec3 defT = shape.defaultTranslations[tsn];
                    for (int f = 0; f < numKeyframes; ++f) {
                        if (!(toQuat16(rots[tsn][f]) == def)) {
                            seq.rotationMatters.set(int(tsn));
                            break;
                        }
                    }
                    for (int f = 0; f < numKeyframes; ++f) {
                        const Vec3& p = trans[tsn][f];
                        if (!approx(p.x, defT.x) || !approx(p.y, defT.y) || !approx(p.z, defT.z)) {
                            seq.translationMatters.set(int(tsn));
                            break;
                        }
                    }
                    for (int f = 0; f < numKeyframes; ++f) {
                        const Vec3& s = scales[tsn][f];
                        if (approx(s.x, 1) && approx(s.y, 1) && approx(s.z, 1)) {
                            continue;
                        }
                        if (approx(s.x, s.y) && approx(s.y, s.z)) {
                            anyUniform = true;
                        } else {
                            anyAligned = true;
                        }
                        seq.scaleMatters.set(int(tsn));
                        break;
                    }
                }
            }
            ScaleMode mode = opt.scale;
            if (seq.scaleMatters.any()) {
                if (mode == ScaleMode::Auto) {
                    mode = anyAligned ? ScaleMode::Aligned : ScaleMode::Uniform;
                } else if (mode == ScaleMode::Uniform && anyAligned) {
                    warn(format("sequence '%s': non-uniform scale averaged (uniform scale selected)", clip.name.c_str()));
                }
                seq.flags |= mode == ScaleMode::Uniform   ? uint32_t(SequenceFlag::UniformScale)
                             : mode == ScaleMode::Aligned ? uint32_t(SequenceFlag::AlignedScale)
                                                          : uint32_t(SequenceFlag::ArbitraryScale);
            }
            (void)anyUniform;

            seq.baseRotation = int32_t(shape.nodeRotations.size());
            seq.baseTranslation = int32_t(shape.nodeTranslations.size());
            seq.baseScale = mode == ScaleMode::Aligned     ? int32_t(shape.nodeAlignedScales.size())
                            : mode == ScaleMode::Arbitrary ? int32_t(shape.nodeArbitraryScaleFactors.size())
                                                           : int32_t(shape.nodeUniformScales.size());
            seq.baseObjectState = int32_t(shape.objectStates.size());
            seq.firstGroundFrame = int32_t(shape.groundTranslations.size());
            seq.firstTrigger = int32_t(shape.triggers.size());
            for (size_t tsn = 0; tsn < numNodes; ++tsn) {
                if (seq.rotationMatters.test(int(tsn))) {
                    for (int f = 0; f < numKeyframes; ++f) {
                        shape.nodeRotations.push_back(toQuat16(rots[tsn][f]));
                    }
                }
            }
            for (size_t tsn = 0; tsn < numNodes; ++tsn) {
                if (seq.translationMatters.test(int(tsn))) {
                    shape.nodeTranslations.insert(shape.nodeTranslations.end(), trans[tsn].begin(), trans[tsn].end());
                }
            }
            for (size_t tsn = 0; tsn < numNodes; ++tsn) {
                if (!seq.scaleMatters.test(int(tsn))) {
                    continue;
                }
                for (int f = 0; f < numKeyframes; ++f) {
                    const Vec3& s = scales[tsn][f];
                    if (mode == ScaleMode::Aligned) {
                        shape.nodeAlignedScales.push_back(s);
                    } else if (mode == ScaleMode::Arbitrary) {
                        shape.nodeArbitraryScaleFactors.push_back(s);
                        shape.nodeArbitraryScaleRots.push_back(Quat16{});
                    } else {
                        shape.nodeUniformScales.push_back((s.x + s.y + s.z) / 3.0f);
                    }
                }
            }

            // Morph weights pick the dominant target per keyframe (TSShape frames are discrete).
            for (const auto& [objIndex, targets] : morphObjects) {
                const ObjectBuild& ob = objects[objIndex];
                const aiMeshMorphAnim* ch = nullptr;
                double tps = 1.0;
                for (const MeshSource& s : ob.meshes) {
                    auto it = clip.morphs.find(nodes[s.node].ai->mName.C_Str());
                    if (it != clip.morphs.end()) {
                        ch = it->second.first;
                        tps = it->second.second;
                    }
                }
                if (!ch || !ch->mNumKeys) {
                    continue;
                }
                seq.frameMatters.set(ob.tsIndex);
                (void)targets;
            }
            if (seq.frameMatters.any()) {
                warn(format("sequence '%s': morph weights stored as discrete mesh frames (dominant target per keyframe)",
                            clip.name.c_str()));
                for (int objIndex = 0; objIndex < int(shape.objects.size()); ++objIndex) {
                    if (!seq.frameMatters.test(objIndex)) {
                        continue;
                    }
                    const ObjectBuild* ob = nullptr;
                    for (const ObjectBuild& o : objects) {
                        if (o.tsIndex == objIndex) {
                            ob = &o;
                        }
                    }
                    const aiMeshMorphAnim* ch = nullptr;
                    double tps = 1.0;
                    for (const MeshSource& s : ob->meshes) {
                        auto it = clip.morphs.find(nodes[s.node].ai->mName.C_Str());
                        if (it != clip.morphs.end()) {
                            ch = it->second.first;
                            tps = it->second.second;
                        }
                    }
                    for (int f = 0; f < numKeyframes; ++f) {
                        const double t = numKeyframes > 1 ? double(duration) * f / (numKeyframes - 1) : 0.0;
                        const aiMeshMorphKey* key = ch->mKeys;
                        for (unsigned k = 0; k < ch->mNumKeys; ++k) {
                            if (ch->mKeys[k].mTime / tps <= t + 1e-9) {
                                key = &ch->mKeys[k];
                            }
                        }
                        int frame = 0;
                        double best = 0.5;
                        for (unsigned v = 0; v < key->mNumValuesAndWeights; ++v) {
                            if (key->mWeights[v] > best) {
                                best = key->mWeights[v];
                                frame = int(key->mValues[v]) + 1;
                            }
                        }
                        ObjectState st;
                        st.frameIndex = frame;
                        shape.objectStates.push_back(st);
                    }
                }
            }
            shape.sequences.push_back(seq);
        }
    }

    // --- TSShape-only data saved by shapeToAssimp ---
    void applyMetadata()
    {
        if (!haveMeta) {
            return;
        }
        if (meta.contains("exporter") && meta["exporter"].is_number_integer()) {
            shape.exporterVersion = meta["exporter"].get<int>();
        }
        if (meta.contains("details") && meta["details"].is_array()) {
            // Restore names and billboard settings by size; add billboard details the scene can't express.
            for (const auto& jd : meta["details"]) {
                const float size = jd.value("size", 0.0f);
                const bool billboard = jd.contains("bb");
                Detail* target = nullptr;
                for (Detail& d : shape.details) {
                    if (d.size == size && (d.subShapeNum < 0) == billboard) {
                        target = &d;
                        break;
                    }
                }
                if (!target && billboard) {
                    addBillboard(int(size));
                    target = &shape.details.back();
                }
                if (!target) {
                    continue;
                }
                if (jd.contains("name")) {
                    target->nameIndex = shape.addName(jd["name"].get<std::string>());
                }
                if (billboard) {
                    const auto& bb = jd["bb"];
                    target->bbEquatorSteps = bb.value("equatorSteps", 4u);
                    target->bbPolarSteps = bb.value("polarSteps", 0u);
                    target->bbPolarAngle = bb.value("polarAngle", 0.0f);
                    target->bbDetailLevel = bb.value("detailLevel", 0);
                    target->bbDimension = bb.value("dimension", 64);
                    target->bbIncludePoles = bb.value("includePoles", 1u);
                }
            }
            std::stable_sort(shape.details.begin(), shape.details.end(), [](const Detail& a, const Detail& b) {
                return a.size > b.size || (a.size == b.size && a.subShapeNum < 0 && b.subShapeNum >= 0);
            });
        }
        if (meta.contains("materials") && meta["materials"].is_array()) {
            for (const auto& jm : meta["materials"]) {
                const std::string name = jm.value("name", "");
                for (Material& m : shape.materials) {
                    if (m.name == name) {
                        m.flags = jm.value("flags", m.flags);
                        m.detailScale = jm.value("detailScale", m.detailScale);
                        m.reflectionAmount = jm.value("reflectionAmount", m.reflectionAmount);
                    }
                }
            }
        }
        if (meta.contains("sequences") && meta["sequences"].is_array()) {
            for (const auto& js : meta["sequences"]) {
                const std::string name = js.value("name", "");
                for (Sequence& q : shape.sequences) {
                    if (lowerAscii(shape.name(q.nameIndex)) != lowerAscii(name)) {
                        continue;
                    }
                    const uint32_t keepScale = q.flags & 7u;
                    q.flags = (js.value("flags", q.flags) & ~7u) | keepScale;
                    q.priority = js.value("priority", q.priority);
                    q.toolBegin = js.value("toolBegin", q.toolBegin);
                    if (js.contains("triggers") && js["triggers"].is_array() && q.numTriggers == 0) {
                        q.firstTrigger = int32_t(shape.triggers.size());
                        for (const auto& jt : js["triggers"]) {
                            Trigger tr;
                            tr.state = jt.value("state", 0u);
                            tr.pos = jt.value("pos", 0.0f);
                            shape.triggers.push_back(tr);
                            ++q.numTriggers;
                        }
                    }
                }
            }
        }
    }

    void computeShapeBounds()
    {
        shape.computeBounds();
        if (!boundsNode || !boundsNode->mNumMeshes) {
            return;
        }
        const NodeInfo& info = nodes[infoOf[boundsNode]];
        bool first = true;
        for (unsigned k = 0; k < boundsNode->mNumMeshes; ++k) {
            const aiMesh* am = scene.mMeshes[boundsNode->mMeshes[k]];
            for (unsigned v = 0; v < am->mNumVertices; ++v) {
                const Vec3 p = mulPoint(info.world, toVec3(am->mVertices[v]));
                if (first) {
                    shape.bounds.min = shape.bounds.max = p;
                    first = false;
                } else {
                    shape.bounds.min = {std::min(shape.bounds.min.x, p.x), std::min(shape.bounds.min.y, p.y),
                                        std::min(shape.bounds.min.z, p.z)};
                    shape.bounds.max = {std::max(shape.bounds.max.x, p.x), std::max(shape.bounds.max.y, p.y),
                                        std::max(shape.bounds.max.z, p.z)};
                }
            }
        }
        if (!first) {
            shape.center = (shape.bounds.min + shape.bounds.max) * 0.5f;
            shape.radius = length(shape.bounds.max - shape.center);
            shape.tubeRadius = shape.radius;
        }
    }

    bool run(std::string& error)
    {
        if (!scene.mRootNode) {
            error = "scene has no root node";
            return false;
        }
        shape = Shape{};
        shape.exporterVersion = opt.exporterVersion;
        haveMeta = findJsonMeta(scene, kTsMetadataKey, meta) && meta.is_object();
        trailing = opt.lod == LodMode::TrailingNumber || (opt.lod == LodMode::Auto && hasBaseStartLayout());
        if (haveMeta) {
            if (opt.lod == LodMode::Auto && meta.value("lodNames", false)) {
                trailing = true;
            }
            if (meta.contains("collapse") && meta["collapse"].is_array()) {
                for (const auto& c : meta["collapse"]) {
                    if (c.is_string()) {
                        collapseNames.insert(c.get<std::string>());
                    }
                }
            }
            if (meta.contains("meshNodes") && meta["meshNodes"].is_object()) {
                for (const auto& [node, v] : meta["meshNodes"].items()) {
                    if (v.is_array() && v.size() == 2 && v[0].is_string() && v[1].is_number()) {
                        meshNodes[node] = {v[0].get<std::string>(), v[1].get<int>()};
                    }
                }
            }
        }
        for (unsigned i = 0; i < scene.mRootNode->mNumChildren; ++i) {
            if (lowerAscii(scene.mRootNode->mChildren[i]->mName.C_Str()) == "bounds") {
                boundsNode = scene.mRootNode->mChildren[i];
                break;
            }
        }
        collectNodes(scene.mRootNode, -1, Mat4{});
        buildNodes();
        collectMarkers();
        collectObjects();
        if (objects.empty()) {
            warn("scene has no triangle meshes");
        }
        buildObjectsAndDetails();
        importSequences();
        applyMetadata();
        shape.rebuildLinks();
        computeShapeBounds();
        shape.updateSmallestVisible();
        return true;
    }
};

} // namespace

std::string splitTrailingNumber(const std::string& name, int& size)
{
    size_t end = name.size();
    size_t p = end;
    while (p > 0 && name[p - 1] >= '0' && name[p - 1] <= '9') {
        --p;
    }
    if (p == end) {
        return name;
    }
    long long value = std::stoll(name.substr(p, std::min<size_t>(end - p, 9)));
    if (p > 0 && name[p - 1] == '-') {
        --p;
        value = -value;
    }
    size = int(value);
    return name.substr(0, p);
}

bool assimpToShape(const aiScene& scene, const AiToTsOptions& options, Shape& shape, std::vector<std::string>& warnings,
                   std::string& error)
{
    Converter c{scene, options, shape, warnings};
    return c.run(error);
}

} // namespace ts
