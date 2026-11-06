#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/assimp/ai_bridge.h"
#include "model/ts/ts_shape.h"

struct aiScene;

namespace model {

// Node metadata holding a node's real name when the writer had to rename it.
constexpr const char* kOriginalNameKey = "gdc_name";

// A loaded model in whichever form it was read (TSShape or aiScene); the other form is built on
// demand and cached, so converting one source to several targets converts it only once.
class Model {
public:
    Model();
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    bool load(const std::string& path, std::string& error, std::vector<std::string>& warnings);

    bool isTs() const { return fromTs; }
    const std::string& path() const { return sourcePath; }
    // "Y-up (.glb) -> Z-up": the frame the file was read in and what it was converted to (game space).
    const std::string& upAxisNote() const { return upNote; }

    const ts::Shape* shape(const ts::AiToTsOptions& options, std::vector<std::string>& warnings, std::string& error);
    const aiScene* scene(const ts::TsToAiOptions& options, std::vector<std::string>& warnings);

private:
    std::string sourcePath;
    bool fromTs = false;
    std::string upNote;

    std::optional<ts::Shape> loadedShape;
    std::unique_ptr<aiScene> loadedScene;

    std::optional<ts::Shape> convertedShape;
    ts::AiToTsOptions convertedShapeOptions;
    std::vector<std::string> convertedShapeWarnings;
    std::unique_ptr<aiScene> convertedScene;
    ts::TsToAiOptions convertedSceneOptions;
    std::vector<std::string> convertedSceneWarnings;
};

} // namespace model
