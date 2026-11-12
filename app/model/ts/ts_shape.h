#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/ts/ts_math.h"

// In-memory Torque3D / BeamNG TSShape, laid out like the engine so DTS v19-29 and CDAE v31
// can be read and written field for field. Struct sizes match the on-disk records.
namespace ts {

constexpr int kExporterCollada = 124;
constexpr int kExporterAssimp = 233;
constexpr int kMaxSetBits = 64 * 32;

struct Box {
    Vec3 min{0, 0, 0}, max{0, 0, 0};
    // Grows the box to hold p; the first point (first == true) resets it.
    void extend(const Vec3& p, bool& first);
};

struct Node {
    int32_t nameIndex = -1;
    int32_t parentIndex = -1;
    int32_t firstObject = -1;
    int32_t firstChild = -1;
    int32_t nextSibling = -1;
};

struct Object {
    int32_t nameIndex = -1;
    int32_t numMeshes = 0;
    int32_t startMeshIndex = 0;
    int32_t nodeIndex = -1;
    int32_t nextSibling = -1;
    int32_t firstDecal = -1;
};

struct Detail {
    int32_t nameIndex = -1;
    int32_t subShapeNum = 0;
    int32_t objectDetailNum = 0;
    float size = 0;
    float averageError = -1;
    float maxError = -1;
    int32_t polyCount = 0;
    int32_t bbDimension = 0;
    int32_t bbDetailLevel = 0;
    uint32_t bbEquatorSteps = 0;
    uint32_t bbPolarSteps = 0;
    float bbPolarAngle = 0;
    uint32_t bbIncludePoles = 0;
};

struct ObjectState {
    float vis = 1;
    int32_t frameIndex = 0;
    int32_t matFrameIndex = 0;
};

struct Trigger {
    uint32_t state = 0;
    float pos = 0;
};

static_assert(sizeof(Node) == 20 && sizeof(Object) == 24 && sizeof(Detail) == 52, "engine record sizes");
static_assert(sizeof(ObjectState) == 12 && sizeof(Trigger) == 8 && sizeof(Quat16) == 8, "engine record sizes");

struct IntegerSet {
    std::vector<uint32_t> words;

    bool test(int i) const;
    void set(int i);
    // number of set bits below i (position of i within the sequence's animated arrays)
    int rank(int i) const;
    int count() const;
    bool any() const;
    // index of the highest used dword + 1, as the engine serializes it
    int usedWords() const;
};

enum class SequenceFlag : uint32_t {
    UniformScale = 1u << 0,
    AlignedScale = 1u << 1,
    ArbitraryScale = 1u << 2,
    Blend = 1u << 3,
    Cyclic = 1u << 4,
    MakePath = 1u << 5,
};

struct Sequence {
    int32_t nameIndex = -1;
    uint32_t flags = 0;
    int32_t numKeyframes = 0;
    float duration = 0;
    int32_t priority = 0;
    int32_t firstGroundFrame = 0;
    int32_t numGroundFrames = 0;
    int32_t baseRotation = 0;
    int32_t baseTranslation = 0;
    int32_t baseScale = 0;
    int32_t baseObjectState = 0;
    int32_t baseDecalState = 0;
    int32_t firstTrigger = 0;
    int32_t numTriggers = 0;
    float toolBegin = 0;
    IntegerSet rotationMatters, translationMatters, scaleMatters, visMatters, frameMatters, matFrameMatters;

    bool has(SequenceFlag f) const { return (flags & uint32_t(f)) != 0; }
};

struct Primitive {
    enum : uint32_t {
        Triangles = 0x00000000,
        Strip = 0x40000000,
        Fan = 0x80000000,
        TypeMask = 0xC0000000,
        Indexed = 0x20000000,
        NoMaterial = 0x10000000,
        MaterialMask = 0x0FFFFFFF,
    };
    int32_t start = 0;
    int32_t numElements = 0;
    uint32_t matIndex = 0;

    uint32_t type() const { return matIndex & TypeMask; }
    bool hasMaterial() const { return (matIndex & NoMaterial) == 0; }
    uint32_t material() const { return matIndex & MaterialMask; }
};
static_assert(sizeof(Primitive) == 12, "engine record sizes");

struct Color8 {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

enum class MeshType : uint32_t { Standard = 0, Skin = 1, Decal = 2, Sorted = 3, Null = 4 };

enum MeshFlag : uint32_t {
    Billboard = 1u << 31,
    HasDetailTexture = 1u << 30,
    BillboardZAxis = 1u << 29,
    UseEncodedNormals = 1u << 28,
};

struct Mat4Raw {
    float m[16];
};
inline Mat4 toMat4(const Mat4Raw& r)
{
    Mat4 m;
    for (int i = 0; i < 16; ++i) {
        m.m[i] = r.m[i];
    }
    return m;
}
inline Mat4Raw toRaw(const Mat4& m)
{
    Mat4Raw r;
    for (int i = 0; i < 16; ++i) {
        r.m[i] = m.m[i];
    }
    return r;
}

struct Mesh {
    MeshType type = MeshType::Null;
    int32_t numFrames = 1;
    int32_t numMatFrames = 1;
    int32_t parentMesh = -1;
    Box bounds;
    Vec3 center;
    float radius = 0;

    std::vector<Vec3> verts;
    std::vector<Vec2> tverts;
    std::vector<Vec2> tverts2;
    std::vector<Color8> colors;
    std::vector<Vec3> norms;
    std::vector<uint8_t> encodedNorms;
    std::vector<Primitive> primitives;
    std::vector<uint32_t> indices;
    std::vector<Vec4> tangents;
    int32_t vertsPerFrame = 0;
    uint32_t flags = 0;

    // Skin data: vertices are in shape space, bone b deforms with nodeWorld[nodeIndex[b]] * initialTransforms[b].
    std::vector<Vec3> initialVerts;
    std::vector<Vec3> initialNorms;
    std::vector<Mat4Raw> initialTransforms;
    std::vector<int32_t> vertexIndex;
    std::vector<int32_t> boneIndex;
    std::vector<float> weight;
    std::vector<int32_t> nodeIndex;

    bool isNull() const { return type == MeshType::Null; }
    bool isSkin() const { return type == MeshType::Skin; }
    // positions/normals used for rendering the rest pose
    const std::vector<Vec3>& restVerts() const { return isSkin() && !initialVerts.empty() ? initialVerts : verts; }
    const std::vector<Vec3>& restNorms() const { return isSkin() && !initialNorms.empty() ? initialNorms : norms; }
};

struct Material {
    std::string name;
    uint32_t flags = 3; // S_Wrap | T_Wrap
    uint32_t reflectanceMap = 0xFFFFFFFF;
    uint32_t bumpMap = 0xFFFFFFFF;
    uint32_t detailMap = 0xFFFFFFFF;
    float detailScale = 1;
    float reflectionAmount = 1;
};

enum MaterialFlag : uint32_t {
    SWrap = 1u << 0,
    TWrap = 1u << 1,
    Translucent = 1u << 2,
    Additive = 1u << 3,
    Subtractive = 1u << 4,
    SelfIlluminating = 1u << 5,
    NeverEnvMap = 1u << 6,
    NoMipMap = 1u << 7,
    MipMapZeroBorder = 1u << 8,
};

struct Shape {
    int readVersion = 0;
    int exporterVersion = kExporterCollada;

    float smallestVisibleSize = 0;
    int32_t smallestVisibleDL = 0;
    float radius = 0;
    float tubeRadius = 0;
    Vec3 center;
    Box bounds;

    std::vector<Node> nodes;
    std::vector<Object> objects;
    std::vector<int32_t> subShapeFirstNode, subShapeFirstObject, subShapeNumNodes, subShapeNumObjects;
    std::vector<Quat16> defaultRotations;
    std::vector<Vec3> defaultTranslations;
    std::vector<Quat16> nodeRotations;
    std::vector<Vec3> nodeTranslations;
    std::vector<float> nodeUniformScales;
    std::vector<Vec3> nodeAlignedScales;
    std::vector<Vec3> nodeArbitraryScaleFactors;
    std::vector<Quat16> nodeArbitraryScaleRots;
    std::vector<Vec3> groundTranslations;
    std::vector<Quat16> groundRotations;
    std::vector<ObjectState> objectStates;
    std::vector<Trigger> triggers;
    std::vector<Detail> details;
    std::vector<std::string> names;
    std::vector<Mesh> meshes;
    std::vector<Sequence> sequences;
    std::vector<Material> materials;

    const std::string& name(int index) const;
    int findName(const std::string& n) const; // case-insensitive, like the engine's name table
    int addName(const std::string& n);

    Mat4 nodeDefaultLocal(int node) const;
    Mat4 nodeDefaultWorld(int node) const;
    std::vector<Mat4> defaultWorldTransforms() const;
    int subShapeOfObject(int object) const;
    // The object's mesh for a detail level, null when the detail has none for it (other subshape, null mesh).
    const Mesh* detailMesh(int object, int detail) const;
    // Largest non-negative mesh detail, -1 without one.
    int highestVisibleDetail() const;

    // Fills firstObject/firstChild/nextSibling links and object nextSibling (computed at load in the engine).
    void rebuildLinks();
    // Mesh bounds/center/radius plus shape bounds, center, radius and tubeRadius from the rest pose.
    void computeBounds();
    // Recomputes smallestVisibleSize/DL from the details the way the engine does after load.
    void updateSmallestVisible();
    // True when objects reference meshes 0..n-1 consecutively in object order (what v31 writes).
    bool meshesCompact() const;
    // Reorders meshes into object order and drops unreferenced ones.
    void compactMeshes();
};

// Skin vertices (and optionally normals) posed with the given node world transforms.
void skinVerts(const Mesh& mesh, const std::vector<Mat4>& nodeWorld, std::vector<Vec3>& verts, std::vector<Vec3>* norms = nullptr);

// Triangle-list indices of one primitive (strips and fans expanded, degenerate strip joints skipped).
void primitiveTriangles(const Mesh& mesh, const Primitive& prim, std::vector<uint32_t>& out);
// Converts every primitive to an indexed triangle list in place.
void convertToTriangles(Mesh& mesh);

// Ground sample i is the bounds pose at time duration * i / (count - 1). Sample 0 is the pose at the start.
bool groundMoves(const Shape& shape, const Sequence& seq);
Mat4 groundTransform(const Shape& shape, const Sequence& seq, float time);

} // namespace ts
