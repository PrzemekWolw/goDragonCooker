#pragma once

#include <optional>
#include <string>

#include "model/ts/ts_shape.h"

template <typename T>
class aiMatrix4x4t;
typedef aiMatrix4x4t<float> aiMatrix4x4;

namespace model {

enum class UpAxis { X, Y, Z };

const char* upAxisName(UpAxis up); // "X", "Y" or "Z"

aiMatrix4x4 zUpToYUp();
aiMatrix4x4 yUpToZUp();
// The engine's COLLADA up-axis conversion (Y_UP: (-x, z, y), X_UP: (z, x, y)).
aiMatrix4x4 colladaUpToZUp(UpAxis up);

UpAxis importedUp(const std::string& ext);

// How a writer stores the scene: the frame of the written coordinates, and whether Assimp's exporter drops
// the root node's transform (FBX), so the rotation has to sit on a node below a new root.
struct WriterFrame {
    UpAxis up = UpAxis::Y;
    bool wrapRoot = false;
};
WriterFrame writerFrame(const std::string& formatId);

// What an exported file says and holds, read without any up-axis conversion.
struct RawFrame {
    bool ok = false;
    std::string error;
    std::optional<UpAxis> declared; // COLLADA <up_axis>, FBX UpAxis; none for formats without the field
    bool declaredNegative = false;  // FBX -Y etc.
    ts::Box bounds;                 // world-space bounds of the triangles as stored
    bool hasGeometry = false;
};
RawFrame readRawFrame(const std::string& path);

} // namespace model
