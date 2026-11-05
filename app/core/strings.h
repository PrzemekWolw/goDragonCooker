#pragma once

#include <string>

std::string format(const char* fmt, ...);
std::string toLower(std::string s);
bool hasPrefix(const std::string& s, const std::string& prefix);
bool hasSuffix(const std::string& s, const std::string& suffix);
std::string trim(const std::string& s);
