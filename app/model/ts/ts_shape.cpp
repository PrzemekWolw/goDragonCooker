#include "model/ts/ts_shape.h"

#include <algorithm>
#include <limits>

#include "core/strings.h"

namespace ts {

bool IntegerSet::test(int i) const
{
    if (i < 0) {
        return false;
    }
    const size_t w = size_t(i) >> 5;
    return w < words.size() && (words[w] >> (i & 31)) & 1u;
}

void IntegerSet::set(int i)
{
    if (i < 0 || i >= kMaxSetBits) {
        return;
    }
    const size_t w = size_t(i) >> 5;
    if (words.size() <= w) {
        words.resize(w + 1, 0);
    }
    words[w] |= 1u << (i & 31);
}

static int popcount(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

int IntegerSet::rank(int i) const
{
    int n = 0;
    for (int w = 0; w < int(words.size()) && w * 32 < i; ++w) {
        const int bits = std::min(32, i - w * 32);
        const uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
        n += popcount(words[w] & mask);
    }
    return n;
}

int IntegerSet::count() const
{
    int n = 0;
    for (uint32_t w : words) {
        n += popcount(w);
    }
    return n;
}

bool IntegerSet::any() const
{
    for (uint32_t w : words) {
        if (w) {
            return true;
        }
    }
    return false;
}

int IntegerSet::usedWords() const
{
    int used = 0;
    for (int i = 0; i < int(words.size()); ++i) {
        if (words[i]) {
            used = i + 1;
        }
    }
    return used;
}

const std::string& Shape::name(int index) const
{
    static const std::string empty;
    return index >= 0 && index < int(names.size()) ? names[index] : empty;
}

int Shape::findName(const std::string& n) const
{
    const std::string key = toLower(n);
    for (int i = 0; i < int(names.size()); ++i) {
        if (toLower(names[i]) == key) {
            return i;
        }
    }
    return -1;
}

int Shape::addName(const std::string& n)
{
    const int found = findName(n);
    if (found >= 0) {
        return found;
    }
    names.push_back(n);
    return int(names.size()) - 1;
}

Mat4 Shape::nodeDefaultLocal(int node) const
{
    const Quat q = node < int(defaultRotations.size()) ? toQuat(defaultRotations[node]) : Quat{};
    const Vec3 t = node < int(defaultTranslations.size()) ? defaultTranslations[node] : Vec3{};
    return composeTRS(t, q, {1, 1, 1});
}

Mat4 Shape::nodeDefaultWorld(int node) const
{
    Mat4 m = nodeDefaultLocal(node);
    int guard = 0;
    for (int p = nodes[node].parentIndex; p >= 0 && p < int(nodes.size()) && guard < int(nodes.size()); p = nodes[p].parentIndex, ++guard) {
        m = nodeDefaultLocal(p) * m;
    }
    return m;
}

std::vector<Mat4> Shape::defaultWorldTransforms() const
{
    std::vector<Mat4> out(nodes.size());
    for (int i = 0; i < int(nodes.size()); ++i) {
        out[i] = nodeDefaultWorld(i);
    }
    return out;
}

int Shape::subShapeOfObject(int object) const
{
    for (int s = 0; s < int(subShapeFirstObject.size()); ++s) {
        const int first = subShapeFirstObject[s];
        const int count = s < int(subShapeNumObjects.size()) ? subShapeNumObjects[s] : 0;
        if (object >= first && object < first + count) {
            return s;
        }
    }
    return -1;
}

void Shape::rebuildLinks()
{
    for (Node& n : nodes) {
        n.firstObject = n.firstChild = n.nextSibling = -1;
    }
    for (int i = int(nodes.size()) - 1; i >= 0; --i) {
        const int p = nodes[i].parentIndex;
        if (p >= 0 && p < int(nodes.size())) {
            nodes[i].nextSibling = nodes[p].firstChild;
            nodes[p].firstChild = i;
        }
    }
    for (Object& o : objects) {
        o.nextSibling = -1;
    }
    for (int i = int(objects.size()) - 1; i >= 0; --i) {
        const int n = objects[i].nodeIndex;
        if (n >= 0 && n < int(nodes.size())) {
            objects[i].nextSibling = nodes[n].firstObject;
            nodes[n].firstObject = i;
        }
    }
}

void Box::extend(const Vec3& p, bool& first)
{
    if (first) {
        min = max = p;
        first = false;
        return;
    }
    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
}

const Mesh* Shape::detailMesh(int object, int detail) const
{
    const Object& obj = objects[object];
    const Detail& det = details[detail];
    if (det.subShapeNum != subShapeOfObject(object) || det.objectDetailNum < 0 || det.objectDetailNum >= obj.numMeshes) {
        return nullptr;
    }
    const int mi = obj.startMeshIndex + det.objectDetailNum;
    return mi < 0 || mi >= int(meshes.size()) || meshes[mi].isNull() ? nullptr : &meshes[mi];
}

int Shape::highestVisibleDetail() const
{
    int best = -1;
    for (int d = 0; d < int(details.size()); ++d) {
        const Detail& det = details[d];
        if (det.subShapeNum >= 0 && det.size >= 0 && (best < 0 || det.size > details[best].size)) {
            best = d;
        }
    }
    return best;
}

void Shape::computeBounds()
{
    for (Mesh& m : meshes) {
        if (m.isNull()) {
            continue;
        }
        const std::vector<Vec3>& v = m.restVerts();
        bool first = true;
        m.bounds = {};
        for (const Vec3& p : v) {
            m.bounds.extend(p, first);
        }
        m.center = (m.bounds.min + m.bounds.max) * 0.5f;
        float r = 0;
        for (const Vec3& p : v) {
            r = std::max(r, length(p - m.center));
        }
        m.radius = r;
    }

    // Shape bounds come from the visible details only; collision (negative size) details are excluded.
    const std::vector<Mat4> world = defaultWorldTransforms();
    bool first = true;
    bounds = {};
    std::vector<Vec3> points;
    for (int o = 0; o < int(objects.size()); ++o) {
        const Object& obj = objects[o];
        for (int d = 0; d < int(details.size()); ++d) {
            const Mesh* m = details[d].size < 0 ? nullptr : detailMesh(o, d);
            if (!m) {
                continue;
            }
            std::vector<Vec3> posed;
            if (m->isSkin()) {
                skinVerts(*m, world, posed);
            } else {
                const Mat4 xf = obj.nodeIndex < 0 ? Mat4{} : world[obj.nodeIndex];
                for (const Vec3& p : m->restVerts()) {
                    posed.push_back(mulPoint(xf, p));
                }
            }
            for (const Vec3& w : posed) {
                bounds.extend(w, first);
                points.push_back(w);
            }
        }
    }
    center = (bounds.min + bounds.max) * 0.5f;
    radius = 0;
    tubeRadius = 0;
    for (const Vec3& p : points) {
        const Vec3 d = p - center;
        radius = std::max(radius, length(d));
        tubeRadius = std::max(tubeRadius, std::sqrt(d.x * d.x + d.y * d.y));
    }
}

void Shape::updateSmallestVisible()
{
    smallestVisibleSize = std::numeric_limits<float>::max();
    smallestVisibleDL = -1;
    for (int i = 0; i < int(details.size()); ++i) {
        if (details[i].size >= 0 && details[i].size < smallestVisibleSize) {
            smallestVisibleSize = details[i].size;
            smallestVisibleDL = i;
        }
    }
    if (smallestVisibleDL < 0) {
        smallestVisibleSize = 0;
    }
}

void skinVerts(const Mesh& mesh, const std::vector<Mat4>& nodeWorld, std::vector<Vec3>& verts, std::vector<Vec3>* norms)
{
    const std::vector<Vec3>& src = mesh.restVerts();
    const std::vector<Vec3>& srcN = mesh.restNorms();
    std::vector<Mat4> bones(mesh.initialTransforms.size());
    for (size_t b = 0; b < bones.size(); ++b) {
        const int node = b < mesh.nodeIndex.size() ? mesh.nodeIndex[b] : -1;
        const Mat4 world = node >= 0 && node < int(nodeWorld.size()) ? nodeWorld[node] : Mat4{};
        bones[b] = world * toMat4(mesh.initialTransforms[b]);
    }
    verts.assign(src.size(), Vec3{});
    std::vector<float> total(src.size(), 0.0f);
    if (norms) {
        norms->assign(src.size(), Vec3{});
    }
    const size_t n = std::min({mesh.vertexIndex.size(), mesh.boneIndex.size(), mesh.weight.size()});
    for (size_t i = 0; i < n; ++i) {
        const int v = mesh.vertexIndex[i];
        const int b = mesh.boneIndex[i];
        if (v < 0 || v >= int(src.size()) || b < 0 || b >= int(bones.size())) {
            continue;
        }
        verts[v] += mulPoint(bones[b], src[v]) * mesh.weight[i];
        total[v] += mesh.weight[i];
        if (norms && v < int(srcN.size())) {
            (*norms)[v] += mulVector(bones[b], srcN[v]) * mesh.weight[i];
        }
    }
    for (size_t v = 0; v < src.size(); ++v) {
        if (total[v] <= 0.0f) {
            verts[v] = src[v];
            if (norms && v < srcN.size()) {
                (*norms)[v] = srcN[v];
            }
        } else if (norms) {
            (*norms)[v] = normalize((*norms)[v]);
        }
    }
}

bool Shape::meshesCompact() const
{
    int next = 0;
    for (const Object& o : objects) {
        if (o.numMeshes > 0 && o.startMeshIndex != next) {
            return false;
        }
        next += std::max(o.numMeshes, 0);
    }
    return next == int(meshes.size());
}

void Shape::compactMeshes()
{
    if (meshesCompact()) {
        return;
    }
    std::vector<Mesh> out;
    for (Object& o : objects) {
        const int start = int(out.size());
        for (int j = 0; j < o.numMeshes; ++j) {
            const int idx = o.startMeshIndex + j;
            out.push_back(idx >= 0 && idx < int(meshes.size()) ? meshes[idx] : Mesh{});
        }
        o.startMeshIndex = start;
    }
    meshes = std::move(out);
}

void primitiveTriangles(const Mesh& mesh, const Primitive& prim, std::vector<uint32_t>& out)
{
    const int s = prim.start, n = prim.numElements;
    if (n < 3 || s < 0 || size_t(s) + size_t(n) > mesh.indices.size()) {
        return;
    }
    const uint32_t* idx = mesh.indices.data() + s;
    switch (prim.type()) {
    case Primitive::Strip:
        for (int k = 0; k + 2 < n; ++k) {
            const uint32_t a = idx[k], b = idx[k + 1], c = idx[k + 2];
            if (a == b || b == c || a == c) {
                continue;
            }
            if (k & 1) {
                out.insert(out.end(), {a, c, b});
            } else {
                out.insert(out.end(), {a, b, c});
            }
        }
        break;
    case Primitive::Fan:
        for (int k = 1; k + 1 < n; ++k) {
            out.insert(out.end(), {idx[0], idx[k], idx[k + 1]});
        }
        break;
    default:
        for (int k = 0; k + 2 < n; k += 3) {
            out.insert(out.end(), {idx[k], idx[k + 1], idx[k + 2]});
        }
        break;
    }
}

void convertToTriangles(Mesh& mesh)
{
    bool needed = false;
    for (const Primitive& p : mesh.primitives) {
        needed |= p.type() != Primitive::Triangles;
    }
    if (!needed) {
        return;
    }
    std::vector<Primitive> prims;
    std::vector<uint32_t> indices;
    for (const Primitive& p : mesh.primitives) {
        std::vector<uint32_t> tris;
        primitiveTriangles(mesh, p, tris);
        if (tris.empty()) {
            continue;
        }
        Primitive out;
        out.start = int32_t(indices.size());
        out.numElements = int32_t(tris.size());
        out.matIndex = (p.matIndex & ~Primitive::TypeMask) | Primitive::Triangles | Primitive::Indexed;
        indices.insert(indices.end(), tris.begin(), tris.end());
        prims.push_back(out);
    }
    mesh.primitives = std::move(prims);
    mesh.indices = std::move(indices);
}

} // namespace ts
