#pragma once

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
void* loadLibrary(const fs::path& path, std::string& error);
void* librarySymbol(void* library, const char* name);
// CPU name and RAM lines for the system report.
std::vector<std::string> platformSystemInfo();
