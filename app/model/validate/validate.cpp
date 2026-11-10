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

#include "core/strings.h"

namespace model {
namespace {

using namespace ts;

constexpr double kPi = 3.14159265358979323846;

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
    if (!raw.ok) {
        line(Verdict::Warn, "up axis: could not re-read the file to check it: " + raw.error);
        j["checked"] = false;
        rep.json["verdict"] = verdictName(rep.verdict);
        return;
    }
    Verdict v = Verdict::Pass;
    std::string text = format("up axis: %s-up", upAxisName(expected));
    if (raw.declared) {
        const bool match = *raw.declared == expected && !raw.declaredNegative;
        text += format(", file declares %s%s up", raw.declaredNegative ? "-" : "", upAxisName(*raw.declared));
        j["declared"] = std::string(raw.declaredNegative ? "-" : "") + upAxisName(*raw.declared);
        if (!match) {
            v = Verdict::Fail;
            text += " (mismatch)";
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
            text += ", stored extents lie inside the game model (geometry is incomplete, see above)";
            j["incomplete"] = true;
        } else if (worst > tol) {
            v = Verdict::Fail;
            text += format(", stored extents %.3g x %.3g x %.3g but %.3g x %.3g x %.3g expected",
                           raw.bounds.max.x - raw.bounds.min.x, raw.bounds.max.y - raw.bounds.min.y,
                           raw.bounds.max.z - raw.bounds.min.z, wantMax.x - wantMin.x, wantMax.y - wantMin.y,
                           wantMax.z - wantMin.z);
        } else {
            text += ", stored extents match the game model";
        }
    }
    line(v, text);
    j["verdict"] = verdictName(v);
    rep.json["verdict"] = verdictName(rep.verdict);
}

namespace {

bool lodAnchor(const aiNode* node)
{
    if (!node) {
        return false;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (isBaseStart(node->mChildren[i])) {
            return true;
        }
    }
    return false;
}

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

void collectMeshNodes(const aiNode* node, std::set<std::string>& out)
{
    if (!node) {
        return;
    }
    if (node->mNumMeshes) {
        out.insert(toLower(node->mName.C_Str()));
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        collectMeshNodes(node->mChildren[i], out);
    }
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
        report.lines.push_back("WARN LOD: the format keeps no node names, so the game sees no detail levels or collision meshes");
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
        report.lines.push_back(format(
            "FAIL LOD: game does not see base/start under '%s'. Detail names fall back to size 2, so nulldetail hides the meshes.",
            seen[0] ? seen : "(none)"));
    } else {
        report.lines.push_back("LOD: base/start is where the game looks for detail levels");
    }

    std::set<std::string> want, got;
    collectMeshNodes(source.mRootNode, want);
    collectMeshNodes(written.mRootNode, got);
    std::vector<std::string> missing;
    for (const std::string& name : want) {
        if (!got.count(name) && missing.size() < 4) {
            missing.push_back(name);
        }
    }
    const size_t missingCount = want.size() - std::count_if(want.begin(), want.end(), [&](const std::string& n) { return got.count(n) != 0; });
    report.json["lod"]["meshNodes"] = want.size();
    report.json["lod"]["meshNodesFound"] = want.size() - missingCount;
    if (missingCount) {
        raise(Verdict::Fail);
        std::string text = format("FAIL LOD: %zu mesh node name(s) lost", missingCount);
        for (const std::string& m : missing) {
            text += " " + m;
        }
        if (missingCount > missing.size()) {
            text += " ...";
        }
        report.lines.push_back(text);
    }
    report.json["verdict"] = verdictName(report.verdict);
}

ValidationReport compareSummaries(const Summary& src, const Summary& dst, const Capabilities& caps, const Tolerance& tol)
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
        std::set<std::string> dstNodes;
        for (const NodePose& n : dst.nodes) {
            dstNodes.insert(toLower(n.name));
        }
        for (const std::string& n : dst.meshHolders) {
            dstNodes.insert(toLower(n));
        }
        size_t found = 0;
        for (const NodePose& n : src.nodes) {
            found += dstNodes.count(toLower(n.name));
        }
        std::set<std::string> dstMats(dst.materials.begin(), dst.materials.end());
        std::set<std::string> usedSrcMats;
        for (const Tri& t : src.tris) {
            if (t.material >= 0 && t.material < int(src.materials.size())) {
                usedSrcMats.insert(src.materials[t.material]);
            }
        }
        size_t matsFound = 0;
        for (const std::string& m : usedSrcMats) {
            matsFound += dstMats.count(m);
        }
        j["structure"] = {{"nodes", src.nodes.size()},           {"nodesFound", found},
                          {"materials", usedSrcMats.size()},     {"materialsFound", matsFound},
                          {"details", src.details.size()},       {"resultDetails", dst.details.size()},
                          {"skinnedMeshes", src.skinnedMeshes},  {"resultSkinnedMeshes", dst.skinnedMeshes},
                          {"animations", src.anims.size()},      {"resultAnimations", dst.anims.size()}};
        Verdict v = Verdict::Pass;
        if (caps.hierarchy && found < src.nodes.size()) {
            v = Verdict::Warn;
        }
        if (caps.materials && matsFound < usedSrcMats.size()) {
            v = Verdict::Warn;
        }
        if (caps.lods && !src.details.empty() && src.details.size() != dst.details.size()) {
            v = Verdict::Warn;
        }
        std::string text = format("structure: nodes %zu/%zu, materials %zu/%zu", found, src.nodes.size(), matsFound, usedSrcMats.size());
        if (caps.lods && !src.details.empty()) {
            text += format(", details %zu/%zu", dst.details.size(), src.details.size());
        }
        if (caps.hierarchy && found < src.nodes.size()) {
            std::vector<std::string> missing;
            for (const NodePose& n : src.nodes) {
                if (!dstNodes.count(toLower(n.name)) && missing.size() < 4) {
                    missing.push_back(n.name);
                }
            }
            text += " (missing";
            for (const std::string& m : missing) {
                text += " " + m;
            }
            text += found + missing.size() < src.nodes.size() ? " ...)" : ")";
        }
        if (caps.materials && matsFound < usedSrcMats.size()) {
            text += " (missing materials";
            size_t listed = 0;
            for (const std::string& m : usedSrcMats) {
                if (!dstMats.count(m) && listed++ < 4) {
                    text += " " + m;
                }
            }
            text += ")";
        }
        line(v, text);
        if (caps.skins && src.skinnedMeshes > 0 && dst.skinnedMeshes == 0) {
            line(Verdict::Fail, format("skin: %d skinned mesh(es) became rigid", src.skinnedMeshes));
        }
        if (caps.animations && !src.anims.empty() && dst.anims.empty()) {
            line(Verdict::Fail, format("animation: %zu animation(s) lost", src.anims.size()));
        }
        if (caps.morphs && src.morphTargets > 0 && dst.morphTargets < src.morphTargets) {
            line(Verdict::Warn, format("morphs: %d of %d targets kept", dst.morphTargets, src.morphTargets));
        }
    }

    if (dst.nanValues > src.nanValues) {
        line(Verdict::Fail, format("NaN/Inf: %zu new non-finite values", dst.nanValues - src.nanValues));
    }
    if (dst.degenerate > src.degenerate) {
        line(Verdict::Warn, format("degenerate: %zu new zero-area triangles", dst.degenerate - src.degenerate));
    }

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
        const std::string tag = groups.size() > 1 ? format("[detail %d] ", group) : std::string();
        nlohmann::json g;
        g["group"] = group;
        g["triangles"] = A.size();
        g["resultTriangles"] = B.size();
        if (A.empty() || B.empty()) {
            line(A.empty() ? Verdict::Warn : Verdict::Fail,
                 tag + format("geometry: %zu source triangles, %zu result triangles", A.size(), B.size()));
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
            std::string text = format("geometry: %zu/%zu triangles matched (%zu extra", match.pairs.size(), comparable, match.unmatchedB);
            if (match.droppedDegenerate) {
                text += format(", %zu below tolerance skipped", match.droppedDegenerate);
            }
            line(v, tag + text + format("), max %.3g (%.2g of bbox), rms %.3g", maxDist, maxDist / diag, std::max(ab.dist.rms(), ba.dist.rms())));
        }

        size_t flipped = 0;
        for (const Pair& p : match.pairs) {
            flipped += p.flipped;
        }
        g["windingFlipped"] = flipped;
        line(flipped * 100 > match.pairs.size() ? Verdict::Fail : flipped ? Verdict::Warn : Verdict::Pass,
             tag + format("winding: %zu flipped", flipped));

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
            line(v, tag + format("normals: max %.2f deg, rms %.3f deg, %zu flipped", ang.max, ang.rms(), nflip));

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
                 tag + format("sharp edges: %zu hard, %zu lost, %zu new", hardCount, lost, added));
        } else if (src.hasNormals && !dst.hasNormals) {
            line(Verdict::Warn, tag + "normals: not stored by the target");
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
            line(d.max > uvTol || vflip ? Verdict::Warn : Verdict::Pass,
                 tag + format("%s: max %.3g, rms %.3g, %zu V-flipped", name, d.max, d.rms(), vflip));
        };
        if (caps.uv && src.hasUv0) {
            if (dst.hasUv0) {
                uvCheck("uv0", false);
            } else {
                line(Verdict::Warn, tag + "uv0: lost");
            }
        }
        if (caps.uv2 && src.hasUv1) {
            if (dst.hasUv1) {
                uvCheck("uv1", true);
            } else {
                line(Verdict::Warn, tag + "uv1: lost");
            }
        }

        if (caps.colors && src.hasColors) {
            if (!dst.hasColors) {
                line(Verdict::Warn, tag + "colors: lost");
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
                line(maxd > tol.color ? Verdict::Warn : Verdict::Pass, tag + format("colors: max %d/255", maxd));
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
            line(ang.max > tol.tangentDeg || hand ? Verdict::Warn : Verdict::Pass,
                 tag + format("tangents: max %.2f deg, %zu handedness mismatched", ang.max, hand));
        }

        if (caps.materials) {
            size_t changed = 0;
            for (const Pair& p : match.pairs) {
                const std::string a = p.a->material >= 0 && p.a->material < int(src.materials.size()) ? src.materials[p.a->material] : "";
                const std::string b = p.b->material >= 0 && p.b->material < int(dst.materials.size()) ? dst.materials[p.b->material] : "";
                changed += a != b && !a.empty();
            }
            g["materialChanged"] = changed;
            line(changed ? Verdict::Warn : Verdict::Pass, tag + format("materials: %zu triangle(s) changed material", changed));
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
            line(maxW > tol.weight || missing ? Verdict::Warn : Verdict::Pass,
                 tag + format("skin: %zu skinned vertices, max weight diff %.4f, %zu missing influences", skinned, maxW, missing));
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
                line(Verdict::Warn, format("animation '%s': missing", a.name.c_str()));
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
            line(bad ? Verdict::Warn : Verdict::Pass,
                 format("animation '%s': %zu moving nodes (%zu missing), max rot %.3f deg, max trans %.3g, duration %.3f/%.3f s",
                        a.name.c_str(), a.nodes.size(), missingNodes, maxRot, maxTrans, a.duration, b->duration));
        }
        j["animations"] = ja;
    }
    j["verdict"] = verdictName(rep.verdict);
    return rep;
}

} // namespace model
