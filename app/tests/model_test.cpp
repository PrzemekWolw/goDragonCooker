// gdc_model_test: round-trips DTS/CDAE files and prints a per-file pass/fail line. With --convert,
// converts every readable model to the listed targets through the converter and validates it.
// Usage: gdc_model_test [--verbose] [--allow-reject <filename>]... [--convert glb,dae,... --out <dir> [--back]
//        [--report <file.json>]] [--dump] <file-or-dir>...
// Targets: glb, gltf, glb-draco, dae, fbx, obj, cdae, dts (v29), dts24..dts29, any writable format id, or all.
#if defined(_DEBUG) && defined(_MSC_VER)
#include <crtdbg.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/platform.h"
#include "core/strings.h"
#include "model/axis.h"
#include "model/convert.h"
#include "model/validate/validate.h"
#include "model/validate/summary.h"
#include "model/ts/ts_io.h"

namespace {

bool verbose = false;
int failures = 0;
std::vector<std::string> allowReject;

template <class T>
bool sameBytes(const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

// Exact structural comparison of everything both formats preserve. legacy = DTS v24/25.
std::string compareShapes(const ts::Shape& a, const ts::Shape& b, bool legacy)
{
    const bool checkUv2 = !legacy;
#define CMP(field)                                     \
    if (!sameBytes(a.field, b.field)) {                \
        return #field;                                 \
    }
    CMP(nodes) CMP(objects) CMP(subShapeFirstNode) CMP(subShapeFirstObject) CMP(subShapeNumNodes)
    CMP(subShapeNumObjects) CMP(defaultRotations) CMP(defaultTranslations) CMP(nodeRotations)
    CMP(nodeTranslations) CMP(nodeUniformScales) CMP(nodeAlignedScales) CMP(nodeArbitraryScaleFactors)
    CMP(nodeArbitraryScaleRots) CMP(groundTranslations) CMP(groundRotations) CMP(objectStates) CMP(triggers)
#undef CMP
    if (a.names != b.names) {
        return "names";
    }
    if (a.details.size() != b.details.size()) {
        return "details";
    }
    for (size_t i = 0; i < a.details.size(); ++i) {
        const ts::Detail& x = a.details[i];
        const ts::Detail& y = b.details[i];
        if (x.nameIndex != y.nameIndex || x.size != y.size || x.subShapeNum != y.subShapeNum) {
            return format("details[%zu]", i);
        }
        // Billboard details pack their settings into objectDetailNum before v26.
        const bool same = x.subShapeNum >= 0 ? x.objectDetailNum == y.objectDetailNum
                                             : x.bbEquatorSteps == y.bbEquatorSteps && x.bbPolarSteps == y.bbPolarSteps &&
                                                   x.bbDetailLevel == y.bbDetailLevel &&
                                                   (legacy ? std::min(x.bbDimension, 255) : x.bbDimension) == y.bbDimension &&
                                                   x.bbIncludePoles == y.bbIncludePoles;
        if (!same) {
            return format("details[%zu] billboard eq %d/%d polar %d/%d level %d/%d dim %d/%d poles %d/%d", i,
                          x.bbEquatorSteps, y.bbEquatorSteps, x.bbPolarSteps, y.bbPolarSteps, x.bbDetailLevel, y.bbDetailLevel,
                          x.bbDimension, y.bbDimension, int(x.bbIncludePoles), int(y.bbIncludePoles));
        }
    }
    if (a.meshes.size() != b.meshes.size()) {
        return format("mesh count %zu vs %zu", a.meshes.size(), b.meshes.size());
    }
    for (size_t i = 0; i < a.meshes.size(); ++i) {
        const ts::Mesh& x = a.meshes[i];
        const ts::Mesh& y = b.meshes[i];
        const ts::MeshType tx = x.type == ts::MeshType::Decal ? ts::MeshType::Null : x.type;
        if (tx != y.type) {
            return format("mesh[%zu].type", i);
        }
        if (y.isNull()) {
            continue;
        }
#define MCMP(field)                                    \
    if (!sameBytes(x.field, y.field)) {                \
        return format("mesh[%zu]." #field, i);         \
    }
        MCMP(verts) MCMP(tverts) MCMP(primitives) MCMP(indices) MCMP(initialVerts) MCMP(initialTransforms)
        MCMP(vertexIndex) MCMP(boneIndex) MCMP(weight) MCMP(nodeIndex)
#undef MCMP
        if (checkUv2 && (!sameBytes(x.tverts2, y.tverts2) || !sameBytes(x.colors, y.colors))) {
            return format("mesh[%zu].tverts2/colors", i);
        }
        if (x.numFrames != y.numFrames || x.vertsPerFrame != y.vertsPerFrame) {
            return format("mesh[%zu] frames", i);
        }
    }
    if (a.sequences.size() != b.sequences.size()) {
        return "sequence count";
    }
    for (size_t i = 0; i < a.sequences.size(); ++i) {
        const ts::Sequence& x = a.sequences[i];
        const ts::Sequence& y = b.sequences[i];
        if (x.nameIndex != y.nameIndex || x.numKeyframes != y.numKeyframes || x.duration != y.duration ||
            x.baseRotation != y.baseRotation || x.baseTranslation != y.baseTranslation || x.flags != y.flags ||
            x.rotationMatters.count() != y.rotationMatters.count() || x.translationMatters.count() != y.translationMatters.count()) {
            return format("sequence[%zu]", i);
        }
    }
    if (a.materials.size() != b.materials.size()) {
        return "material count";
    }
    for (size_t i = 0; i < a.materials.size(); ++i) {
        if (a.materials[i].name != b.materials[i].name || a.materials[i].flags != b.materials[i].flags) {
            return format("material[%zu]", i);
        }
    }
    return {};
}

void report(const std::string& file, const std::string& step, const std::string& problem)
{
    if (problem.empty()) {
        if (verbose) {
            std::printf("  ok   %s\n", step.c_str());
        }
        return;
    }
    ++failures;
    std::printf("  FAIL %s: %s (%s)\n", step.c_str(), problem.c_str(), file.c_str());
}

void testShapeFile(const std::string& path)
{
    ts::Shape shape;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<uint8_t> data;
    if (!ts::readFileBytes(path, data, error)) {
        report(path, "read", error);
        return;
    }
    int exporter = 0;
    const int version = ts::peekVersion(data, &exporter);
    if (!ts::readShape(data, shape, error, &warnings)) {
        const std::string name = fs::u8path(path).filename().u8string();
        for (const std::string& r : allowReject) {
            if (r == name) {
                std::printf("%s: rejected as expected (%s)\n", name.c_str(), error.c_str());
                return;
            }
        }
        report(path, format("read v%d", version), error);
        return;
    }
    size_t meshCount = 0, skinCount = 0;
    for (const ts::Mesh& m : shape.meshes) {
        meshCount += !m.isNull();
        skinCount += m.isSkin();
    }
    std::printf("%s: v%d exporter %d, %zu nodes, %zu objects, %zu meshes (%zu skin), %zu sequences, %zu materials\n",
                fs::u8path(path).filename().u8string().c_str(), version, exporter, shape.nodes.size(), shape.objects.size(),
                meshCount, skinCount, shape.sequences.size(), shape.materials.size());
    for (const std::string& w : warnings) {
        std::printf("  warn %s\n", w.c_str());
    }

    for (ts::CdaeCompression c : {ts::CdaeCompression::Off, ts::CdaeCompression::On}) {
        const char* label = c == ts::CdaeCompression::On ? "cdae v31 zstd" : "cdae v31 raw";
        std::vector<uint8_t> out;
        ts::CdaeWriteOptions opt;
        opt.compression = c;
        ts::Shape back;
        if (!ts::writeCdae(shape, opt, out, error)) {
            report(path, label, "write: " + error);
        } else if (!ts::readShape(out, back, error)) {
            report(path, label, "reread: " + error);
        } else {
            report(path, label, compareShapes(shape, back, false));
        }
    }
    for (int v = ts::kMinDtsWriteVersion; v <= ts::kMaxDtsWriteVersion; ++v) {
        const std::string label = format("dts v%d", v);
        std::vector<uint8_t> out;
        std::vector<std::string> writeWarnings;
        ts::Shape back;
        if (!ts::writeDts(shape, v, out, error, &writeWarnings)) {
            if (v <= 25 && error.find("v26") != std::string::npos) {
                if (verbose) {
                    std::printf("  skip %s: %s\n", label.c_str(), error.c_str());
                }
                continue;
            }
            report(path, label, "write: " + error);
        } else if (!ts::readShape(out, back, error)) {
            report(path, label, "reread: " + error);
        } else {
            report(path, label, compareShapes(shape, back, v <= 25));
        }
    }
}

struct Target {
    std::string tag;
    std::string formatId;
    nlohmann::json settings;
};
std::vector<Target> targets;
bool convertBack = false;
std::string outDir;
nlohmann::json jsonReport = nlohmann::json::array();

bool parseTargets(const std::string& list)
{
    const std::map<std::string, Target> known = {
        {"glb", {"glb", "gltf2", {}}},
        {"gltf", {"gltf", "gltf2", {{"container", 1}}}},
        {"glb-draco", {"glb-draco", "gltf2", {{"draco", 1}}}},
        {"dae", {"dae", "collada", {}}},
        {"fbx", {"fbx", "fbx", {}}},
        {"obj", {"obj", "obj", {}}},
        {"cdae", {"cdae", "cdae", {}}},
        {"dts", {"dts", "dts", {}}},
        {"dts24", {"dts24", "dts", {{"version", ts::kMaxDtsWriteVersion - 24}}}},
    };
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = std::min(list.find(',', start), list.size());
        const std::string tag = list.substr(start, comma - start);
        if (tag == "all") {
            for (const model::Format* f : model::writableFormats()) {
                targets.push_back({f->id, f->id, {}});
            }
            targets.push_back(known.at("glb-draco"));
            start = comma + 1;
            continue;
        }
        if (known.find(tag) == known.end() && model::formatById(tag) && model::formatById(tag)->canWrite) {
            targets.push_back({tag, tag, {}});
            start = comma + 1;
            continue;
        }
        auto it = known.find(tag);
        if (it == known.end() && tag.size() == 5 && hasPrefix(tag, "dts")) {
            const int version = std::atoi(tag.c_str() + 3);
            if (version >= ts::kMinDtsWriteVersion && version <= ts::kMaxDtsWriteVersion) {
                targets.push_back({tag, "dts", {{"version", ts::kMaxDtsWriteVersion - version}}});
                start = comma + 1;
                continue;
            }
        }
        if (it == known.end()) {
            std::fprintf(stderr, "unknown target '%s'\n", tag.c_str());
            return false;
        }
        targets.push_back(it->second);
        start = comma + 1;
    }
    return true;
}

// Converts and validates; returns the output path ("" on failure).
std::string convertOne(model::Model& source, const std::string& name, const Target& t, const std::string& suffix)
{
    const model::Format* target = model::formatById(t.formatId);
    if (!target) {
        report(name, t.tag, "format not available");
        return {};
    }
    const std::string stem = fs::u8path(name).filename().u8string();
    const std::string out = (fs::u8path(outDir) / fs::u8path(stem + suffix + "." + t.tag + target->outputExt(model::withDefaults(*target, t.settings)))).u8string();
    model::ConvertOptions o;
    o.target = target;
    o.settings = t.settings;
    o.validate = true;
    const model::ConvertResult r = model::convertModel(source, out, o);
    const std::string label = format("%s%s -> %s", stem.c_str(), suffix.c_str(), t.tag.c_str());
    if (!r.ok) {
        report(name, label, r.error);
        return {};
    }
    std::printf("  %s: %s\n", label.c_str(), model::verdictName(r.report.verdict));
    for (const std::string& line : r.log) {
        if (verbose) {
            std::printf("      %s\n", line.c_str());
        }
    }
    for (const std::string& w : r.warnings) {
        if (verbose) {
            std::printf("      warn %s\n", w.c_str());
        }
    }
    for (const std::string& line : r.report.lines) {
        if (verbose || line.rfind("WARN", 0) == 0 || line.rfind("FAIL", 0) == 0) {
            std::printf("      %s\n", line.c_str());
        }
    }
    nlohmann::json entry = r.report.json;
    entry["source"] = name;
    entry["output"] = out;
    jsonReport.push_back(entry);
    if (r.report.verdict == model::Verdict::Fail) {
        ++failures;
    }
    return out;
}

void testConvert(const std::string& path)
{
    model::Model source;
    std::string error;
    std::vector<std::string> warnings;
    std::printf("loading %s\n", path.c_str());
    if (!source.load(path, error, warnings)) {
        report(path, "load", error);
        return;
    }
    std::printf("%s\n", fs::u8path(path).filename().u8string().c_str());
    for (const Target& t : targets) {
        const std::string out = convertOne(source, path, t, "");
        if (out.empty() || !convertBack) {
            continue;
        }
        const model::Format* f = model::formatById(t.formatId);
        model::Model again;
        if (!again.load(out, error, warnings)) {
            report(out, "reload", error);
            continue;
        }
        convertOne(again, out, f->isTs ? Target{"glb", "gltf2", {}} : Target{"cdae", "cdae", {}}, ".back");
    }
}

bool dumpOnly = false;
std::string axisRef;         // --axis-ref: game-space reference model for --axis
model::UpAxis axisExpected = model::UpAxis::Y;
bool axisOnly = false;

// Reads `path` without any up-axis conversion and checks its extents against the reference model carried into
// the expected frame.
void axisFile(const std::string& path)
{
    model::Model ref;
    std::string error;
    std::vector<std::string> warnings, ignored;
    if (!ref.load(axisRef, error, warnings)) {
        std::printf("%s: %s\n", axisRef.c_str(), error.c_str());
        ++failures;
        return;
    }
    model::Summary s;
    if (ref.isTs()) {
        std::string e;
        s = model::summarize(*ref.shape({}, ignored, e), model::DetailFilter{});
    } else {
        s = model::summarize(*ref.scene({}, ignored));
    }
    model::ValidationReport rep;
    model::checkUpAxis(rep, axisExpected, model::readRawFrame(path), s);
    std::printf("%s (%s): %s\n", path.c_str(), rep.verdict == model::Verdict::Fail ? "FAIL" : "ok", rep.lines.empty() ? "" : rep.lines[0].c_str());
    if (rep.verdict == model::Verdict::Fail) {
        ++failures;
    }
}

// Per-material triangle counts of the model as the validator sees it.
void dumpFile(const std::string& path)
{
    model::Model m;
    std::string error;
    std::vector<std::string> warnings;
    if (!m.load(path, error, warnings)) {
        std::printf("%s: %s\n", path.c_str(), error.c_str());
        return;
    }
    std::vector<std::string> ignored;
    model::Summary s;
    if (m.isTs()) {
        std::string e;
        s = model::summarize(*m.shape({}, ignored, e), model::DetailFilter{});
    } else {
        s = model::summarize(*m.scene({}, ignored));
    }
    std::map<std::string, size_t> counts;
    for (const model::Tri& t : s.tris) {
        ++counts[t.material >= 0 && t.material < int(s.materials.size()) ? s.materials[t.material] : "<none>"];
    }
    std::printf("%s: %zu tris, %zu nodes, %zu mesh holders\n", path.c_str(), s.tris.size(), s.nodes.size(), s.meshHolders.size());
    for (const auto& [name, n] : counts) {
        std::printf("  %8zu %s\n", n, name.c_str());
    }
    if (verbose) {
        for (const model::NodePose& n : s.nodes) {
            std::printf("  node %s\n", n.name.c_str());
        }
        for (const std::string& n : s.meshHolders) {
            std::printf("  holder %s\n", n.c_str());
        }
    }
}

void testFile(const std::string& path)
{
    if (axisOnly) {
        axisFile(path);
        return;
    }
    if (dumpOnly) {
        dumpFile(path);
        return;
    }
    const std::string ext = model::modelExtension(path);
    const bool isTs = ext == ".dts" || ext == ".cdae";
    if (isTs && targets.empty()) {
        testShapeFile(path);
    } else if (!targets.empty() && model::readerForExt(ext)) {
        testConvert(path);
    }
}

void testPath(const std::string& path)
{
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (fs::is_directory(p, ec)) {
        for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                std::printf("ITERATOR ERROR %s\n", ec.message().c_str());
                ec.clear();
            }
            const auto& e = *it;
            std::string file;
            try {
                if (e.is_regular_file()) {
                    file = e.path().u8string();
                    testFile(file);
                }
            } catch (const std::exception& ex) {
                std::printf("EXCEPTION %s: %s\n", file.c_str(), ex.what());
                ++failures;
            }
        }
    } else {
        testFile(path);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_DEBUG) && defined(_MSC_VER)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    std::vector<std::string> inputs;
    std::string reportPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--allow-reject") == 0 && i + 1 < argc) {
            allowReject.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--convert") == 0 && i + 1 < argc) {
            if (!parseTargets(argv[++i])) {
                return 2;
            }
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            dumpOnly = true;
        } else if (std::strcmp(argv[i], "--axis") == 0 && i + 1 < argc) {
            axisOnly = true;
            axisExpected = argv[++i][0] == 'Z' ? model::UpAxis::Z : model::UpAxis::Y;
        } else if (std::strcmp(argv[i], "--axis-ref") == 0 && i + 1 < argc) {
            axisRef = argv[++i];
        } else if (std::strcmp(argv[i], "--back") == 0) {
            convertBack = true;
        } else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            reportPath = argv[++i];
        } else {
            inputs.push_back(argv[i]);
        }
    }
    if (inputs.empty() || (!targets.empty() && outDir.empty())) {
        std::fprintf(stderr, "usage: gdc_model_test [--verbose] [--allow-reject <filename>]... [--convert glb,dae,... --out <dir> "
                             "[--back] [--report <file.json>]] <file-or-dir>...\n");
        return 2;
    }
    if (axisOnly && axisRef.empty()) {
        std::fprintf(stderr, "--axis needs --axis-ref <game-space model>\n");
        return 2;
    }
    for (const std::string& in : inputs) {
        testPath(in);
    }
    if (!reportPath.empty()) {
        std::ofstream(fs::u8path(reportPath)) << jsonReport.dump(2);
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
