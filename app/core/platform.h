#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

fs::path u8path(const std::string& s);
std::string u8str(const fs::path& p);
fs::path exeDir();
fs::path userConfigDir();
std::string targetName();
fs::path findAsset(const std::string& name);
// findAsset for the platform's shared library file of `stem` (stem.dll, libstem.dylib, libstem.so).
fs::path findLibrary(const std::string& stem);
void* loadLibrary(const fs::path& path, std::string& error);
void* librarySymbol(void* library, const char* name);
// CPU name and RAM lines for the system report.
std::vector<std::string> platformSystemInfo();

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out, std::string& error);
// Creates the parent directories.
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data, std::string& error);
std::vector<std::string> readLines(const fs::path& path);
