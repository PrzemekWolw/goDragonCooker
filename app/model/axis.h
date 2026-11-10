#pragma once

#include <optional>
#include <string>

#include "model/ts/ts_shape.h"

template <typename T>
class aiMatrix4x4t;
typedef aiMatrix4x4t<float> aiMatrix4x4;
struct aiScene;

namespace model {

enum class UpAxis { X, Y, Z };

const char* upAxisName(UpAxis up); // "X", "Y" or "Z"

aiMatrix4x4 zUpToYUp();
aiMatrix4x4 yUpToZUp();
// The engine's COLLADA up-axis conversion (Y_UP: (-x, z, y), X_UP: (z, x, y)).
aiMatrix4x4 colladaUpToZUp(UpAxis up);

UpAxis importedUp(const std::string& ext);

// Rotates the whole scene the way Blender converts between Z-up and Y-up: vertex data (and skin/animation data)
// is rotated and every node transform conjugated, so a node keeps its orientation relative to its geometry.
void rotateScene(aiScene& scene, const aiMatrix4x4& rot);

// The frame of the coordinates a writer stores.
struct WriterFrame {
    UpAxis up = UpAxis::Y;
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
