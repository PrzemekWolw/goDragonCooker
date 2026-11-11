// DTS v19-29: a header, three typed memory buffers (32/16/8 bit) holding the shape and its meshes
// separated by guard words, then sequences and the material list as a plain stream.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/i18n.h"
#include "core/strings.h"
#include "model/ts/ts_io.h"

namespace ts {
namespace {

struct StreamIn {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    template <class T>
    T read()
    {
        T v{};
        if (pos + sizeof(T) > size) {
            ok = false;
            return v;
        }
        std::memcpy(&v, data + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    bool readBytes(void* dst, size_t n)
    {
        if (pos + n > size) {
            ok = false;
            return false;
        }
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
};

class MemIn {
public:
    MemIn(const int32_t* b32, size_t n32, const int16_t* b16, size_t n16, const int8_t* b8, size_t n8)
        : m32(b32), c32(n32), m16(b16), c16(n16), m8(b8), c8(n8)
    {
    }

    int32_t get32()
    {
        if (p32 >= c32) {
            fail("32-bit buffer overrun");
            return 0;
        }
        return m32[p32++];
    }
    float getF()
    {
        const int32_t v = get32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    bool get32(void* dst, size_t count)
    {
        if (count > c32 - std::min(p32, c32)) {
            fail("32-bit buffer overrun");
            return false;
        }
        if (count) {
            std::memcpy(dst, m32 + p32, count * 4);
        }
        p32 += count;
        return true;
    }
    void skip32(int64_t count)
    {
        if (count < 0 || size_t(count) > c32 - std::min(p32, c32)) {
            fail("32-bit buffer overrun");
            return;
        }
        p32 += size_t(count);
    }
    bool get16(void* dst, size_t count)
    {
        if (count > c16 - std::min(p16, c16)) {
            fail("16-bit buffer overrun");
            return false;
        }
        if (count) {
            std::memcpy(dst, m16 + p16, count * 2);
        }
        p16 += count;
        return true;
    }
    void skip16(int64_t count)
    {
        if (count < 0 || size_t(count) > c16 - std::min(p16, c16)) {
            fail("16-bit buffer overrun");
            return;
        }
        p16 += size_t(count);
    }
    bool get8(void* dst, size_t count)
    {
        if (count > c8 - std::min(p8, c8)) {
            fail("8-bit buffer overrun");
            return false;
        }
        if (count) {
            std::memcpy(dst, m8 + p8, count);
        }
        p8 += count;
        return true;
    }
    void skip8(int64_t count)
    {
        if (count < 0 || size_t(count) > c8 - std::min(p8, c8)) {
            fail("8-bit buffer overrun");
            return;
        }
        p8 += size_t(count);
    }
    const int8_t* peek8() const { return m8 + p8; }
    size_t left8() const { return c8 - std::min(p8, c8); }

    bool checkGuard(const char* where)
    {
        if (failed()) {
            return false;
        }
        int32_t g32 = 0;
        int16_t g16 = 0;
        int8_t g8 = 0;
        if (!get32(&g32, 1) || !get16(&g16, 1) || !get8(&g8, 1)) {
            return false;
        }
        const bool ok = g32 == guard32 && g16 == int16_t(guard16) && g8 == int8_t(guard8);
        ++guard32;
        ++guard16;
        ++guard8;
        if (!ok) {
            fail(format("guard mismatch at %s (expected %d, found %d/%d/%d)", where, guard32 - 1, g32, g16, g8));
        }
        return ok;
    }

    // count read from the file, rejected when larger than anything the buffers could hold
    int32_t getCount(const char* what)
    {
        const int32_t n = get32();
        if (n < 0 || size_t(n) > (c32 + c16 + c8) * 4 + 16) {
            fail(format("bad %s count %d", what, n));
            return 0;
        }
        return n;
    }

    void fail(const std::string& e)
    {
        if (err.empty()) {
            err = e;
        }
    }
    bool failed() const { return !err.empty(); }
    const std::string& error() const { return err; }

private:
    const int32_t* m32;
    size_t c32;
    const int16_t* m16;
    size_t c16;
    const int8_t* m8;
    size_t c8;
    size_t p32 = 0, p16 = 0, p8 = 0;
    int32_t guard32 = 0, guard16 = 0, guard8 = 0;
    std::string err;
};

template <class T>
void getVec32(MemIn& in, std::vector<T>& v, int32_t count)
{
    static_assert(sizeof(T) % 4 == 0, "32-bit records");
    v.resize(size_t(std::max(count, 0)));
    if (!in.get32(v.data(), v.size() * sizeof(T) / 4)) {
        v.clear();
    }
}

void getQuats(MemIn& in, std::vector<Quat16>& v, int32_t count)
{
    v.resize(size_t(std::max(count, 0)));
    if (!in.get16(v.data(), v.size() * 4)) {
        v.clear();
    }
}

bool readIntegerSet(StreamIn& s, IntegerSet& set)
{
    s.read<int32_t>(); // numInts, unused
    const int32_t sz = s.read<int32_t>();
    if (!s.ok || sz < 0 || sz > 64) {
        s.ok = false;
        return false;
    }
    set.words.resize(size_t(sz));
    for (int32_t i = 0; i < sz; ++i) {
        set.words[i] = s.read<uint32_t>();
    }
    return s.ok;
}

bool readSequence(StreamIn& s, int V, Sequence& q)
{
    q.nameIndex = s.read<int32_t>();
    q.flags = V > 21 ? s.read<uint32_t>() : 0;
    q.numKeyframes = s.read<int32_t>();
    q.duration = s.read<float>();
    if (V < 22) {
        if (s.read<uint8_t>()) {
            q.flags |= uint32_t(SequenceFlag::Blend);
        }
        if (s.read<uint8_t>()) {
            q.flags |= uint32_t(SequenceFlag::Cyclic);
        }
        if (s.read<uint8_t>()) {
            q.flags |= uint32_t(SequenceFlag::MakePath);
        }
    }
    q.priority = s.read<int32_t>();
    q.firstGroundFrame = s.read<int32_t>();
    q.numGroundFrames = s.read<int32_t>();
    if (V > 21) {
        q.baseRotation = s.read<int32_t>();
        q.baseTranslation = s.read<int32_t>();
        q.baseScale = s.read<int32_t>();
        q.baseObjectState = s.read<int32_t>();
        q.baseDecalState = s.read<int32_t>();
    } else {
        q.baseRotation = s.read<int32_t>();
        q.baseTranslation = q.baseRotation;
        q.baseObjectState = s.read<int32_t>();
        q.baseDecalState = s.read<int32_t>();
    }
    q.firstTrigger = s.read<int32_t>();
    q.numTriggers = s.read<int32_t>();
    q.toolBegin = s.read<float>();
    readIntegerSet(s, q.rotationMatters);
    if (V < 22) {
        q.translationMatters = q.rotationMatters;
    } else {
        readIntegerSet(s, q.translationMatters);
        readIntegerSet(s, q.scaleMatters);
    }
    IntegerSet dummy;
    readIntegerSet(s, dummy); // decals
    readIntegerSet(s, dummy); // ifl materials
    readIntegerSet(s, q.visMatters);
    readIntegerSet(s, q.frameMatters);
    readIntegerSet(s, q.matFrameMatters);
    return s.ok;
}

std::string stripMaterialPath(const std::string& n)
{
    const size_t p = n.find_last_of("/\\");
    return p == std::string::npos ? n : n.substr(p + 1);
}

bool readMaterialList(StreamIn& s, int V, std::vector<Material>& mats)
{
    mats.clear();
    const uint8_t version = s.read<uint8_t>();
    if (!s.ok) {
        return false;
    }
    if (version != 1) {
        // text form: one name per line, terminated by an empty line
        if (version != 0) {
            std::string line(1, char(version));
            while (s.ok && s.pos < s.size) {
                const char c = char(s.read<uint8_t>());
                if (c == '\n') {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.empty()) {
                        break;
                    }
                    mats.push_back({stripMaterialPath(line)});
                    line.clear();
                } else {
                    line += c;
                }
            }
        }
    } else {
        const uint32_t count = s.read<uint32_t>();
        if (!s.ok || count > 1u << 20) {
            return false;
        }
        mats.resize(count);
        for (Material& m : mats) {
            const uint8_t len = s.read<uint8_t>();
            std::string name(len, '\0');
            s.readBytes(name.data(), len);
            m.name = stripMaterialPath(name);
        }
    }
    for (Material& m : mats) {
        m.flags = s.read<uint32_t>();
    }
    for (Material& m : mats) {
        m.reflectanceMap = s.read<uint32_t>();
    }
    for (Material& m : mats) {
        m.bumpMap = s.read<uint32_t>();
    }
    for (Material& m : mats) {
        m.detailMap = s.read<uint32_t>();
    }
    if (V == 25) {
        for (size_t i = 0; i < mats.size(); ++i) {
            s.read<uint32_t>();
        }
    }
    for (Material& m : mats) {
        m.detailScale = s.read<float>();
    }
    if (V > 20) {
        for (Material& m : mats) {
            m.reflectionAmount = s.read<float>();
        }
    }
    return s.ok;
}

// Copies the first count elements of a parent's array when parentMesh shares data.
template <class T>
void shareFrom(std::vector<T>& dst, const std::vector<T>& src, int32_t count)
{
    dst.assign(src.begin(), src.begin() + std::min<size_t>(src.size(), size_t(std::max(count, 0))));
}

class LegacyReader {
public:
    LegacyReader(MemIn& in, int version, Shape& shape, std::vector<std::string>* warnings)
        : in(in), V(version), shape(shape), warnings(warnings)
    {
    }

    bool assemble();

private:
    void warn(const std::string& w)
    {
        if (warnings) {
            warnings->push_back(w);
        }
    }
    const Mesh* parentOf(int32_t parent) const
    {
        if (parent < 0) {
            return nullptr;
        }
        if (parent < int32_t(meshes.size())) {
            return &meshes[parent];
        }
        return nullptr;
    }
    bool readMesh(MeshType type, Mesh& m, const std::vector<Mesh>& pool);
    bool readSkinExtra(Mesh& m, const Mesh* parent);
    bool readSorted();
    bool readDecal();
    void fixupOldSkins(int numMeshes, const std::vector<Mesh>& skins, const std::vector<int32_t>& detFirstSkin,
                       const std::vector<int32_t>& detailNumSkins);

    MemIn& in;
    int V;
    Shape& shape;
    std::vector<std::string>* warnings;
    std::vector<Mesh> meshes;
};

bool LegacyReader::readMesh(MeshType type, Mesh& m, const std::vector<Mesh>& pool)
{
    m.type = type;
    in.checkGuard("mesh start");
    m.numFrames = in.get32();
    m.numMatFrames = in.get32();
    m.parentMesh = in.get32();
    in.get32(&m.bounds, 6);
    in.get32(&m.center, 3);
    m.radius = float(in.get32());

    const Mesh* parent = nullptr;
    if (m.parentMesh >= 0) {
        if (m.parentMesh >= int32_t(pool.size())) {
            in.fail(format("mesh parent %d out of range", m.parentMesh));
            return false;
        }
        parent = &pool[m.parentMesh];
    }

    const int32_t numVerts = in.getCount("vertex");
    if (parent) {
        shareFrom(m.verts, parent->isSkin() ? parent->initialVerts : parent->verts, numVerts);
    } else {
        getVec32(in, m.verts, numVerts);
    }
    const int32_t numTVerts = in.getCount("uv");
    if (parent) {
        shareFrom(m.tverts, parent->tverts, numTVerts);
    } else {
        getVec32(in, m.tverts, numTVerts);
    }
    if (V > 25) {
        const int32_t numT2 = in.getCount("uv2");
        if (parent) {
            shareFrom(m.tverts2, parent->tverts2, numT2);
        } else {
            getVec32(in, m.tverts2, numT2);
        }
        const int32_t numColors = in.getCount("color");
        if (parent) {
            shareFrom(m.colors, parent->colors, numColors);
        } else {
            getVec32(in, m.colors, numColors);
        }
    }
    if (parent) {
        shareFrom(m.norms, parent->isSkin() ? parent->initialNorms : parent->norms, numVerts);
        if (V > 21) {
            shareFrom(m.encodedNorms, parent->encodedNorms, numVerts);
        }
    } else {
        getVec32(in, m.norms, numVerts);
        if (V > 21) {
            m.encodedNorms.resize(size_t(numVerts));
            in.get8(m.encodedNorms.data(), m.encodedNorms.size());
        }
    }

    if (V > 25) {
        const int32_t numPrims = in.getCount("primitive");
        getVec32(in, m.primitives, numPrims);
        const int32_t numIdx = in.getCount("index");
        getVec32(in, m.indices, numIdx);
    } else {
        const int32_t numPrims = in.getCount("primitive");
        std::vector<int16_t> prim16(size_t(numPrims) * 2);
        in.get16(prim16.data(), prim16.size());
        std::vector<int32_t> mat32(static_cast<size_t>(numPrims));
        in.get32(mat32.data(), mat32.size());
        m.primitives.resize(size_t(numPrims));
        for (int32_t i = 0; i < numPrims && !in.failed(); ++i) {
            m.primitives[i].start = uint16_t(prim16[i * 2]);
            m.primitives[i].numElements = uint16_t(prim16[i * 2 + 1]);
            m.primitives[i].matIndex = uint32_t(mat32[i]);
        }
        const int32_t numIdx = in.getCount("index");
        std::vector<uint16_t> idx16(static_cast<size_t>(numIdx));
        in.get16(idx16.data(), idx16.size());
        m.indices.assign(idx16.begin(), idx16.end());
    }

    const int32_t merge = in.getCount("merge index");
    in.skip16(merge);
    m.vertsPerFrame = in.get32();
    m.flags = uint32_t(in.get32()) & ~uint32_t(UseEncodedNormals);

    if (V >= 28) {
        const int32_t numTangents = in.getCount("tangent");
        if (parent) {
            shareFrom(m.tangents, parent->tangents, numTangents);
        } else {
            getVec32(in, m.tangents, numTangents);
        }
    }
    in.checkGuard("mesh end");

    if (type == MeshType::Skin) {
        return readSkinExtra(m, parent);
    }
    return !in.failed();
}

bool LegacyReader::readSkinExtra(Mesh& m, const Mesh* parent)
{
    const int32_t numVerts = in.getCount("skin vertex");
    if (parent) {
        shareFrom(m.initialVerts, parent->initialVerts, numVerts);
        shareFrom(m.initialNorms, parent->initialNorms, numVerts);
        if (V > 21) {
            shareFrom(m.encodedNorms, parent->encodedNorms, numVerts);
        }
    } else {
        getVec32(in, m.initialVerts, numVerts);
        getVec32(in, m.initialNorms, numVerts);
        if (V > 21) {
            in.skip8(numVerts);
        }
    }
    const int32_t numXforms = in.getCount("skin transform");
    if (parent) {
        shareFrom(m.initialTransforms, parent->initialTransforms, numXforms);
    } else {
        getVec32(in, m.initialTransforms, numXforms);
    }
    const int32_t numWeights = in.getCount("skin weight");
    if (parent) {
        shareFrom(m.vertexIndex, parent->vertexIndex, numWeights);
        shareFrom(m.boneIndex, parent->boneIndex, numWeights);
        shareFrom(m.weight, parent->weight, numWeights);
    } else {
        getVec32(in, m.vertexIndex, numWeights);
        getVec32(in, m.boneIndex, numWeights);
        getVec32(in, m.weight, numWeights);
    }
    const int32_t numNodes = in.getCount("skin node");
    if (parent) {
        shareFrom(m.nodeIndex, parent->nodeIndex, numNodes);
    } else {
        getVec32(in, m.nodeIndex, numNodes);
    }
    in.checkGuard("skin end");
    return !in.failed();
}

bool LegacyReader::readSorted()
{
    const int32_t numClusters = in.getCount("cluster");
    in.skip32(int64_t(numClusters) * 8);
    for (int i = 0; i < 4; ++i) {
        in.skip32(in.getCount("cluster list"));
    }
    in.get32(); // alwaysWriteDepth
    in.checkGuard("sorted mesh end");
    return !in.failed();
}

bool LegacyReader::readDecal()
{
    if (V < 20) {
        in.checkGuard("decal start");
        in.skip32(15);
    }
    const int32_t numPrims = in.getCount("decal primitive");
    in.skip16(int64_t(numPrims) * 2);
    in.skip32(numPrims);
    in.skip16(in.getCount("decal index"));
    if (V < 20) {
        in.skip32(3);
        in.checkGuard("decal mid");
    }
    const int32_t numStarts = in.getCount("decal start");
    in.skip32(numStarts);
    if (V >= 19) {
        in.skip32(int64_t(numStarts) * 8);
    }
    in.get32(); // materialIndex
    in.checkGuard("decal end");
    return !in.failed();
}

void LegacyReader::fixupOldSkins(int numMeshes, const std::vector<Mesh>& skinsIn, const std::vector<int32_t>& detFirstSkin,
                                 const std::vector<int32_t>& detailNumSkins)
{
    const int numSkins = int(skinsIn.size());
    std::vector<int> skins(numSkins);
    int emptySkins = 0;
    for (int i = 0; i < numSkins; ++i) {
        skins[i] = skinsIn[i].isNull() ? -1 : i;
        emptySkins += skins[i] < 0;
    }
    const int numObjects = int(shape.objects.size());
    const int numDetails = int(detFirstSkin.size());
    std::vector<int> skinsCopy;
    int numSkinObjects = 0, skinsUsed = 0;
    while (skinsUsed < numSkins - emptySkins) {
        Object object;
        object.nameIndex = 0;
        object.startMeshIndex = numMeshes + int(skinsCopy.size());
        const int usedBefore = skinsUsed;
        for (int dl = 0; dl < numDetails; ++dl) {
            const int first = detFirstSkin[dl];
            const int end = first + detailNumSkins[dl];
            int i = first < 0 ? numSkins : first;
            for (; i < numSkins && i < end; ++i) {
                if (skins[i] >= 0) {
                    skinsCopy.push_back(skins[i]);
                    skins[i] = -1;
                    ++object.numMeshes;
                    ++skinsUsed;
                    break;
                }
            }
            if (i == numSkins || i == end) {
                skinsCopy.push_back(-1);
                ++object.numMeshes;
            }
        }
        while (!skinsCopy.empty() && skinsCopy.back() < 0) {
            skinsCopy.pop_back();
            --object.numMeshes;
        }
        if (object.numMeshes > 0) {
            shape.objects.push_back(object);
            ++numSkinObjects;
        }
        if (skinsUsed == usedBefore) {
            warn(tr("gdc.model.note.oldSkin"));
            break;
        }
    }
    for (int idx : skinsCopy) {
        shape.meshes.push_back(idx >= 0 ? skinsIn[idx] : Mesh{});
    }
    if (shape.subShapeFirstObject.size() == 1) {
        shape.subShapeNumObjects[0] += numSkinObjects;
    }
    shape.objectStates.insert(shape.objectStates.begin() + std::min<size_t>(numObjects, shape.objectStates.size()),
                              size_t(numSkinObjects), ObjectState{});
    for (Sequence& seq : shape.sequences) {
        seq.baseObjectState += numSkinObjects;
    }
}

bool LegacyReader::assemble()
{
    const int32_t numNodes = in.getCount("node");
    const int32_t numObjects = in.getCount("object");
    const int32_t numDecals = in.getCount("decal");
    const int32_t numSubShapes = in.getCount("subshape");
    const int32_t numIflMaterials = in.getCount("ifl");
    int32_t numNodeRots, numNodeTrans, numUniform = 0, numAligned = 0, numArbitrary = 0;
    if (V < 22) {
        numNodeRots = numNodeTrans = in.get32() - numNodes;
    } else {
        numNodeRots = in.getCount("rotation");
        numNodeTrans = in.getCount("translation");
        numUniform = in.getCount("uniform scale");
        numAligned = in.getCount("aligned scale");
        numArbitrary = in.getCount("arbitrary scale");
    }
    const int32_t numGroundFrames = V > 23 ? in.getCount("ground frame") : 0;
    const int32_t numObjectStates = in.getCount("object state");
    const int32_t numDecalStates = in.getCount("decal state");
    const int32_t numTriggers = in.getCount("trigger");
    const int32_t numDetails = in.getCount("detail");
    const int32_t numMeshes = in.getCount("mesh");
    const int32_t numSkins = V < 23 ? in.getCount("skin") : 0;
    const int32_t numNames = in.getCount("name");
    shape.smallestVisibleSize = float(in.get32());
    shape.smallestVisibleDL = in.get32();
    if (numNodeRots < 0 || numNodeTrans < 0) {
        in.fail("bad node transform count");
    }
    in.checkGuard("counts");

    shape.radius = in.getF();
    shape.tubeRadius = in.getF();
    in.get32(&shape.center, 3);
    in.get32(&shape.bounds, 6);
    in.checkGuard("bounds");

    getVec32(in, shape.nodes, numNodes);
    in.checkGuard("nodes");
    getVec32(in, shape.objects, numObjects);
    in.checkGuard("objects");
    in.skip32(int64_t(numDecals) * 5);
    in.checkGuard("decals");
    in.skip32(int64_t(numIflMaterials) * 5);
    in.checkGuard("ifl materials");

    getVec32(in, shape.subShapeFirstNode, numSubShapes);
    getVec32(in, shape.subShapeFirstObject, numSubShapes);
    in.skip32(numSubShapes);
    in.checkGuard("subshape firsts");
    getVec32(in, shape.subShapeNumNodes, numSubShapes);
    getVec32(in, shape.subShapeNumObjects, numSubShapes);
    in.skip32(numSubShapes);
    in.checkGuard("subshape counts");

    getQuats(in, shape.defaultRotations, numNodes);
    getVec32(in, shape.defaultTranslations, numNodes);
    getVec32(in, shape.nodeTranslations, numNodeTrans);
    getQuats(in, shape.nodeRotations, numNodeRots);
    in.checkGuard("node transforms");

    if (V > 21) {
        getVec32(in, shape.nodeUniformScales, numUniform);
        getVec32(in, shape.nodeAlignedScales, numAligned);
        getVec32(in, shape.nodeArbitraryScaleFactors, numArbitrary);
        getQuats(in, shape.nodeArbitraryScaleRots, numArbitrary);
        in.checkGuard("node scales");
    }

    if (V < 22) {
        // Old shapes keep ground frames inside the node arrays, indexed past the defaults.
        for (Sequence& seq : shape.sequences) {
            const size_t oldSz = shape.groundTranslations.size();
            for (int32_t j = 0; j < seq.numGroundFrames; ++j) {
                const int64_t src = int64_t(seq.firstGroundFrame) + j - numNodes;
                const bool valid = src >= 0 && src < int64_t(shape.nodeTranslations.size()) && src < int64_t(shape.nodeRotations.size());
                shape.groundTranslations.push_back(valid ? shape.nodeTranslations[size_t(src)] : Vec3{});
                shape.groundRotations.push_back(valid ? shape.nodeRotations[size_t(src)] : Quat16{});
            }
            seq.firstGroundFrame = int32_t(oldSz);
            seq.baseTranslation -= numNodes;
            seq.baseRotation -= numNodes;
            seq.baseScale = 0;
        }
    }
    if (V > 23) {
        getVec32(in, shape.groundTranslations, numGroundFrames);
        getQuats(in, shape.groundRotations, numGroundFrames);
        in.checkGuard("ground frames");
    }

    getVec32(in, shape.objectStates, numObjectStates);
    in.checkGuard("object states");
    in.skip32(numDecalStates);
    in.checkGuard("decal states");
    getVec32(in, shape.triggers, numTriggers);
    in.checkGuard("triggers");

    if (V >= 26) {
        getVec32(in, shape.details, numDetails);
    } else {
        shape.details.resize(size_t(numDetails));
        for (Detail& det : shape.details) {
            in.get32(&det, 7);
            det.bbDimension = det.bbDetailLevel = 0;
            det.bbEquatorSteps = det.bbPolarSteps = det.bbIncludePoles = 0;
            det.bbPolarAngle = 0;
            if (det.subShapeNum >= 0) {
                continue;
            }
            const uint32_t opts = uint32_t(det.objectDetailNum);
            det.bbEquatorSteps = opts & 0x7F;
            det.bbPolarSteps = (opts >> 7) & 0x3F;
            det.bbPolarAngle = 0.5f * 3.14159265f * (1.0f / 64.0f) * float((opts >> 13) & 0x3F);
            det.bbDetailLevel = int32_t((opts >> 19) & 0x0F);
            det.bbDimension = int32_t((opts >> 23) & 0xFF);
            det.bbIncludePoles = (opts & 0x80000000u) != 0;
        }
    }
    for (Detail& det : shape.details) {
        if (det.averageError == 0 || det.averageError > 10000 || det.maxError == 0 || det.maxError > 10000) {
            det.averageError = det.maxError = -1.0f;
        }
    }
    in.checkGuard("details");
    if (in.failed()) {
        return false;
    }

    meshes.reserve(size_t(numMeshes));
    int decals = 0, sorted = 0;
    for (int32_t i = 0; i < numMeshes && !in.failed(); ++i) {
        const uint32_t type = uint32_t(in.get32());
        Mesh m;
        if (type == uint32_t(MeshType::Null)) {
            m.type = MeshType::Null;
        } else if (type == uint32_t(MeshType::Decal)) {
            readDecal();
            ++decals;
        } else if (type == uint32_t(MeshType::Standard) || type == uint32_t(MeshType::Skin) || type == uint32_t(MeshType::Sorted)) {
            readMesh(type == uint32_t(MeshType::Sorted) ? MeshType::Standard : MeshType(type), m, meshes);
            if (type == uint32_t(MeshType::Sorted)) {
                readSorted();
                ++sorted;
            }
        } else {
            in.fail(format("unknown mesh type %u", type));
        }
        meshes.push_back(std::move(m));
    }
    if (decals) {
        warn(format(tr("gdc.model.note.decals"), decals));
    }
    if (sorted) {
        warn(format(tr("gdc.model.note.sorted"), sorted));
    }
    in.checkGuard("meshes");

    shape.names.resize(size_t(numNames));
    for (int32_t i = 0; i < numNames && !in.failed(); ++i) {
        const int8_t* p = in.peek8();
        const size_t left = in.left8();
        size_t len = 0;
        while (len < left && p[len]) {
            ++len;
        }
        if (len == left) {
            in.fail("unterminated name");
            break;
        }
        shape.names[i].assign(reinterpret_cast<const char*>(p), len);
        in.skip8(int64_t(len) + 1);
    }
    in.checkGuard("names");

    shape.meshes = std::move(meshes);
    if (V < 23 && !in.failed()) {
        std::vector<int32_t> detFirstSkin(static_cast<size_t>(numDetails));
        std::vector<int32_t> detailNumSkins(static_cast<size_t>(numDetails));
        in.get32(detFirstSkin.data(), detFirstSkin.size());
        in.get32(detailNumSkins.data(), detailNumSkins.size());
        in.checkGuard("skin details");
        std::vector<Mesh> skins;
        skins.reserve(size_t(numSkins));
        for (int32_t i = 0; i < numSkins && !in.failed(); ++i) {
            Mesh m;
            readMesh(MeshType::Skin, m, skins);
            skins.push_back(std::move(m));
        }
        in.checkGuard("skins");
        if (!in.failed() && numSkins) {
            fixupOldSkins(numMeshes, skins, detFirstSkin, detailNumSkins);
        }
    }
    return !in.failed();
}

} // namespace

bool readDts(const std::vector<uint8_t>& data, Shape& shape, std::string& error, std::vector<std::string>* warnings)
{
    shape = Shape{};
    StreamIn s{data.data(), data.size()};
    const uint32_t word = s.read<uint32_t>();
    const int V = int(word & 0xFF);
    shape.readVersion = V;
    shape.exporterVersion = int(word >> 16);
    if (V < kMinReadVersion || V > 29) {
        error = format("unsupported version %d", V);
        return false;
    }

    if (V >= 29) {
        const int32_t numObjects = s.read<int32_t>();
        if (numObjects < 0) {
            error = "bad object name header";
            return false;
        }
        for (int32_t i = 0; i < numObjects && s.ok; ++i) {
            const uint32_t len = s.read<uint32_t>();
            if (len > 32786 || s.pos + len > s.size) {
                s.ok = false;
                break;
            }
            s.pos += len;
        }
    }
    const uint32_t sizeMemBuffer = s.read<uint32_t>();
    const uint32_t start16 = s.read<uint32_t>();
    const uint32_t start8 = s.read<uint32_t>();
    if (!s.ok || start16 > start8 || start8 > sizeMemBuffer || uint64_t(sizeMemBuffer) * 4 > s.size - s.pos) {
        error = "bad shape file: buffer header out of range";
        return false;
    }
    std::vector<int32_t> buffer(sizeMemBuffer);
    s.readBytes(buffer.data(), size_t(sizeMemBuffer) * 4);

    const int32_t numSequences = s.read<int32_t>();
    if (!s.ok || numSequences < 0 || numSequences > 4096) {
        error = format("bad sequence count %d", numSequences);
        return false;
    }
    shape.sequences.resize(size_t(numSequences));
    for (Sequence& seq : shape.sequences) {
        if (!readSequence(s, V, seq)) {
            error = "failed to read sequence data";
            return false;
        }
    }
    if (!readMaterialList(s, V, shape.materials)) {
        error = "failed to read material list";
        return false;
    }
    if (V < 16) {
        for (Material& m : shape.materials) {
            if (m.flags & Translucent) {
                m.flags |= NeverEnvMap;
            }
        }
    }

    MemIn in(buffer.data(), start16, reinterpret_cast<const int16_t*>(buffer.data() + start16), size_t(start8 - start16) * 2,
             reinterpret_cast<const int8_t*>(buffer.data() + start8), size_t(sizeMemBuffer - start8) * 4);
    LegacyReader reader(in, V, shape, warnings);
    if (!reader.assemble()) {
        error = in.error().empty() ? "bad shape file" : in.error();
        return false;
    }
    for (Mesh& m : shape.meshes) {
        m.parentMesh = -1;
    }
    shape.rebuildLinks();
    shape.updateSmallestVisible();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Writer

namespace {

class MemOut {
public:
    std::vector<int32_t> b32;
    std::vector<int16_t> b16;
    std::vector<int8_t> b8;

    void set32(int32_t v) { b32.push_back(v); }
    void setF(float f)
    {
        int32_t v;
        std::memcpy(&v, &f, 4);
        b32.push_back(v);
    }
    void put32(const void* src, size_t count)
    {
        const size_t at = b32.size();
        b32.resize(at + count);
        if (count && src) {
            std::memcpy(b32.data() + at, src, count * 4);
        }
    }
    void put16(const void* src, size_t count)
    {
        const size_t at = b16.size();
        b16.resize(at + count);
        if (count && src) {
            std::memcpy(b16.data() + at, src, count * 2);
        }
    }
    void put8(const void* src, size_t count)
    {
        const size_t at = b8.size();
        b8.resize(at + count);
        if (count && src) {
            std::memcpy(b8.data() + at, src, count);
        }
    }
    void setGuard()
    {
        b32.push_back(guard);
        b16.push_back(int16_t(guard));
        b8.push_back(int8_t(guard));
        ++guard;
    }

private:
    int32_t guard = 0;
};

template <class T>
void putVec(MemOut& out, const std::vector<T>& v)
{
    out.put32(v.data(), v.size() * sizeof(T) / 4);
}

struct StreamOut {
    std::vector<uint8_t>& data;
    template <class T>
    void write(const T& v)
    {
        const size_t at = data.size();
        data.resize(at + sizeof(T));
        std::memcpy(data.data() + at, &v, sizeof(T));
    }
    void writeBytes(const void* p, size_t n)
    {
        const size_t at = data.size();
        data.resize(at + n);
        if (n) {
            std::memcpy(data.data() + at, p, n);
        }
    }
};

void writeIntegerSet(StreamOut& s, const IntegerSet& set)
{
    s.write(int32_t(0));
    const int used = std::min(set.usedWords(), 64);
    s.write(int32_t(used));
    for (int i = 0; i < used; ++i) {
        s.write(set.words[i]);
    }
}

void writeMesh(MemOut& out, const Mesh& m, int V, size_t& tooManyIndices)
{
    out.setGuard();
    out.set32(m.numFrames);
    out.set32(m.numMatFrames);
    out.set32(-1);
    out.put32(&m.bounds, 6);
    out.put32(&m.center, 3);
    out.set32(int32_t(m.radius));

    const std::vector<Vec3>& verts = m.isSkin() && m.verts.empty() ? m.initialVerts : m.verts;
    std::vector<Vec3> norms = m.isSkin() && m.norms.empty() ? m.initialNorms : m.norms;
    norms.resize(verts.size(), Vec3{0, 0, 1});

    out.set32(int32_t(verts.size()));
    putVec(out, verts);
    out.set32(int32_t(m.tverts.size()));
    putVec(out, m.tverts);
    if (V > 25) {
        out.set32(int32_t(m.tverts2.size()));
        putVec(out, m.tverts2);
        out.set32(int32_t(m.colors.size()));
        putVec(out, m.colors);
    }
    putVec(out, norms);
    // The engine ignores the 8-bit normal codes (smUseEncodedNormals is off); keep any we read.
    std::vector<uint8_t> codes = m.encodedNorms;
    codes.resize(norms.size(), 0);
    out.put8(codes.data(), codes.size());

    if (V > 25) {
        out.set32(int32_t(m.primitives.size()));
        putVec(out, m.primitives);
        out.set32(int32_t(m.indices.size()));
        out.put32(m.indices.data(), m.indices.size());
    } else {
        out.set32(int32_t(m.primitives.size()));
        for (const Primitive& p : m.primitives) {
            const int16_t se[2] = {int16_t(p.start), int16_t(p.numElements)};
            out.put16(se, 2);
            out.set32(int32_t(p.matIndex));
            if (p.start > 0xFFFF || p.numElements > 0xFFFF) {
                ++tooManyIndices;
            }
        }
        out.set32(int32_t(m.indices.size()));
        for (uint32_t i : m.indices) {
            const int16_t v = int16_t(uint16_t(i));
            out.put16(&v, 1);
            if (i > 0xFFFF) {
                ++tooManyIndices;
            }
        }
    }
    out.set32(0); // merge indices
    out.set32(m.vertsPerFrame);
    out.set32(int32_t(m.flags & ~uint32_t(UseEncodedNormals)));
    if (V >= 28) {
        out.set32(int32_t(m.tangents.size()));
        putVec(out, m.tangents);
    }
    out.setGuard();

    if (m.isSkin()) {
        const std::vector<Vec3>& iv = m.initialVerts.empty() ? m.verts : m.initialVerts;
        std::vector<Vec3> in = m.initialNorms.empty() ? m.norms : m.initialNorms;
        in.resize(iv.size(), Vec3{0, 0, 1});
        out.set32(int32_t(iv.size()));
        putVec(out, iv);
        putVec(out, in);
        std::vector<uint8_t> skinCodes(in.size(), 0);
        out.put8(skinCodes.data(), skinCodes.size());
        out.set32(int32_t(m.initialTransforms.size()));
        putVec(out, m.initialTransforms);
        out.set32(int32_t(m.vertexIndex.size()));
        putVec(out, m.vertexIndex);
        putVec(out, m.boneIndex);
        putVec(out, m.weight);
        out.set32(int32_t(m.nodeIndex.size()));
        putVec(out, m.nodeIndex);
        out.setGuard();
    }
}

} // namespace

std::vector<std::string> dtsVersionLosses(int version)
{
    std::vector<std::string> out;
    if (version < 29) {
        out.push_back(tr("gdc.model.dtsLoss.names"));
    }
    if (version < 28) {
        out.push_back(tr("gdc.model.dtsLoss.tangents"));
    }
    if (version < 26) {
        out.push_back(tr("gdc.model.dtsLoss.uvColor"));
        out.push_back(tr("gdc.model.dtsLoss.bigMesh"));
        out.push_back(tr("gdc.model.dtsLoss.billboard"));
    }
    if (version == 25) {
        out.push_back(tr("gdc.model.dtsLoss.lightmap"));
    }
    return out;
}

bool writeDts(const Shape& shapeIn, int V, std::vector<uint8_t>& data, std::string& error, std::vector<std::string>* warnings)
{
    if (V < kMinDtsWriteVersion || V > kMaxDtsWriteVersion) {
        error = format("DTS write version %d not supported (24-29)", V);
        return false;
    }
    Shape shape = shapeIn;
    for (Mesh& m : shape.meshes) {
        if (m.type == MeshType::Decal || m.type == MeshType::Sorted) {
            m.type = m.type == MeshType::Decal ? MeshType::Null : MeshType::Standard;
        }
    }

    data.clear();
    StreamOut s{data};
    s.write(uint32_t(V) | (uint32_t(shape.exporterVersion) << 16));
    if (V >= 29) {
        s.write(int32_t(shape.objects.size()));
        for (const Object& o : shape.objects) {
            if (o.nameIndex < 0 || o.nameIndex > int(shape.names.size())) {
                continue;
            }
            const std::string& n = shape.name(o.nameIndex);
            s.write(uint32_t(n.size()));
            s.writeBytes(n.data(), n.size());
        }
    }

    MemOut out;
    out.set32(int32_t(shape.nodes.size()));
    out.set32(int32_t(shape.objects.size()));
    out.set32(0);
    out.set32(int32_t(shape.subShapeFirstNode.size()));
    out.set32(0);
    out.set32(int32_t(shape.nodeRotations.size()));
    out.set32(int32_t(shape.nodeTranslations.size()));
    out.set32(int32_t(shape.nodeUniformScales.size()));
    out.set32(int32_t(shape.nodeAlignedScales.size()));
    out.set32(int32_t(shape.nodeArbitraryScaleFactors.size()));
    out.set32(int32_t(shape.groundTranslations.size()));
    out.set32(int32_t(shape.objectStates.size()));
    out.set32(0);
    out.set32(int32_t(shape.triggers.size()));
    out.set32(int32_t(shape.details.size()));
    out.set32(int32_t(shape.meshes.size()));
    out.set32(int32_t(shape.names.size()));
    out.set32(int32_t(shape.smallestVisibleSize));
    out.set32(shape.smallestVisibleDL);
    out.setGuard();

    out.setF(shape.radius);
    out.setF(shape.tubeRadius);
    out.put32(&shape.center, 3);
    out.put32(&shape.bounds, 6);
    out.setGuard();

    putVec(out, shape.nodes);
    out.setGuard();
    putVec(out, shape.objects);
    out.setGuard();
    out.setGuard(); // decals
    out.setGuard(); // ifl materials
    const size_t numSub = shape.subShapeFirstNode.size();
    putVec(out, shape.subShapeFirstNode);
    putVec(out, shape.subShapeFirstObject);
    out.put32(nullptr, numSub);
    out.setGuard();
    putVec(out, shape.subShapeNumNodes);
    putVec(out, shape.subShapeNumObjects);
    out.put32(nullptr, numSub);
    out.setGuard();

    out.put16(shape.defaultRotations.data(), shape.defaultRotations.size() * 4);
    putVec(out, shape.defaultTranslations);
    out.put16(shape.nodeRotations.data(), shape.nodeRotations.size() * 4);
    putVec(out, shape.nodeTranslations);
    out.setGuard();

    putVec(out, shape.nodeUniformScales);
    putVec(out, shape.nodeAlignedScales);
    putVec(out, shape.nodeArbitraryScaleFactors);
    out.put16(shape.nodeArbitraryScaleRots.data(), shape.nodeArbitraryScaleRots.size() * 4);
    out.setGuard();

    putVec(out, shape.groundTranslations);
    out.put16(shape.groundRotations.data(), shape.groundRotations.size() * 4);
    out.setGuard();

    putVec(out, shape.objectStates);
    out.setGuard();
    out.setGuard(); // decal states
    putVec(out, shape.triggers);
    out.setGuard();

    if (V > 25) {
        putVec(out, shape.details);
    } else {
        for (const Detail& det : shape.details) {
            Detail d = det;
            if (d.subShapeNum < 0) {
                auto fit = [&](int32_t v, int32_t maxV, const char* what) {
                    if (v < 0 || v > maxV) {
                        if (warnings) {
                            warnings->push_back(format(tr("gdc.model.note.billboard"), what, v, maxV));
                        }
                        return uint32_t(std::clamp(v, 0, maxV));
                    }
                    return uint32_t(v);
                };
                const int32_t angleSteps = int32_t(std::lround(d.bbPolarAngle / (0.5f * 3.14159265f / 64.0f)));
                d.objectDetailNum = int32_t(fit(d.bbEquatorSteps, 0x7F, tr("gdc.model.note.bbEquator")) |
                                            (fit(d.bbPolarSteps, 0x3F, tr("gdc.model.note.bbPolar")) << 7) |
                                            (fit(angleSteps, 0x3F, tr("gdc.model.note.bbAngle")) << 13) |
                                            (fit(d.bbDetailLevel, 0x0F, tr("gdc.model.note.bbDetail")) << 19) |
                                            (fit(d.bbDimension, 0xFF, tr("gdc.model.note.bbSize")) << 23) | (d.bbIncludePoles ? 0x80000000u : 0u));
            }
            out.put32(&d, 7);
        }
    }
    out.setGuard();

    std::vector<bool> isMesh(shape.meshes.size(), false);
    for (const Object& o : shape.objects) {
        for (int j = 0; j < o.numMeshes; ++j) {
            const int idx = o.startMeshIndex + j;
            if (idx >= 0 && idx < int(isMesh.size())) {
                isMesh[idx] = true;
            }
        }
    }
    size_t tooMany = 0;
    size_t droppedExtras = 0;
    for (size_t i = 0; i < shape.meshes.size(); ++i) {
        const Mesh& m = shape.meshes[i];
        if (!isMesh[i] || m.isNull()) {
            out.set32(int32_t(MeshType::Null));
            continue;
        }
        out.set32(int32_t(m.type));
        if (V <= 25 && (!m.tverts2.empty() || !m.colors.empty())) {
            ++droppedExtras;
        }
        writeMesh(out, m, V, tooMany);
    }
    out.setGuard();
    for (const std::string& n : shape.names) {
        out.put8(n.c_str(), n.size() + 1);
    }
    out.setGuard();

    if (tooMany) {
        error = format(tr("gdc.model.err.dtsIndex"), V, tooMany);
        return false;
    }
    if (warnings && droppedExtras) {
        warnings->push_back(format(tr("gdc.model.note.droppedExtras"), V, droppedExtras));
    }

    size_t size32 = out.b32.size();
    size_t size16 = out.b16.size();
    size_t size8 = out.b8.size();
    out.b16.resize((size16 + 1) & ~size_t(1), 0);
    out.b8.resize((size8 + 3) & ~size_t(3), 0);
    size16 = out.b16.size() / 2;
    size8 = out.b8.size() / 4;
    s.write(uint32_t(size32 + size16 + size8));
    s.write(uint32_t(size32));
    s.write(uint32_t(size32 + size16));
    s.writeBytes(out.b32.data(), out.b32.size() * 4);
    s.writeBytes(out.b16.data(), out.b16.size() * 2);
    s.writeBytes(out.b8.data(), out.b8.size());

    s.write(int32_t(shape.sequences.size()));
    for (const Sequence& q : shape.sequences) {
        s.write(q.nameIndex);
        s.write(q.flags);
        s.write(q.numKeyframes);
        s.write(q.duration);
        s.write(q.priority);
        s.write(q.firstGroundFrame);
        s.write(q.numGroundFrames);
        s.write(q.baseRotation);
        s.write(q.baseTranslation);
        s.write(q.baseScale);
        s.write(q.baseObjectState);
        s.write(q.baseDecalState);
        s.write(q.firstTrigger);
        s.write(q.numTriggers);
        s.write(q.toolBegin);
        writeIntegerSet(s, q.rotationMatters);
        writeIntegerSet(s, q.translationMatters);
        writeIntegerSet(s, q.scaleMatters);
        writeIntegerSet(s, IntegerSet{});
        writeIntegerSet(s, IntegerSet{});
        writeIntegerSet(s, q.visMatters);
        writeIntegerSet(s, q.frameMatters);
        writeIntegerSet(s, q.matFrameMatters);
    }

    s.write(uint8_t(1));
    s.write(uint32_t(shape.materials.size()));
    for (const Material& m : shape.materials) {
        const size_t len = std::min<size_t>(m.name.size(), 255);
        s.write(uint8_t(len));
        s.writeBytes(m.name.data(), len);
    }
    for (const Material& m : shape.materials) {
        s.write(m.flags);
    }
    for (const Material& m : shape.materials) {
        s.write(m.reflectanceMap);
    }
    for (const Material& m : shape.materials) {
        s.write(m.bumpMap);
    }
    for (const Material& m : shape.materials) {
        s.write(m.detailMap);
    }
    if (V == 25) {
        for (size_t i = 0; i < shape.materials.size(); ++i) {
            s.write(uint32_t(0xFFFFFFFF));
        }
    }
    for (const Material& m : shape.materials) {
        s.write(m.detailScale);
    }
    for (const Material& m : shape.materials) {
        s.write(m.reflectionAmount);
    }
    return true;
}

} // namespace ts
