#include "model/validate/validate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <assimp/scene.h>

#include "core/i18n.h"
#include "core/strings.h"

namespace model {
namespace {

using namespace ts;

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kShown = 4; // names listed in one report line

// The first kShown of `names`, with "..." when `total` is larger.
std::string listNames(const std::vector<std::string>& names, size_t total)
{
    std::string s;
    for (size_t i = 0; i < names.size() && i < kShown; ++i) {
        s += (i ? ", " : "") + (names[i].empty() ? std::string("\"\"") : names[i]);
    }
    if (total > std::min(names.size(), kShown)) {
        s += s.empty() ? "..." : ", ...";
    }
    return s;
}

void addLine(ValidationReport& rep, Verdict v, const std::string& text)
{
    if (int(v) > int(rep.verdict)) {
        rep.verdict = v;
    }
    rep.lines.push_back(std::string(v == Verdict::Pass ? "" : v == Verdict::Warn ? "WARN " : "FAIL ") + text);
}

double angleDeg(const Vec3& a, const Vec3& b)
{
    const float la = length(a), lb = length(b);
    if (la <= 0 || lb <= 0) {
        return 0;
    }
    const double c = std::clamp(double(dot(a, b)) / (double(la) * lb), -1.0, 1.0);
    return std::acos(c) * 180.0 / kPi;
}

struct CellKey {
    int64_t x, y, z;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    size_t operator()(const CellKey& k) const
    {
        return size_t(k.x * 73856093LL) ^ size_t(k.y * 19349663LL) ^ size_t(k.z * 83492791LL);
    }
};

CellKey cellOf(const Vec3& p, double cell)
{
    return {int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell))};
}

struct Stat {
    double max = 0;
    double sumSq = 0;
    size_t count = 0;
    void add(double v)
    {
        max = std::max(max, v);
        sumSq += v * v;
        ++count;
    }
    double rms() const { return count ? std::sqrt(sumSq / double(count)) : 0.0; }
};

// Nearest-point distances from every unique position of `from` to the positions of `to`.
struct PointCloudResult {
    Stat dist;
    size_t outliers = 0; // nothing within the search radius
};

std::vector<Vec3> uniquePositions(const std::vector<const Tri*>& tris, double q)
{
    std::unordered_set<CellKey, CellHash> seen;
    std::vector<Vec3> out;
    for (const Tri* t : tris) {
        for (const Corner& c : t->c) {
            if (seen.insert(cellOf(c.pos, q)).second) {
                out.push_back(c.pos);
            }
        }
    }
    return out;
}

PointCloudResult pointDistance(const std::vector<Vec3>& from, const std::vector<Vec3>& to, double cell)
{
    PointCloudResult r;
    std::unordered_map<CellKey, std::vector<Vec3>, CellHash> grid;
    for (const Vec3& p : to) {
        grid[cellOf(p, cell)].push_back(p);
    }
    for (const Vec3& p : from) {
        const CellKey k = cellOf(p, cell);
        double best = 1e300;
        // Points in ring r+1 are at least r cells away, so ring r is enough once best <= r cells.
        for (int ring = 0; ring <= 2 && (ring == 0 || best > (ring - 1) * cell); ++ring) {
            for (int64_t dx = -ring; dx <= ring; ++dx) {
                for (int64_t dy = -ring; dy <= ring; ++dy) {
                    for (int64_t dz = -ring; dz <= ring; ++dz) {
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring) {
                            continue;
                        }
                        auto it = grid.find({k.x + dx, k.y + dy, k.z + dz});
                        if (it == grid.end()) {
                            continue;
                        }
                        for (const Vec3& q : it->second) {
                            best = std::min(best, double(length(q - p)));
                        }
                    }
                }
            }
        }
        if (best > 2.0 * cell) {
            ++r.outliers;
            r.dist.add(2.0 * cell);
        } else {
            r.dist.add(best);
        }
    }
    return r;
}

struct Pair {
    const Tri* a;
    const Tri* b;
    int map[3]; // corner i of a matches corner map[i] of b
    bool flipped;
};

struct MatchResult {
    std::vector<Pair> pairs;
    size_t unmatchedA = 0, unmatchedB = 0;
    size_t droppedDegenerate = 0; // source triangles below the tolerance, not compared (encoders may drop them)
};

// Zero-area or smaller than `size`: cannot be told apart from its neighbours.
bool isDegenerate(const Tri* t, double size)
{
    const Vec3 e1 = t->c[1].pos - t->c[0].pos, e2 = t->c[2].pos - t->c[0].pos;
    const double area2 = length(cross(e1, e2));
    const double longest = std::max({length(e1), length(e2), length(t->c[2].pos - t->c[1].pos)});
    return longest <= size || area2 <= size * std::max(longest, 1e-30);
}

// Overlapping triangles (decals, double-sided copies) share positions; `attrCost` picks the
// candidate whose attributes agree best.
using AttrCost = std::function<double(const Tri*, const Tri*, const int*)>;

MatchResult matchTriangles(const std::vector<const Tri*>& A, const std::vector<const Tri*>& B, double tol, const AttrCost& attrCost)
{
    MatchResult r;
    const double cell = std::max(tol * 2.0, 1e-12);
    std::unordered_map<CellKey, std::vector<size_t>, CellHash> grid;
    auto centroid = [](const Tri* t) { return (t->c[0].pos + t->c[1].pos + t->c[2].pos) * (1.0f / 3.0f); };
    // Below 2 tol: not compared. Up to 4 tol: compared, but an unmatched one is not reported
    // missing, since its partner may have fallen below the cut.
    std::vector<char> used(B.size(), 0);
    for (size_t i = 0; i < B.size(); ++i) {
        if (isDegenerate(B[i], tol * 2.0)) {
            used[i] = 1;
            continue;
        }
        grid[cellOf(centroid(B[i]), cell)].push_back(i);
    }
    for (const Tri* a : A) {
        if (isDegenerate(a, tol * 2.0)) {
            ++r.droppedDegenerate;
            continue;
        }
        const CellKey k = cellOf(centroid(a), cell);
        double bestD = 1e300;
        Pair best{a, nullptr, {0, 1, 2}, false};
        size_t bestIndex = 0;
        for (int64_t dx = -1; dx <= 1; ++dx) {
            for (int64_t dy = -1; dy <= 1; ++dy) {
                for (int64_t dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find({k.x + dx, k.y + dy, k.z + dz});
                    if (it == grid.end()) {
                        continue;
                    }
                    for (size_t bi : it->second) {
                        if (used[bi]) {
                            continue;
                        }
                        const Tri* b = B[bi];
                        for (int flip = 0; flip < 2; ++flip) {
                            for (int rot = 0; rot < 3; ++rot) {
                                int map[3];
                                double d = 0;
                                for (int i = 0; i < 3; ++i) {
                                    map[i] = flip ? (rot - i + 3) % 3 : (rot + i) % 3;
                                    d = std::max(d, double(length(a->c[i].pos - b->c[map[i]].pos)));
                                }
                                if (d > tol) {
                                    continue;
                                }
                                // Same winding wins ties against a flipped duplicate.
                                const double score = attrCost(a, b, map) + (flip ? 0.5 : 0.0) + d / std::max(tol, 1e-30) * 1e-3;
                                if (score < bestD) {
                                    bestD = score;
                                    best = {a, b, {map[0], map[1], map[2]}, flip != 0};
                                    bestIndex = bi;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (best.b) {
            used[bestIndex] = 1;
            r.pairs.push_back(best);
        } else if (isDegenerate(a, tol * 4.0)) {
            ++r.droppedDegenerate;
        } else {
            ++r.unmatchedA;
        }
    }
    for (size_t i = 0; i < B.size(); ++i) {
        r.unmatchedB += !used[i] && !isDegenerate(B[i], tol * 4.0);
    }
    return r;
}

struct EdgeKey {
    CellKey a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};
struct EdgeHash {
    size_t operator()(const EdgeKey& k) const { return CellHash()(k.a) * 31 ^ CellHash()(k.b); }
};

bool lessCell(const CellKey& a, const CellKey& b)
{
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}

// Edges shared by exactly two triangles, with the larger normal split across them at either endpoint.
std::unordered_map<EdgeKey, double, EdgeHash> edgeCreases(const std::vector<const Tri*>& tris, double q)
{
    struct Side {
        Vec3 na, nb; // normals at the edge's (sorted) endpoints
    };
    std::unordered_map<EdgeKey, std::vector<Side>, EdgeHash> edges;
    for (const Tri* t : tris) {
        for (int i = 0; i < 3; ++i) {
            const Corner& c0 = t->c[i];
            const Corner& c1 = t->c[(i + 1) % 3];
            CellKey k0 = cellOf(c0.pos, q), k1 = cellOf(c1.pos, q);
            if (k0 == k1) {
                continue;
            }
            const bool swap = lessCell(k1, k0);
            edges[swap ? EdgeKey{k1, k0} : EdgeKey{k0, k1}].push_back(swap ? Side{c1.nrm, c0.nrm} : Side{c0.nrm, c1.nrm});
        }
    }
    std::unordered_map<EdgeKey, double, EdgeHash> out;
    for (const auto& [key, sides] : edges) {
        if (sides.size() == 2) {
            out[key] = std::max(angleDeg(sides[0].na, sides[1].na), angleDeg(sides[0].nb, sides[1].nb));
        }
    }
    return out;
}

Mat4 rotOnly(const Mat4& m)
{
    Mat4 r = m;
    r.at(0, 3) = r.at(1, 3) = r.at(2, 3) = 0;
    return r;
}

double rotDiffDeg(const Mat4& a, const Mat4& b)
{
    const Quat qa = quatFromMatrix(rotOnly(a)), qb = quatFromMatrix(rotOnly(b));
    const double d = std::fabs(double(qa.x) * qb.x + double(qa.y) * qb.y + double(qa.z) * qb.z + double(qa.w) * qb.w);
    return 2.0 * std::acos(std::min(1.0, d)) * 180.0 / kPi;
}

Vec3 translationOf(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

} // namespace

const char* verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Pass:
        return "pass";
    case Verdict::Warn:
        return "warn";
    default:
        return "fail";
    }
}

Capabilities intersect(const Capabilities& a, const Capabilities& b)
{
    Capabilities c;
    c.hierarchy = a.hierarchy && b.hierarchy;
    c.materials = a.materials && b.materials;
    c.normals = a.normals && b.normals;
    c.uv = a.uv && b.uv;
    c.uv2 = a.uv2 && b.uv2;
    c.colors = a.colors && b.colors;
    c.tangents = a.tangents && b.tangents;
    c.skins = a.skins && b.skins;
    c.animations = a.animations && b.animations;
    c.morphs = a.morphs && b.morphs;
    c.lods = a.lods && b.lods;
    c.meshNames = a.meshNames && b.meshNames;
    c.poses = a.poses && b.poses;
    return c;
}

Tolerance toleranceFor(const Format& target, const nlohmann::json& settings)
{
    Tolerance t;
    if (target.id == "gltf2" && settingBool(target, settings, "draco")) {
        // Quantized to 2^bits steps over the attribute range; allow two steps. Positions are not quantized.
        // Octahedral normals: one step is about 360 / 2^bits degrees.
        t.normalDeg = std::max(t.normalDeg, 2.0 * 360.0 / double(1u << settingValue(target, settings, "qNormal")));
        t.uv = std::max(t.uv, 2.0 / double(1u << settingValue(target, settings, "qUv")));
        t.uvRangeRelative = true;
        t.color = std::max(t.color, int(std::ceil(2.0 * 255.0 / double(1u << settingValue(target, settings, "qColor")))));
        t.tangentDeg = std::max(t.tangentDeg, 2.0 * 180.0 / double(1u << settingValue(target, settings, "qGeneric")));
    }
    if (target.isTs) {
        t.rotDeg = std::max(t.rotDeg, 0.02); // Quat16 storage
    }
    return t;
}

void checkUpAxis(ValidationReport& rep, UpAxis expected, const RawFrame& raw, const Summary& source)
{
    auto line = [&](Verdict v, const std::string& text) {
        if (int(v) > int(rep.verdict)) {
            rep.verdict = v;
        }
        rep.lines.push_back(std::string(v == Verdict::Pass ? "" : v == Verdict::Warn ? "WARN " : "FAIL ") + text);
    };
    nlohmann::json& j = rep.json["upAxis"];
    j["expected"] = upAxisName(expected);
    auto axisLabel = [](UpAxis axis, bool negative) {
        return negative ? format(tr("gdc.model.warn.negativeAxis"), upAxisName(axis)) : std::string(upAxisName(axis));
    };
    if (!raw.ok) {
        line(Verdict::Warn, format(tr("gdc.model.warn.upUnread"), raw.error.c_str()));
        j["checked"] = false;
        rep.json["verdict"] = verdictName(rep.verdict);
        return;
    }
    Verdict v = Verdict::Pass;
    std::string text = format(tr("gdc.model.warn.upOk"), upAxisName(expected));
    if (raw.declared) {
        const bool match = *raw.declared == expected && !raw.declaredNegative;
        j["declared"] = std::string(raw.declaredNegative ? "-" : "") + upAxisName(*raw.declared);
        if (!match) {
            v = Verdict::Fail;
            text = format(tr("gdc.model.warn.upWrong"), axisLabel(*raw.declared, raw.declaredNegative).c_str(), upAxisName(expected));
        }
    }
    // FBX UnitScaleFactor and COLLADA meter. The game multiplies the model by this and does not
    // convert FBX centimeters into meters, so only 1 keeps the original size.
    if (raw.gameScale) {
        j["unitScale"] = *raw.gameScale;
        if (std::abs(*raw.gameScale - 1.0) > 1e-4) {
            v = Verdict::Fail;
            text += " ";
            text += format(tr("gdc.model.warn.unitScale"), *raw.gameScale);
        }
    }
    if (raw.hasGeometry && !source.tris.empty()) {
        const ts::Vec3 lo = source.bounds.min, hi = source.bounds.max;
        ts::Vec3 wantMin = lo, wantMax = hi;
        if (expected == UpAxis::Y) { // (x, y, z) -> (x, z, -y)
            wantMin = {lo.x, lo.z, -hi.y};
            wantMax = {hi.x, hi.z, -lo.y};
        }
        const double tol = 1e-3 * std::max(double(source.diagonal()), 1e-6);
        const float d[6] = {raw.bounds.min.x - wantMin.x, raw.bounds.min.y - wantMin.y, raw.bounds.min.z - wantMin.z,
                            raw.bounds.max.x - wantMax.x, raw.bounds.max.y - wantMax.y, raw.bounds.max.z - wantMax.z};
        double worst = 0;
        for (float x : d) {
            worst = std::max(worst, double(std::fabs(x)));
        }
        j["extentsDeviation"] = worst;
        // A writer that drops meshes shrinks the extents without turning the model: those still sit inside
        // the expected box, and the geometry check reports the loss.
        const bool inside = d[0] >= -tol && d[1] >= -tol && d[2] >= -tol && d[3] <= tol && d[4] <= tol && d[5] <= tol;
        if (worst > tol && inside) {
            text += " ";
            text += tr("gdc.model.warn.upSmaller");
            j["incomplete"] = true;
        } else if (worst > tol) {
            v = Verdict::Fail;
            text += " ";
            text += format(tr("gdc.model.warn.upSize"), raw.bounds.max.x - raw.bounds.min.x, raw.bounds.max.y - raw.bounds.min.y,
                           raw.bounds.max.z - raw.bounds.min.z, wantMax.x - wantMin.x, wantMax.y - wantMin.y, wantMax.z - wantMin.z);
        } else {
            text += " ";
            text += tr("gdc.model.warn.upSizeOk");
            if (raw.gameScale && std::abs(*raw.gameScale - 1.0) <= 1e-4) {
                text += " ";
                text += tr("gdc.model.warn.unitOk");
            }
        }
    }
    line(v, text);
    j["verdict"] = verdictName(v);
    rep.json["verdict"] = verdictName(rep.verdict);
}

namespace {

bool treeHasLodAnchor(const aiNode* node)
{
    if (!node) {
        return false;
    }
    if (isBaseStart(node) || lodAnchor(node)) {
        return true;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (treeHasLodAnchor(node->mChildren[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

void checkGameLod(ValidationReport& report, const aiScene& written, const aiScene& source, bool hierarchy)
{
    if (!treeHasLodAnchor(source.mRootNode)) {
        return;
    }
    if (!hierarchy) {
        if (int(Verdict::Warn) > int(report.verdict)) {
            report.verdict = Verdict::Warn;
        }
        report.lines.push_back(std::string("WARN ") + tr("gdc.model.warn.lodNoNames"));
        report.json["lod"] = {{"source", true}, {"detected", false}};
        report.json["verdict"] = verdictName(report.verdict);
        return;
    }
    const aiNode* root = written.mRootNode;
    const char* seen = root ? root->mName.C_Str() : "";
    const bool detected = isBaseStart(root) || lodAnchor(root);
    auto raise = [&](Verdict v) {
        if (int(v) > int(report.verdict)) {
            report.verdict = v;
        }
    };
    report.json["lod"] = {{"source", true}, {"detected", detected}};
    if (!detected) {
        raise(Verdict::Fail);
        report.lines.push_back(std::string("FAIL ") +
                               format(tr("gdc.model.warn.lodMissing"), seen[0] ? seen : tr("gdc.model.warn.unnamed")));
    } else {
        report.lines.push_back(tr("gdc.model.warn.lodOk"));
    }
    report.json["verdict"] = verdictName(report.verdict);
}

void checkStoredNames(ValidationReport& report, const std::vector<std::pair<std::string, std::string>>& restored,
                      const aiScene& written)
{
    // Load puts back the real name from the file's extras; the game and Blender ignore those and see the stored one.
    if (restored.empty()) {
        report.lines.push_back(tr("gdc.model.warn.namesStoredOk"));
    } else {
        std::vector<std::string> shown;
        for (size_t i = 0; i < restored.size() && i < kShown; ++i) {
            shown.push_back(format("%s -> %s", restored[i].first.c_str(), restored[i].second.c_str()));
        }
        addLine(report, Verdict::Fail, format(tr("gdc.model.warn.namesStored"), restored.size(), listNames(shown, restored.size()).c_str()));
    }
    report.json["storedNames"] = {{"renamed", restored.size()}};

    // Two materials with one name are told apart by Blender with ".001" and by the game not at all.
    std::map<std::string, size_t> uses;
    for (unsigned i = 0; i < written.mNumMaterials; ++i) {
        aiString name;
        if (written.mMaterials[i]->Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS && name.length) {
            ++uses[name.C_Str()];
        }
    }
    std::vector<std::string> repeated;
    size_t repeatedCount = 0;
    for (const auto& [name, count] : uses) {
        if (count > 1) {
            ++repeatedCount;
            repeated.push_back(name);
        }
    }
    report.json["storedNames"]["duplicateMaterials"] = repeatedCount;
    if (repeatedCount) {
        addLine(report, Verdict::Warn, format(tr("gdc.model.warn.matRepeated"), repeatedCount, listNames(repeated, repeatedCount).c_str()));
    }
    report.json["verdict"] = verdictName(report.verdict);
}

void checkImportNames(ValidationReport& report, const aiScene& written, bool gltf)
{
    auto warn = [&](const char* id, size_t count, const std::vector<std::string>& names) {
        addLine(report, Verdict::Warn, format(tr(id), count, listNames(names, count).c_str()));
    };
    size_t problems = 0;

    // Blender's glTF importer builds one material per vertex-color use ("name" and "name.001") when a material is
    // on primitives with and without COLOR_0.
    if (gltf) {
        std::vector<char> colored(written.mNumMaterials, 0), plain(written.mNumMaterials, 0);
        for (unsigned i = 0; i < written.mNumMeshes; ++i) {
            const aiMesh* m = written.mMeshes[i];
            if (m->mMaterialIndex < written.mNumMaterials) {
                (m->HasVertexColors(0) ? colored : plain)[m->mMaterialIndex] = 1;
            }
        }
        std::vector<std::string> split;
        size_t splitCount = 0;
        for (unsigned i = 0; i < written.mNumMaterials; ++i) {
            if (colored[i] && plain[i]) {
                ++splitCount;
                aiString name;
                written.mMaterials[i]->Get(AI_MATKEY_NAME, name);
                if (split.size() < kShown) {
                    split.push_back(name.length ? name.C_Str() : format("#%u", i));
                }
            }
        }
        report.json["importNames"]["splitMaterials"] = splitCount;
        if (splitCount) {
            warn("gdc.model.warn.matSplit", splitCount, split);
            ++problems;
        }
    }

    // Blender keeps one name per object and per mesh, so a name used by two parts becomes "name.001".
    std::map<std::string, size_t> nodeUses;
    std::map<std::string, std::set<const aiNode*>> meshHolders;
    auto walk = [&](auto& self, const aiNode* n, bool root) -> void {
        if (!root && n->mName.length) {
            ++nodeUses[n->mName.C_Str()];
        }
        for (unsigned i = 0; i < n->mNumMeshes; ++i) {
            if (n->mMeshes[i] < written.mNumMeshes) {
                meshHolders[written.mMeshes[n->mMeshes[i]]->mName.C_Str()].insert(n);
            }
        }
        for (unsigned i = 0; i < n->mNumChildren; ++i) {
            self(self, n->mChildren[i], false);
        }
    };
    if (written.mRootNode) {
        walk(walk, written.mRootNode, true);
    }
    std::vector<std::string> nodeNames, meshNames;
    size_t nodeCount = 0, meshCount = 0;
    for (const auto& [name, count] : nodeUses) {
        if (count > 1) {
            ++nodeCount;
            if (nodeNames.size() < kShown) {
                nodeNames.push_back(name);
            }
        }
    }
    for (const auto& [name, holders] : meshHolders) {
        if (holders.size() > 1) {
            ++meshCount;
            if (meshNames.size() < kShown) {
                meshNames.push_back(name);
            }
        }
    }
    report.json["importNames"]["duplicateNodes"] = nodeCount;
    report.json["importNames"]["duplicateMeshes"] = meshCount;
    if (nodeCount) {
        warn("gdc.model.warn.nodesRepeated", nodeCount, nodeNames);
        ++problems;
    }
    if (meshCount) {
        warn("gdc.model.warn.meshesRepeated", meshCount, meshNames);
        ++problems;
    }
    if (!problems) {
        report.lines.push_back(tr("gdc.model.warn.importNamesOk"));
    }
    report.json["verdict"] = verdictName(report.verdict);
}

namespace {

// Nodes by lowercase name (first of a repeated name).
using NodeMap = std::map<std::string, const NodePose*>;

NodeMap nodeMap(const Summary& s)
{
    NodeMap m;
    for (const NodePose& n : s.nodes) {
        m.emplace(toLower(n.name), &n);
    }
    return m;
}

// Nearest ancestor (or the node itself with `self`) whose name is in `common`; "" when there is none.
std::string commonAncestor(const NodeMap& all, std::string name, const std::set<std::string>& common, bool self)
{
    if (!self) {
        auto it = all.find(name);
        name = it != all.end() ? toLower(it->second->parent) : std::string();
    }
    for (size_t guard = 0; !name.empty() && guard <= all.size(); ++guard) {
        if (common.count(name)) {
            return name;
        }
        auto it = all.find(name);
        name = it != all.end() ? toLower(it->second->parent) : std::string();
    }
    return {};
}

std::set<int> usedObjects(const Summary& s)
{
    std::set<int> used;
    for (const Tri& t : s.tris) {
        if (t.object >= 0 && t.object < int(s.objects.size())) {
            used.insert(t.object);
        }
    }
    return used;
}

std::set<std::string> usedMaterials(const Summary& s)
{
    std::set<std::string> used;
    for (const Tri& t : s.tris) {
        if (t.material >= 0 && t.material < int(s.materials.size())) {
            used.insert(s.materials[t.material]);
        }
    }
    return used;
}

// Placeholder materials the converter or an importer gives triangles that had none.
bool placeholderMaterial(const std::string& name) { return name == "NoMaterial" || name == "DefaultMaterial"; }

// Node tree, node poses, mesh names, detail levels and material names against the source.
void checkStructure(ValidationReport& rep, const Summary& src, const Summary& dst, const Capabilities& caps, const Tolerance& tol,
                    const StructureHints& hints, double diag)
{
    nlohmann::json& js = rep.json["structure"];

    std::vector<std::string> renamed;
    for (const Summary* s : {&src, &dst}) {
        for (const auto& [from, to] : s->renamed) {
            renamed.push_back(from + " -> " + to);
        }
    }
    if (!renamed.empty()) {
        addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.renamed"), renamed.size(), listNames(renamed, renamed.size()).c_str()));
    }
    js["renamed"] = renamed.size();

    // --- nodes, tree, pose ---
    const NodeMap allA = nodeMap(src), allB = nodeMap(dst);
    std::set<std::string> inserted, dropped;
    for (const std::string& n : hints.inserted) {
        inserted.insert(toLower(n));
    }
    for (const std::string& n : hints.dropped) {
        dropped.insert(toLower(n));
    }
    std::set<std::string> common;
    std::vector<std::string> missing, extra;
    for (const auto& [name, n] : allA) {
        if (allB.count(name)) {
            common.insert(name);
        } else if (!dropped.count(name)) {
            missing.push_back(n->name);
        }
    }
    for (const auto& [name, n] : allB) {
        if (!allA.count(name) && !inserted.count(name)) {
            extra.push_back(n->name);
        }
    }
    if (caps.hierarchy) {
        std::vector<std::string> reparented, moved;
        double maxRot = 0, maxTrans = 0;
        for (const std::string& name : common) {
            const NodePose* a = allA.at(name);
            const NodePose* b = allB.at(name);
            const std::string pa = commonAncestor(allA, name, common, false), pb = commonAncestor(allB, name, common, false);
            if (pa != pb) {
                reparented.push_back(format("%s (%s -> %s)", a->name.c_str(), pa.empty() ? "-" : pa.c_str(), pb.empty() ? "-" : pb.c_str()));
            }
            // Mesh holders are placed by their geometry, which the triangle match checks.
            if (!caps.poses || a->meshHolder || b->meshHolder) {
                continue;
            }
            const double trans = length(translationOf(a->world) - translationOf(b->world));
            const double rot = a->mirrored || b->mirrored ? 0.0 : rotDiffDeg(a->world, b->world);
            maxRot = std::max(maxRot, rot);
            maxTrans = std::max(maxTrans, trans);
            if (rot > tol.rotDeg || trans > tol.transRel * diag) {
                moved.push_back(a->name);
            }
        }
        js["nodes"] = allA.size();
        js["nodesMissing"] = missing.size();
        js["nodesExtra"] = extra.size();
        js["tree"] = {{"compared", common.size()}, {"reparented", reparented.size()}};
        if (caps.poses) {
            js["pose"] = {{"moved", moved.size()}, {"maxRotDeg", maxRot}, {"maxTrans", maxTrans}};
        }
        if (!missing.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.namesParts"), allA.size() - missing.size(), allA.size(),
                                               listNames(missing, missing.size()).c_str()));
        }
        if (!extra.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.nodesExtra"), extra.size(), listNames(extra, extra.size()).c_str()));
        }
        if (!reparented.empty()) {
            addLine(rep, Verdict::Fail,
                    format(tr("gdc.model.warn.treeParent"), reparented.size(), listNames(reparented, reparented.size()).c_str()));
        }
        if (!moved.empty()) {
            addLine(rep, Verdict::Fail,
                    format(tr("gdc.model.warn.poseMoved"), moved.size(), listNames(moved, moved.size()).c_str(), maxRot, maxTrans));
        }
        if (!common.empty() && missing.empty() && extra.empty() && reparented.empty() && moved.empty()) {
            addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.treeOk"), common.size()));
        }
    }

    // --- mesh names ---
    if (!caps.meshNames) {
        if (!src.objects.empty()) {
            addLine(rep, Verdict::Pass, tr("gdc.model.warn.meshNamesNone"));
        }
    } else if (src.tsObjects == dst.tsObjects) {
        std::map<std::string, const ObjectInfo*> objA, objB;
        for (int i : usedObjects(src)) {
            objA.emplace(toLower(src.objects[i].name), &src.objects[i]);
        }
        for (int i : usedObjects(dst)) {
            objB.emplace(toLower(dst.objects[i].name), &dst.objects[i]);
        }
        std::vector<std::string> lost, added, owners;
        for (const auto& [name, o] : objA) {
            auto it = objB.find(name);
            if (it == objB.end()) {
                lost.push_back(o->name);
                continue;
            }
            if (!caps.hierarchy || o->node.empty() || it->second->node.empty()) {
                continue;
            }
            const std::string na = commonAncestor(allA, toLower(o->node), common, true);
            const std::string nb = commonAncestor(allB, toLower(it->second->node), common, true);
            if (na != nb) {
                owners.push_back(format("%s (%s -> %s)", o->name.c_str(), na.empty() ? "-" : na.c_str(), nb.empty() ? "-" : nb.c_str()));
            }
        }
        for (const auto& [name, o] : objB) {
            if (!objA.count(name)) {
                added.push_back(o->name);
            }
        }
        js["meshNames"] = {{"meshes", objA.size()}, {"lost", lost.size()}, {"extra", added.size()}, {"otherNode", owners.size()}};
        if (!lost.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.meshNamesLost"), lost.size(), objA.size(), listNames(lost, lost.size()).c_str()));
        }
        if (!added.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.meshNamesExtra"), added.size(), listNames(added, added.size()).c_str()));
        }
        if (!owners.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.meshOwner"), owners.size(), listNames(owners, owners.size()).c_str()));
        }
        if (lost.empty() && added.empty() && owners.empty()) {
            addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.meshNamesOk"), objA.size()));
        }
    }

    // --- detail levels ---
    if (caps.lods && !hints.singleDetail && !src.details.empty()) {
        std::multiset<int> sa, sb;
        for (const auto& d : src.details) {
            sa.insert(int(std::lround(d.second)));
        }
        for (const auto& d : dst.details) {
            sb.insert(int(std::lround(d.second)));
        }
        auto sizes = [](const std::multiset<int>& s) {
            std::string out;
            for (int v : s) {
                out += (out.empty() ? "" : " ") + std::to_string(v);
            }
            return out;
        };
        std::vector<std::string> renamedDetails;
        const bool named = src.tsObjects && dst.tsObjects && !src.details.front().first.empty() && !dst.details.empty() &&
                           !dst.details.front().first.empty();
        if (named && src.details.size() == dst.details.size()) {
            for (size_t i = 0; i < src.details.size(); ++i) {
                if (toLower(src.details[i].first) != toLower(dst.details[i].first)) {
                    renamedDetails.push_back(src.details[i].first + " -> " + dst.details[i].first);
                }
            }
        }
        js["detailSizes"] = sizes(sa);
        js["resultDetailSizes"] = sizes(sb);
        if (sa != sb) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.detailSizes"), sizes(sb).c_str(), sizes(sa).c_str()));
        } else if (!renamedDetails.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.detailNames"), renamedDetails.size(),
                                               listNames(renamedDetails, renamedDetails.size()).c_str()));
        } else {
            addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.detailsOk"), src.details.size()));
        }
    }

    // --- material names ---
    if (caps.materials) {
        const std::set<std::string> usedA = usedMaterials(src), usedB = usedMaterials(dst);
        const bool untextured = std::any_of(src.tris.begin(), src.tris.end(), [&](const Tri& t) {
            return t.material < 0 || t.material >= int(src.materials.size()) || src.materials[t.material].empty();
        });
        std::vector<std::string> lost, added, unused;
        for (const std::string& m : usedA) {
            if (!usedB.count(m)) {
                lost.push_back(m);
            }
        }
        for (const std::string& m : usedB) {
            if (!usedA.count(m) && !(untextured && placeholderMaterial(m))) {
                added.push_back(m);
            }
        }
        const std::set<std::string> dstAll(dst.materials.begin(), dst.materials.end());
        for (const std::string& m : std::set<std::string>(src.materials.begin(), src.materials.end())) {
            if (!usedA.count(m) && !dstAll.count(m) && !m.empty()) {
                unused.push_back(m);
            }
        }
        js["materials"] = usedA.size();
        js["materialsMissing"] = lost.size();
        js["materialsExtra"] = added.size();
        js["materialsUnusedDropped"] = unused.size();
        if (!lost.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.namesMats"), usedA.size() - lost.size(), usedA.size(),
                                               listNames(lost, lost.size()).c_str()));
        }
        if (!added.empty()) {
            addLine(rep, Verdict::Fail, format(tr("gdc.model.warn.matsExtra"), added.size(), listNames(added, added.size()).c_str()));
        }
        if (lost.empty() && added.empty()) {
            addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.matsOk"), usedA.size()));
        }
        if (!unused.empty()) {
            addLine(rep, Verdict::Pass, format(tr("gdc.model.warn.matsUnused"), unused.size(), listNames(unused, unused.size()).c_str()));
        }
    }
}

} // namespace

ValidationReport compareSummaries(const Summary& src, const Summary& dst, const Capabilities& caps, const Tolerance& tol,
                                  const StructureHints& hints)
{
    ValidationReport rep;
    nlohmann::json& j = rep.json;
    auto raise = [&](Verdict v) {
        if (int(v) > int(rep.verdict)) {
            rep.verdict = v;
        }
    };
    auto line = [&](Verdict v, const std::string& text) {
        raise(v);
        rep.lines.push_back(std::string(v == Verdict::Pass ? "" : v == Verdict::Warn ? "WARN " : "FAIL ") + text);
    };

    const double diag = std::max(double(src.diagonal()), 1e-6);
    const double posTol = tol.posRel * diag;

    // --- structure ---
    {
        j["structure"] = {{"details", src.details.size()},      {"resultDetails", dst.details.size()},
                          {"skinnedMeshes", src.skinnedMeshes}, {"resultSkinnedMeshes", dst.skinnedMeshes},
                          {"animations", src.anims.size()},     {"resultAnimations", dst.anims.size()}};
        checkStructure(rep, src, dst, caps, tol, hints, diag);
        if (caps.skins && src.skinnedMeshes > 0 && dst.skinnedMeshes == 0) {
            line(Verdict::Fail, format(tr("gdc.model.warn.skinRigid"), src.skinnedMeshes));
        }
        if (caps.animations && !src.anims.empty() && dst.anims.empty()) {
            line(Verdict::Fail, format(tr("gdc.model.warn.animLost"), src.anims.size()));
        }
        if (caps.morphs && src.morphTargets > 0 && dst.morphTargets < src.morphTargets) {
            line(Verdict::Warn, format(tr("gdc.model.warn.morphs"), dst.morphTargets, src.morphTargets));
        }
    }

    if (dst.nanValues > src.nanValues) {
        line(Verdict::Fail, format(tr("gdc.model.warn.nan"), dst.nanValues - src.nanValues));
    }
    if (dst.degenerate > src.degenerate) {
        line(Verdict::Warn, format(tr("gdc.model.warn.flat"), dst.degenerate - src.degenerate));
    }

    const bool objectsComparable = caps.meshNames && src.tsObjects == dst.tsObjects;
    // Object names as shared ids, so the triangle match compares integers.
    std::map<std::string, int> objectIds;
    auto idsOf = [&](const Summary& s) {
        std::vector<int> ids;
        for (const ObjectInfo& o : s.objects) {
            ids.push_back(objectIds.emplace(toLower(o.name), int(objectIds.size())).first->second);
        }
        return ids;
    };
    const std::vector<int> idsA = idsOf(src), idsB = idsOf(dst);
    auto objA = [&](const Tri* t) { return t->object >= 0 && t->object < int(idsA.size()) ? idsA[size_t(t->object)] : -1; };
    auto objB = [&](const Tri* t) { return t->object >= 0 && t->object < int(idsB.size()) ? idsB[size_t(t->object)] : -1; };

    // --- per group geometry and attributes ---
    std::map<int, std::pair<std::vector<const Tri*>, std::vector<const Tri*>>> groups;
    for (const Tri& t : src.tris) {
        groups[t.group].first.push_back(&t);
    }
    for (const Tri& t : dst.tris) {
        groups[t.group].second.push_back(&t);
    }
    nlohmann::json jg = nlohmann::json::array();
    for (const auto& [group, sets] : groups) {
        const auto& A = sets.first;
        const auto& B = sets.second;
        const std::string tag = groups.size() <= 1 ? std::string(tr("gdc.model.warn.shapePrefix"))
                                 : group < 0       ? format(tr("gdc.model.warn.detailCollision"), group)
                                                   : format(tr("gdc.model.warn.detailLevel"), group);
        nlohmann::json g;
        g["group"] = group;
        g["triangles"] = A.size();
        g["resultTriangles"] = B.size();
        if (A.empty() || B.empty()) {
            if (A.empty() && B.empty()) {
                line(Verdict::Warn, format(tr("gdc.model.warn.shapeEmpty"), tag.c_str()));
            } else if (B.empty()) {
                line(Verdict::Fail, format(tr("gdc.model.warn.shapeMissing"), tag.c_str(), A.size()));
            } else {
                line(Verdict::Warn, format(tr("gdc.model.warn.shapeExtraOnly"), tag.c_str(), B.size()));
            }
            jg.push_back(g);
            continue;
        }

        const double cloudCell = diag / 256.0;
        const double uniq = std::max(posTol * 0.25, diag * 1e-9);
        const PointCloudResult ab = pointDistance(uniquePositions(A, uniq), uniquePositions(B, uniq), cloudCell);
        const PointCloudResult ba = pointDistance(uniquePositions(B, uniq), uniquePositions(A, uniq), cloudCell);
        const double maxDist = std::max(ab.dist.max, ba.dist.max);
        const AttrCost cost = [&](const Tri* a, const Tri* b, const int* map) {
            double c = 0;
            const std::string* ma = a->material >= 0 && a->material < int(src.materials.size()) ? &src.materials[a->material] : nullptr;
            const std::string* mb = b->material >= 0 && b->material < int(dst.materials.size()) ? &dst.materials[b->material] : nullptr;
            if (ma && mb && *ma != *mb) {
                c += 4.0;
            }
            // LODs and decals put identical triangles in different meshes; prefer the one in the same mesh.
            if (objectsComparable && objA(a) != objB(b)) {
                c += 8.0;
            }
            for (int i = 0; i < 3; ++i) {
                const Corner& x = a->c[i];
                const Corner& y = b->c[map[i]];
                c += std::min(1.0, double(std::fabs(x.uv0.x - y.uv0.x) + std::fabs(x.uv0.y - y.uv0.y)));
                c += 0.5 * (1.0 - std::clamp(double(dot(x.nrm, y.nrm)), -1.0, 1.0));
                c += (x.tangent.w < 0) != (y.tangent.w < 0) ? 0.25 : 0.0;
                c += 0.1 * (1.0 - std::clamp(double(dot(Vec3{x.tangent.x, x.tangent.y, x.tangent.z}, Vec3{y.tangent.x, y.tangent.y, y.tangent.z})),
                                             -1.0, 1.0));
            }
            return c;
        };
        const MatchResult match = matchTriangles(A, B, posTol, cost);
        const size_t comparable = A.size() - match.droppedDegenerate;
        const double matchedFrac = comparable ? double(match.pairs.size()) / double(comparable) : 1.0;
        g["distanceMax"] = maxDist;
        g["distanceRms"] = std::max(ab.dist.rms(), ba.dist.rms());
        g["matched"] = match.pairs.size();
        g["unmatchedSource"] = match.unmatchedA;
        g["unmatchedResult"] = match.unmatchedB;
        g["droppedDegenerate"] = match.droppedDegenerate;
        {
            Verdict v = Verdict::Pass;
            if (maxDist > posTol || match.unmatchedA || match.unmatchedB) {
                v = Verdict::Warn;
            }
            if (maxDist > posTol * 10.0 && (ab.outliers || ba.outliers || matchedFrac < 0.999)) {
                v = Verdict::Fail;
            }
            if (v == Verdict::Pass && !match.droppedDegenerate) {
                line(Verdict::Pass, format(tr("gdc.model.warn.shapeExact"), tag.c_str(), comparable));
            } else {
                std::string text = format(tr("gdc.model.warn.shape"), tag.c_str(), match.pairs.size(), comparable, match.unmatchedA,
                                          match.unmatchedB, maxDist, 100.0 * maxDist / diag);
                if (match.droppedDegenerate) {
                    text += " ";
                    text += format(tr("gdc.model.warn.shapeTiny"), match.droppedDegenerate);
                }
                text += " ";
                if (v == Verdict::Fail) {
                    text += tr("gdc.model.warn.shapeBad");
                } else if (match.unmatchedA || match.unmatchedB) {
                    text += tr("gdc.model.warn.shapeClose");
                } else if (v == Verdict::Warn) {
                    text += tr("gdc.model.warn.shapeShift");
                } else {
                    text += tr("gdc.model.warn.shapeOk");
                }
                line(v, text);
            }
        }

        size_t flipped = 0;
        for (const Pair& p : match.pairs) {
            flipped += p.flipped;
        }
        g["windingFlipped"] = flipped;
        line(flipped * 100 > match.pairs.size() ? Verdict::Fail : flipped ? Verdict::Warn : Verdict::Pass,
             format(tr(flipped ? "gdc.model.warn.windingFlip" : "gdc.model.warn.windingOk"), tag.c_str(), flipped));

        if (caps.normals && src.hasNormals && dst.hasNormals) {
            Stat ang;
            size_t nflip = 0;
            for (const Pair& p : match.pairs) {
                for (int i = 0; i < 3; ++i) {
                    const double a = angleDeg(p.a->c[i].nrm, p.b->c[p.map[i]].nrm);
                    ang.add(a);
                    nflip += a > 90.0;
                }
            }
            g["normalMaxDeg"] = ang.max;
            g["normalRmsDeg"] = ang.rms();
            g["normalsFlipped"] = nflip;
            Verdict v = ang.max > tol.normalDeg ? Verdict::Warn : Verdict::Pass;
            if (nflip * 100 > ang.count) {
                v = Verdict::Fail;
            }
            if (nflip) {
                line(v, format(tr("gdc.model.warn.normalsBack"), tag.c_str(), nflip, ang.max));
            } else if (v == Verdict::Warn) {
                line(v, format(tr("gdc.model.warn.normalsShift"), tag.c_str(), ang.max));
            } else {
                line(v, format(tr("gdc.model.warn.normalsOk"), tag.c_str()));
            }

            const double q = std::max(posTol * 4.0, diag * 1e-7);
            const auto creaseA = edgeCreases(A, q);
            const auto creaseB = edgeCreases(B, q);
            // An edge only counts as lost or new when it moves clearly past the crease, not when
            // normal error nudges an edge that sits right at it.
            const double margin = 2.0 * tol.normalDeg;
            size_t hard = 0, lost = 0, added = 0;
            for (const auto& [e, angle] : creaseA) {
                if (angle <= tol.creaseDeg) {
                    continue;
                }
                ++hard;
                auto it = creaseB.find(e);
                lost += it != creaseB.end() && it->second < tol.creaseDeg - margin;
            }
            for (const auto& [e, angle] : creaseB) {
                auto it = creaseA.find(e);
                added += angle > tol.creaseDeg && it != creaseA.end() && it->second < tol.creaseDeg - margin;
            }
            const size_t hardCount = hard;
            g["hardEdges"] = hardCount;
            g["hardEdgesLost"] = lost;
            g["hardEdgesNew"] = added;
            line(lost || added ? Verdict::Warn : Verdict::Pass,
                 format(tr(lost || added ? "gdc.model.warn.edgesChanged" : "gdc.model.warn.edgesOk"), tag.c_str(), hardCount, lost, added));
        } else if (src.hasNormals && !dst.hasNormals) {
            line(Verdict::Warn, format(tr("gdc.model.warn.normalsMissing"), tag.c_str()));
        }

        auto uvCheck = [&](const char* name, bool second) {
            double uvTol = tol.uv;
            if (tol.uvRangeRelative) {
                float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
                for (const Tri* t : A) {
                    for (const Corner& c : t->c) {
                        const Vec2& uv = second ? c.uv1 : c.uv0;
                        lo[0] = std::min(lo[0], uv.x), hi[0] = std::max(hi[0], uv.x);
                        lo[1] = std::min(lo[1], uv.y), hi[1] = std::max(hi[1], uv.y);
                    }
                }
                uvTol *= std::max(1.0, double(std::max(hi[0] - lo[0], hi[1] - lo[1])));
            }
            Stat d;
            size_t vflip = 0;
            for (const Pair& p : match.pairs) {
                for (int i = 0; i < 3; ++i) {
                    const Vec2& a = second ? p.a->c[i].uv1 : p.a->c[i].uv0;
                    const Vec2& b = second ? p.b->c[p.map[i]].uv1 : p.b->c[p.map[i]].uv0;
                    const double diff = std::max(std::fabs(a.x - b.x), std::fabs(a.y - b.y));
                    d.add(diff);
                    if (diff > uvTol && std::fabs(a.x - b.x) <= uvTol && std::fabs(a.y - (1.0f - b.y)) <= uvTol) {
                        ++vflip;
                    }
                }
            }
            g[std::string(name) + "Max"] = d.max;
            g[std::string(name) + "VFlipped"] = vflip;
            const bool bad = d.max > uvTol || vflip;
            const char* id = "gdc.model.warn.uv0Ok";
            if (second) {
                id = !bad ? "gdc.model.warn.uv1Ok" : vflip ? "gdc.model.warn.uv1Both" : "gdc.model.warn.uv1Moved";
            } else if (bad) {
                id = vflip ? "gdc.model.warn.uv0Both" : "gdc.model.warn.uv0Moved";
            }
            if (!bad) {
                line(Verdict::Pass, format(tr(id), tag.c_str()));
            } else if (vflip) {
                line(Verdict::Warn, format(tr(id), tag.c_str(), d.max, vflip));
            } else {
                line(Verdict::Warn, format(tr(id), tag.c_str(), d.max));
            }
        };
        if (caps.uv && src.hasUv0) {
            if (dst.hasUv0) {
                uvCheck("uv0", false);
            } else {
                line(Verdict::Warn, format(tr("gdc.model.warn.uv0Lost"), tag.c_str()));
            }
        }
        if (caps.uv2 && src.hasUv1) {
            if (dst.hasUv1) {
                uvCheck("uv1", true);
            } else {
                line(Verdict::Warn, format(tr("gdc.model.warn.uv1Lost"), tag.c_str()));
            }
        }

        if (caps.colors && src.hasColors) {
            if (!dst.hasColors) {
                line(Verdict::Warn, format(tr("gdc.model.warn.colorLost"), tag.c_str()));
            } else {
                int maxd = 0;
                for (const Pair& p : match.pairs) {
                    for (int i = 0; i < 3; ++i) {
                        const Color8& a = p.a->c[i].color;
                        const Color8& b = p.b->c[p.map[i]].color;
                        maxd = std::max({maxd, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
                    }
                }
                g["colorMax"] = maxd;
                if (maxd > tol.color) {
                    line(Verdict::Warn, format(tr(maxd >= 64 ? "gdc.model.warn.colorFar" : "gdc.model.warn.colorDiff"), tag.c_str(), maxd));
                } else {
                    line(Verdict::Pass, format(tr("gdc.model.warn.colorOk"), tag.c_str()));
                }
            }
        }

        if (caps.tangents && src.hasTangents && dst.hasTangents) {
            Stat ang;
            size_t hand = 0;
            for (const Pair& p : match.pairs) {
                for (int i = 0; i < 3; ++i) {
                    const Vec4& a = p.a->c[i].tangent;
                    const Vec4& b = p.b->c[p.map[i]].tangent;
                    ang.add(angleDeg({a.x, a.y, a.z}, {b.x, b.y, b.z}));
                    // Handedness is undefined when the tangent runs along the normal.
                    const Vec3 n = normalize(p.a->c[i].nrm);
                    if (length(cross(n, normalize(Vec3{a.x, a.y, a.z}))) > 0.05f) {
                        hand += (a.w < 0) != (b.w < 0);
                    }
                }
            }
            g["tangentMaxDeg"] = ang.max;
            g["handednessMismatch"] = hand;
            const bool angle = ang.max > tol.tangentDeg;
            const char* id = "gdc.model.warn.tangentOk";
            if (angle && hand) {
                id = "gdc.model.warn.tangentBoth";
            } else if (hand) {
                id = "gdc.model.warn.tangentMirror";
            } else if (angle) {
                id = "gdc.model.warn.tangentAngle";
            }
            if (angle && hand) {
                line(Verdict::Warn, format(tr(id), tag.c_str(), ang.max, hand));
            } else if (hand) {
                line(Verdict::Warn, format(tr(id), tag.c_str(), hand));
            } else if (angle) {
                line(Verdict::Warn, format(tr(id), tag.c_str(), ang.max));
            } else {
                line(Verdict::Pass, format(tr(id), tag.c_str()));
            }
        }

        if (caps.materials) {
            size_t changed = 0;
            for (const Pair& p : match.pairs) {
                const std::string a = p.a->material >= 0 && p.a->material < int(src.materials.size()) ? src.materials[p.a->material] : "";
                const std::string b = p.b->material >= 0 && p.b->material < int(dst.materials.size()) ? dst.materials[p.b->material] : "";
                changed += a != b && !a.empty();
            }
            g["materialChanged"] = changed;
            line(changed ? Verdict::Fail : Verdict::Pass,
                 format(tr(changed ? "gdc.model.warn.materialChanged" : "gdc.model.warn.materialOk"), tag.c_str(), changed));

            // Triangles per material, below-tolerance slivers left out (encoders may drop them).
            std::map<std::string, std::pair<size_t, size_t>> counts;
            for (const Tri* t : A) {
                if (!isDegenerate(t, posTol * 4.0)) {
                    ++counts[t->material >= 0 && t->material < int(src.materials.size()) ? src.materials[t->material] : ""].first;
                }
            }
            for (const Tri* t : B) {
                if (!isDegenerate(t, posTol * 4.0)) {
                    ++counts[t->material >= 0 && t->material < int(dst.materials.size()) ? dst.materials[t->material] : ""].second;
                }
            }
            std::vector<std::string> differ;
            nlohmann::json jc = nlohmann::json::object();
            for (const auto& [name, n] : counts) {
                jc[name] = {n.first, n.second};
                if (n.first != n.second && !(placeholderMaterial(name) && !n.first) && !(name.empty() && !n.second)) {
                    differ.push_back(format("%s %zu -> %zu", name.empty() ? "-" : name.c_str(), n.first, n.second));
                }
            }
            g["materialCounts"] = jc;
            if (!differ.empty()) {
                line(Verdict::Fail, format(tr("gdc.model.warn.matCounts"), tag.c_str(), differ.size(), listNames(differ, differ.size()).c_str()));
            }
        }

        if (objectsComparable) {
            size_t changed = 0;
            for (const Pair& p : match.pairs) {
                const int a = objA(p.a), b = objB(p.b);
                changed += a >= 0 && b >= 0 && a != b;
            }
            g["objectChanged"] = changed;
            line(changed ? Verdict::Fail : Verdict::Pass,
                 format(tr(changed ? "gdc.model.warn.objectChanged" : "gdc.model.warn.objectOk"), tag.c_str(), changed));
        }

        if (caps.skins && src.skinnedMeshes && dst.skinnedMeshes) {
            double maxW = 0;
            size_t missing = 0, skinned = 0;
            std::unordered_set<int> seen;
            for (const Pair& p : match.pairs) {
                for (int i = 0; i < 3; ++i) {
                    const int sa = p.a->c[i].skin, sb = p.b->c[p.map[i]].skin;
                    if (sa < 0 || !seen.insert(sa).second) {
                        continue;
                    }
                    ++skinned;
                    std::map<std::string, float> wa, wb;
                    for (const Influence& in : src.influences[sa]) {
                        wa[toLower(in.bone)] += in.weight;
                    }
                    if (sb >= 0) {
                        for (const Influence& in : dst.influences[sb]) {
                            wb[toLower(in.bone)] += in.weight;
                        }
                    }
                    for (const auto& [bone, w] : wa) {
                        auto it = wb.find(bone);
                        if (it == wb.end()) {
                            missing += w > tol.weight;
                            maxW = std::max(maxW, double(w));
                        } else {
                            maxW = std::max(maxW, std::fabs(double(w) - it->second));
                        }
                    }
                }
            }
            g["skinnedVertices"] = skinned;
            g["weightMaxDiff"] = maxW;
            g["missingInfluences"] = missing;
            if (missing) {
                line(Verdict::Warn, format(tr("gdc.model.warn.skinMissing"), tag.c_str(), skinned, maxW, missing));
            } else if (maxW > tol.weight) {
                line(Verdict::Warn, format(tr("gdc.model.warn.skinDiff"), tag.c_str(), skinned, maxW));
            } else {
                line(Verdict::Pass, format(tr("gdc.model.warn.skinOk"), tag.c_str(), skinned));
            }
        }
        jg.push_back(g);
    }
    j["groups"] = jg;

    // --- animation ---
    if (caps.animations) {
        nlohmann::json ja = nlohmann::json::array();
        for (const AnimSample& a : src.anims) {
            const AnimSample* b = nullptr;
            for (const AnimSample& c : dst.anims) {
                if (toLower(c.name) == toLower(a.name)) {
                    b = &c;
                }
            }
            if (!b && src.anims.size() == 1 && dst.anims.size() == 1) {
                b = &dst.anims.front();
            }
            if (!b) {
                line(Verdict::Warn, format(tr("gdc.model.warn.animMissing"), a.name.c_str()));
                continue;
            }
            std::unordered_map<std::string, size_t> idx;
            for (size_t i = 0; i < b->nodes.size(); ++i) {
                idx[toLower(b->nodes[i])] = i;
            }
            double maxRot = 0, maxTrans = 0;
            size_t missingNodes = 0;
            for (size_t i = 0; i < a.nodes.size(); ++i) {
                auto it = idx.find(toLower(a.nodes[i]));
                if (it == idx.end()) {
                    ++missingNodes;
                    continue;
                }
                const auto& ra = a.rigid[i];
                const auto& rb = b->rigid[it->second];
                for (size_t k = 0; k < ra.size() && k < rb.size(); ++k) {
                    maxRot = std::max(maxRot, rotDiffDeg(ra[k], rb[k]));
                    maxTrans = std::max(maxTrans, double(length(translationOf(ra[k]) - translationOf(rb[k]))));
                }
            }
            const double durDiff = std::fabs(double(a.duration) - b->duration);
            ja.push_back({{"name", a.name},
                          {"nodes", a.nodes.size()},
                          {"missingNodes", missingNodes},
                          {"maxRotDeg", maxRot},
                          {"maxTrans", maxTrans},
                          {"duration", a.duration},
                          {"resultDuration", b->duration}});
            const bool bad = maxRot > tol.rotDeg || maxTrans > tol.transRel * diag || missingNodes || durDiff > 1e-3;
            if (bad) {
                line(Verdict::Warn, format(tr("gdc.model.warn.animBad"), a.name.c_str(), missingNodes, a.nodes.size(), maxRot, maxTrans,
                                           a.duration, b->duration));
            } else {
                line(Verdict::Pass, format(tr("gdc.model.warn.animOk"), a.name.c_str(), a.nodes.size(), a.duration));
            }
        }
        j["animations"] = ja;
    }
    j["verdict"] = verdictName(rep.verdict);
    return rep;
}

} // namespace model
