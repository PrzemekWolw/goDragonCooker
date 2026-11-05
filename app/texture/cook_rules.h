#pragma once

#include <string>
#include <vector>

#include "texture/imageheader.h"
#include "texture/vulkan_codec.h"

// Matches the file suffix to a material slot (color/data/normal). Returns empty if no role matched.
std::string textureRole(const std::string& name);
bool isFloatHDR(const std::string& name);
bool isExpensiveCodec(const std::string& format);
// autoDetect (legacy default) always picks grayscale BC4; otherwise grayscale sources get BC4 and color sources BC7.
std::string resolveDataFormat(const ImgInfo& info, bool autoDetect);
const char* colorSpaceName(ColorSpace space);
// Fills format and color flags for a source with the given role. Returns false for an unknown role.
bool makeCookJob(const std::string& srcFile, const std::string& role, const ImgInfo& img, bool autoDetectFmt,
                 CookJob& job);
// texconv arguments for a cook job. gpuArgs are appended for BC6H/BC7; outDir defaults to the source folder.
std::vector<std::string> cookArgs(const CookJob& job, const std::vector<std::string>& gpuArgs,
                                  const std::string& outDir = "");
