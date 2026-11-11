#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/formats.h"
#include "model/model.h"
#include "model/validate/validate.h"

namespace model {

struct ConvertOptions {
    const Format* target = nullptr;
    nlohmann::json settings; // per-target "Format settings"
    bool validate = false;
};

struct ConvertResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<std::string> log; // settings, sizes and validation lines
    bool validated = false;
    ValidationReport report;
    std::vector<std::string> foldedNodes; // LOD wrappers folded away before an Assimp export
};

ConvertResult convertModel(Model& source, const std::string& outputPath, const ConvertOptions& options);

ts::AiToTsOptions aiToTsOptions(const Format& target, const nlohmann::json& settings);
ts::TsToAiOptions tsToAiOptions(const Format& target, const nlohmann::json& settings);

struct OutputRequest {
    std::string source;
    std::string targetId;
    std::string ext; // with dot
};

// One output path per request: beside the source (outRoot empty) or mirrored from srcRoot into
// outRoot. When two sources share a stem, or the output would be the source itself, the source
// extension is kept (a.fbx -> a.fbx.glb).
std::vector<std::string> planOutputPaths(const std::vector<OutputRequest>& requests, const std::string& srcRoot, const std::string& outRoot);

} // namespace model
