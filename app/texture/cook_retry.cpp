#include "texture/cook_retry.h"

#include <atomic>

#include "core/platform.h"
#include "core/strings.h"
#include "texture/cook_rules.h"
#include "texture/texconv.h"

namespace {

constexpr double kMinGainDb = 0.5;

bool isBetter(const CookQuality& retry, const CookQuality& first)
{
    if (!retry.compared) {
        return false;
    }
    if (!first.compared) {
        return retry.error.empty() && first.verdict == CookVerdict::Fail;
    }
    return retry.psnr >= first.psnr + kMinGainDb && retry.badBlocks <= first.badBlocks;
}

} // namespace

CookRetry retryCook(const CookJob& job, const CookQuality& first, bool firstWasTexconv,
                    const std::vector<std::string>& gpuArgs, const CookThresholds& thresholds)
{
    CookRetry r;
    r.quality = first;
    if (!texconvInit().empty()) {
        r.note = "retry skipped: texconv not available";
        return r;
    }

    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec) / "goDragonCooker_retry" / std::to_string(counter++);
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    if (ec) {
        r.note = "retry skipped: cannot create " + u8str(temp);
        return r;
    }

    r.attempted = true;
    const std::vector<std::string> args = cookArgs(job, gpuArgs, u8str(temp));
    std::string codec = firstWasTexconv ? "texconv CPU" : "texconv";
    std::string error = texconvRun(args, firstWasTexconv);
    if (!error.empty() && !firstWasTexconv) {
        codec = "texconv CPU";
        error = texconvRun(args, true);
    }
    const fs::path ddsName = u8path(ddsPathFor(job)).filename();
    if (!error.empty()) {
        r.note = "retry failed: " + trim(error);
    } else {
        const CookQuality retry = validateCook(job, u8str(temp / ddsName), thresholds);
        const std::string firstPsnr = first.compared ? format("%.1f dB", first.psnr) : "unreadable";
        const std::string retryPsnr = retry.compared ? format("%.1f dB", retry.psnr) : retry.error;
        if (isBetter(retry, first)) {
            fs::copy_file(temp / ddsName, u8path(ddsPathFor(job)), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                r.note = "retry better but could not replace the DDS: " + ec.message();
            } else {
                r.improved = true;
                r.quality = retry;
                r.note = format("retry improved %s -> %s (%s)", firstPsnr.c_str(), retryPsnr.c_str(), codec.c_str());
            }
        } else {
            r.note = format("retry kept original (%s %s vs %s)", codec.c_str(), retryPsnr.c_str(), firstPsnr.c_str());
            if (firstWasTexconv) {
                r.note += ", the loss is inherent to the source";
            }
        }
    }
    fs::remove_all(temp, ec);
    return r;
}
