#pragma once

#include <string>

enum class NormalEncoding { Linear, Srgb, DoubleLinear, Unknown };

const char* normalEncodingName(NormalEncoding encoding);

NormalEncoding detectNormalEncoding(const std::string& path);

std::string linearizeNormalMap(const std::string& path, NormalEncoding encoding, const std::string& destination,
                               std::string& error);
