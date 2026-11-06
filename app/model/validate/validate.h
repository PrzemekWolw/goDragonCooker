#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/axis.h"
#include "model/formats.h"
#include "model/validate/summary.h"

namespace model {

// Allowed deviation for one conversion; widened for lossy targets (Draco quantization).
struct Tolerance {
    double posRel = 1e-5;   // of the bounding-box diagonal
    double normalDeg = 0.5;
    double uv = 1e-4;
    bool uvRangeRelative = false; // uv scales with the source UV extent (quantized over the range)
    int color = 1;          // 0..255
    double tangentDeg = 2.0;
    double rotDeg = 0.1;
    double transRel = 1e-4; // of the bounding-box diagonal
    double weight = 2e-3;
    double creaseDeg = 30.0; // normals differing more than this across an edge make it hard
};

// The tolerance for writing to `target` with `settings` (Draco widens position/normal/uv/color).
Tolerance toleranceFor(const Format& target, const nlohmann::json& settings);

enum class Verdict { Pass, Warn, Fail };
const char* verdictName(Verdict v);

struct ValidationReport {
    Verdict verdict = Verdict::Pass;
    std::vector<std::string> lines; // human-readable, one per check
    nlohmann::json json;
};

// Compares what the target can hold: `caps` is the intersection of source and target capabilities.
ValidationReport compareSummaries(const Summary& source, const Summary& result, const Capabilities& caps, const Tolerance& tol);

void checkUpAxis(ValidationReport& report, UpAxis expected, const RawFrame& raw, const Summary& source);

// `written` is the scene Assimp produces when it reads the file back. glTF also accounts for the
// extra ROOT parent the game inserts.
void checkGameLod(ValidationReport& report, const std::string& formatId, const aiScene& written, const aiScene& source, bool hierarchy);

void checkSeams(ValidationReport& report, const aiScene& written, const aiScene& source, double posRel);

Capabilities intersect(const Capabilities& a, const Capabilities& b);

} // namespace model
