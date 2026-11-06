#pragma once

#include <string>
#include <vector>

#include "texture/imageheader.h"
#include "texture/vulkan_codec.h"

// Matches the file suffix to a material slot (color/data/dataR/normal). Returns empty if no role matched.
std::string textureRole(const std::string& name);
bool isFloatHDR(const std::string& name);
bool isExpensiveCodec(const std::string& format);
// assumeGrayscale forces BC4 for .data. Grayscale .data is BC4 either way; color .data is BC7. .dataR is always BC4.
std::string resolveDataFormat(const ImgInfo& info, bool assumeGrayscale);
const char* colorSpaceName(ColorSpace space);
// Fills format and color flags for a source with the given role. Returns false for an unknown role.
bool makeCookJob(const std::string& srcFile, const std::string& role, const ImgInfo& img, bool autoDetectFmt,
                 CookJob& job);
// texconv arguments for a cook job. gpuArgs are appended for BC6H/BC7; outDir defaults to the source folder.
std::vector<std::string> cookArgs(const CookJob& job, const std::vector<std::string>& gpuArgs,
                                  const std::string& outDir = "");
