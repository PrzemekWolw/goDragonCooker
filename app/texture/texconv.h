#pragma once

#include <string>
#include <vector>

// Loads the texconv shared library. Returns an error message, empty on success.
std::string texconvInit();
std::string texconvVersion();
// Returns an error message, empty on success. BC6H/BC7 calls are serialized; other formats may run concurrently.
// forceCPU adds -nogpu, so texconv never creates a DirectCompute device.
std::string texconvRun(const std::vector<std::string>& args, bool forceCPU = false);
