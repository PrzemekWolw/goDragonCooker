#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/ts/ts_shape.h"

namespace ts {

constexpr int kMinReadVersion = 19;
constexpr int kMaxReadVersion = 31;
constexpr int kMinDtsWriteVersion = 24;
constexpr int kMaxDtsWriteVersion = 29;

// Low 8 bits of the leading word; the exporter id lives in the high 16 bits.
int peekVersion(const std::vector<uint8_t>& data, int* exporter = nullptr);

// Reads DTS v19-29 or CDAE v31. Shared (parentMesh) data is resolved, decal meshes become null
// meshes and sorted meshes become standard meshes, so the result is self-contained.
bool readShape(const std::vector<uint8_t>& data, Shape& shape, std::string& error, std::vector<std::string>* warnings = nullptr);
bool readShapeFile(const std::string& path, Shape& shape, std::string& error, std::vector<std::string>* warnings = nullptr);

bool readDts(const std::vector<uint8_t>& data, Shape& shape, std::string& error, std::vector<std::string>* warnings);
bool readCdae(const std::vector<uint8_t>& data, Shape& shape, std::string& error);

// What a DTS version cannot store, for the UI and the conversion log.
std::vector<std::string> dtsVersionLosses(int version);
bool writeDts(const Shape& shape, int version, std::vector<uint8_t>& out, std::string& error, std::vector<std::string>* warnings = nullptr);

enum class CdaeCompression { Auto, On, Off };
struct CdaeWriteOptions {
    CdaeCompression compression = CdaeCompression::Auto;
    int zstdLevel = 3;
};
// Auto compresses bodies above 512 KB, the size the engine notes as the break-even point.
constexpr size_t kCdaeAutoCompressBytes = 512u * 1024u;
bool writeCdae(const Shape& shape, const CdaeWriteOptions& options, std::vector<uint8_t>& out, std::string& error, bool* compressed = nullptr);

} // namespace ts
