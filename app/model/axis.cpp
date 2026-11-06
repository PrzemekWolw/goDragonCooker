#include "model/axis.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "core/platform.h"
#include "core/strings.h"
#include "model/formats.h"
#include "model/validate/summary.h"

namespace model {

const char* upAxisName(UpAxis up)
{
    switch (up) {
    case UpAxis::X:
        return "X";
    case UpAxis::Y:
        return "Y";
    default:
        return "Z";
    }
}

aiMatrix4x4 zUpToYUp() { return aiMatrix4x4(1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1); }

aiMatrix4x4 yUpToZUp() { return aiMatrix4x4(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1); }

aiMatrix4x4 colladaUpToZUp(UpAxis up)
{
    switch (up) {
    case UpAxis::Y:
        return aiMatrix4x4(-1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    case UpAxis::X:
        return aiMatrix4x4(0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    default:
        return aiMatrix4x4();
    }
}

UpAxis importedUp(const std::string& ext)
{
    static const std::set<std::string> zUp = {".stl", ".ply", ".3mf", ".smd", ".vta"};
    return zUp.count(toLower(ext)) ? UpAxis::Z : UpAxis::Y;
}

WriterFrame writerFrame(const std::string& formatId)
{
    // Z-up formats keep game coordinates. COLLADA writes its own <up_axis>Z_UP</up_axis>.
    static const std::set<std::string> zUp = {"collada", "stl", "ply", "3ds", "3mf", "stp"};
    WriterFrame f;
    f.up = zUp.count(formatId) ? UpAxis::Z : UpAxis::Y;
    f.wrapRoot = formatId == "fbx";
    return f;
}

namespace {

std::optional<UpAxis> colladaDeclared(const std::string& path)
{
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t p = text.find("<up_axis>");
    if (p == std::string::npos) {
        return UpAxis::Y; // COLLADA's default when the element is missing
    }
    const std::string v = text.substr(p + 9, 8);
    return v.find('X') == 0 ? UpAxis::X : v.find('Y') == 0 ? UpAxis::Y : UpAxis::Z;
}

} // namespace

RawFrame readRawFrame(const std::string& path)
{
    RawFrame r;
    const std::string ext = modelExtension(path);
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_COLLADA_IGNORE_UP_DIRECTION, true);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, true);
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_SortByPType);
    if (!scene || !scene->mRootNode) {
        r.error = importer.GetErrorString();
        return r;
    }
    if (ext == ".dae") {
        r.declared = colladaDeclared(path);
    } else if (ext == ".fbx" && scene->mMetaData) {
        int32_t axis = 0, sign = 1;
        if (scene->mMetaData->Get("UpAxis", axis)) {
            r.declared = axis == 0 ? UpAxis::X : axis == 1 ? UpAxis::Y : UpAxis::Z;
            scene->mMetaData->Get("UpAxisSign", sign);
            r.declaredNegative = sign < 0;
        }
    }
    const Summary s = summarize(*scene);
    r.hasGeometry = !s.tris.empty();
    r.bounds = s.bounds;
    r.ok = true;
    return r;
}

} // namespace model
