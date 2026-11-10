// Cooks images with texconv and runs the cook validation and retry on them.
//   gdc_cook_test --work <dir> [--target <dB>] [--retry] [--corrupt] <role>:<image>...
// role is color, normal or data. --target sets every PSNR target (a high value forces the retry path).
// --corrupt zeroes a run of blocks in the cooked DDS, which must be reported as a quality failure.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/platform.h"
#include "core/strings.h"
#include "texture/cook_retry.h"
#include "texture/cook_rules.h"
#include "texture/cook_validate.h"
#include "texture/texconv.h"

static void corruptBlocks(const std::string& dds)
{
    std::fstream file(u8path(dds), std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(84);
    char cc[4];
    file.read(cc, 4);
    const std::streamoff data = std::string(cc, 4) == "DX10" ? 148 : 128;
    file.seekp(data + 64 * 16);
    const std::vector<char> zeros(16 * 16, 0);
    file.write(zeros.data(), std::streamsize(zeros.size()));
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string work;
    CookThresholds thresholds;
    bool retry = false, corrupt = false;
    std::vector<std::pair<std::string, std::string>> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--work" && i + 1 < argc) {
            work = argv[++i];
        } else if (a == "--target" && i + 1 < argc) {
            thresholds.bc7 = thresholds.bc5 = thresholds.bc4 = std::stod(argv[++i]);
        } else if (a == "--retry") {
            retry = true;
        } else if (a == "--corrupt") {
            corrupt = true;
        } else if (const size_t colon = a.find(':'); colon != std::string::npos && colon > 1) {
            inputs.emplace_back(a.substr(0, colon), a.substr(colon + 1));
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (work.empty() || inputs.empty()) {
        std::fprintf(stderr, "usage: gdc_cook_test --work <dir> [--target dB] [--retry] [--corrupt] <role>:<image>...\n");
        return 2;
    }
    if (const std::string error = texconvInit(); !error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    std::error_code ec;
    fs::create_directories(u8path(work), ec);
    int failures = 0;
    for (const auto& [role, image] : inputs) {
        const fs::path src = u8path(image);
        const fs::path dst = u8path(work) / (u8str(src.stem()) + "." + role + u8str(src.extension()));
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "FAIL %s: copy failed (%s)\n", image.c_str(), ec.message().c_str());
            ++failures;
            continue;
        }
        ImgInfo info;
        std::string error;
        if (!readImageHeader(dst, info, error)) {
            std::fprintf(stderr, "FAIL %s: %s\n", image.c_str(), error.c_str());
            ++failures;
            continue;
        }
        CookJob job;
        makeCookJob(u8str(dst), role, info, true, job);
        if (!(error = texconvRun(cookArgs(job, {}), true)).empty()) {
            std::fprintf(stderr, "FAIL %s: cook failed (%s)\n", image.c_str(), trim(error).c_str());
            ++failures;
            continue;
        }
        if (corrupt) {
            corruptBlocks(ddsPathFor(job));
        }
        const CookQuality q = validateCook(job, ddsPathFor(job), thresholds);
        std::fprintf(stderr, "%-16s %s %s: %s\n", cookVerdictName(q.verdict), job.format.c_str(), u8str(dst.filename()).c_str(),
                     describeQuality(q).c_str());
        bool improved = false;
        if (retry && q.lowQuality) {
            const CookRetry r = retryCook(job, q, true, {}, thresholds);
            std::fprintf(stderr, "  %s\n", r.note.c_str());
            improved = r.improved;
        }
        if (corrupt ? q.verdict != CookVerdict::Fail || (retry && !improved)
                    : q.verdict == CookVerdict::Fail && !retry) {
            ++failures;
        }
    }
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
