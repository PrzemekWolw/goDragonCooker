#pragma once

#include <string>

namespace model {

struct DracoOptions {
    int level = 10; // 0..10, higher is smaller and slower
    int qNormal = 12;
    int qUv = 16;
    int qColor = 10;
    int qGeneric = 12; // tangents; joints and weights stay lossless
};

// Rewrites a glTF 2.0 file (.glb, or .gltf with external buffers) in place so triangle primitives
// use KHR_draco_mesh_compression. Primitives with morph targets, sparse or shared accessors stay
// uncompressed. `info` receives "N of M primitives compressed".
bool dracoCompressGltf(const std::string& path, const DracoOptions& options, std::string& error, std::string& info);

} // namespace model
