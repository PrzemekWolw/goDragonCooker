#include "tabs/tabs.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/i18n.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/strings.h"
#include "core/task.h"
#include "gpu/gpu.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "texture/cook_retry.h"
#include "texture/cook_rules.h"
#include "texture/cook_validate.h"
#include "texture/texconv.h"
#include "texture/vulkan_codec.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace {

// UI-thread-only fields are plain; runs get copies.
struct CookValidation {
    bool validate = false;
    bool retry = true;
    CookThresholds thresholds;
};

struct CookTab {
    TaskState task;
    std::string directory;
    bool autoDetectFmt = true;
    CookValidation validation;
};

CookTab cookTab;

struct CookCheck {
    bool ran = false;
    CookQuality quality;
    CookRetry retry;
};

std::string baseName(const std::string& path)
{
    return u8str(u8path(path).filename());
}

CookCheck checkCook(const CookJob& job, const CookValidation& v, bool firstWasTexconv)
{
    CookCheck c;
    if (!v.validate) {
        return c;
    }
    c.ran = true;
    c.quality = validateCook(job, ddsPathFor(job), v.thresholds);
    if (v.retry && c.quality.lowQuality) {
        c.retry = retryCook(job, c.quality, firstWasTexconv, preferredGPUArgs(), v.thresholds);
        c.quality = c.retry.quality;
    }
    return c;
}

void runCooker(std::string directory, bool autoDetectFmt, CookValidation validation)
{
    TaskState& tab = cookTab.task;
    tab.begin(tr("gdc.cook.seekingFiles"));
    std::string backend;
    int vulkanDevice;
    {
        std::lock_guard lock(app.mu);
        backend = app.textureBackend;
        vulkanDevice = app.vulkanDevice;
    }

    tab.log(tr("gdc.cook.startingProcessing"));
    tab.log(tr("gdc.cook.seekingFiles"));

    std::vector<CookJob> jobs;
    std::error_code ec;
    const fs::path root = u8path(directory);
    if (!fs::is_directory(root, ec)) {
        tab.log(format(tr("gdc.common.walkErrorCannotRead"), directory.c_str()));
        tab.finish(tr("gdc.common.error"));
        return;
    }
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const fs::path path = it->path();
        const std::string name = u8str(path.filename());
        const std::string role = textureRole(name);
        if (role.empty()) {
            continue;
        }

        ImgInfo img;
        std::string error;
        if (!readImageHeader(path, img, error)) {
            tab.log(format("  SKIP: %s (%s)", name.c_str(), error.c_str()));
            continue;
        }
        if (img.bits != 0 && img.bits != 8 && !isFloatHDR(name)) {
            tab.log(format(tr("gdc.cook.skipNot8BitGot"), name.c_str(), img.bits));
            continue;
        }

        CookJob job;
        makeCookJob(u8str(path), role, img, autoDetectFmt, job);

        fs::path dds = path;
        if (fs::exists(dds.replace_extension(".dds"), ec)) {
            continue;
        }
        jobs.push_back(job);
        tab.log(format(tr("gdc.cook.queue"), name.c_str(), job.format.c_str(), colorSpaceName(img.colorSpace)));
    }
    if (ec) {
        tab.log(format(tr("gdc.cook.walkError"), ec.message().c_str()));
        tab.finish(tr("gdc.common.error"));
        return;
    }

    const size_t total = jobs.size();
    if (total == 0) {
        tab.log(tr("gdc.cook.noFilesToCook"));
        tab.finish(tr("gdc.common.nothingToDo"));
        return;
    }

    std::vector<CookJob> codecJobs, cpuJobs;
    for (const CookJob& job : jobs) {
        (isExpensiveCodec(job.format) ? codecJobs : cpuJobs).push_back(job);
    }

    tab.setStatus(tr("gdc.cook.cookingTextures"));

    const auto t0 = std::chrono::steady_clock::now();
    size_t done = 0, cooked = 0, failed = 0, batchCount = 0;
    size_t validated = 0, passed = 0, warned = 0, qualityFailed = 0, headerOnly = 0;
    size_t retried = 0, improved = 0, keptOriginal = 0;
    const bool firstWasTexconv = backend != textureBackendCompressonator;
    std::vector<CookCheck> checks(jobs.size());
    auto recordResult = [&](const std::string& name, const std::string& error, const CookCheck& check) {
        ++done;
        if (!error.empty()) {
            tab.log(format("  FAIL: %s - %s", name.c_str(), trim(error).c_str()));
            ++failed;
        } else if (!check.ran) {
            tab.log(format("  OK: %s", name.c_str()));
            ++cooked;
        } else {
            ++cooked;
            ++validated;
            const CookVerdict verdict = check.quality.verdict;
            const char* tag = verdict == CookVerdict::Fail ? "FAIL" : verdict == CookVerdict::Warn ? "WARN" : "OK";
            tab.log(format("  %s: %s - %s", tag, name.c_str(), describeQuality(check.quality).c_str()));
            switch (verdict) {
            case CookVerdict::Ok: ++passed; break;
            case CookVerdict::HeaderOnly: ++headerOnly; break;
            case CookVerdict::Warn: ++warned; break;
            case CookVerdict::Fail: ++qualityFailed; break;
            }
            if (!check.retry.note.empty()) {
                tab.log("    " + check.retry.note);
            }
            if (check.retry.attempted) {
                ++retried;
                ++(check.retry.improved ? improved : keptOriginal);
            }
        }
        if (++batchCount >= 16 || done == total) {
            tab.setStatus(format(tr("gdc.cook.cookingCookedFailed"), done, total, cooked, failed));
            batchCount = 0;
        }
    };

    if (backend == textureBackendCompressonator) {
        const unsigned workers = std::min<unsigned>(vulkanPipelineWorkerCount, unsigned(total));
        tab.log(format(tr("gdc.cook.cookingFilesWithCompressonatorVulkan"), total,
                       vulkanWorkerCount, workers));
        runPool(
            jobs.size(), workers, tab,
            [&](size_t i) {
                std::string error = vulkanCompress(jobs[i], vulkanDevice);
                if (error.empty()) {
                    checks[i] = checkCook(jobs[i], validation, firstWasTexconv);
                }
                return error;
            },
            [&](size_t i, const std::string& error) { recordResult(baseName(jobs[i].srcFile), error, checks[i]); });
    } else {
        tab.log(format(tr("gdc.cook.cookingFilesBc6hBc7First"), total, codecJobs.size(),
                       cpuJobs.size()));

        bool useCPUCodec = false;
        for (const CookJob& job : codecJobs) {
            if (tab.stop) {
                tab.log(tr("gdc.cook.cookingCancelled"));
                tab.finish(tr("gdc.common.cancelled"));
                return;
            }
            const std::vector<std::string> args = cookArgs(job, preferredGPUArgs());
            std::string error;
            if (useCPUCodec) {
                error = texconvRun(args, true);
            } else if (!(error = texconvRun(args)).empty()) {
                useCPUCodec = true;
                const std::string fallbackError = texconvRun(args, true);
                if (fallbackError.empty()) {
                    error.clear();
                    tab.log(format(tr("gdc.cook.gpuCodecFailedForUsing"),
                                   baseName(job.srcFile).c_str()));
                } else {
                    error += "; CPU fallback: " + fallbackError;
                }
            }
            const CookCheck check = error.empty() ? checkCook(job, validation, firstWasTexconv) : CookCheck{};
            recordResult(baseName(job.srcFile), error, check);
        }

        if (!cpuJobs.empty()) {
            const unsigned workers =
                std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(cpuJobs.size())));
            tab.log(format(tr("gdc.cook.runningCpuBatchTexturesWith"), cpuJobs.size(), workers));
            std::vector<CookCheck> cpuChecks(cpuJobs.size());
            runPool(
                cpuJobs.size(), workers, tab,
                [&](size_t i) {
                    std::string error = texconvRun(cookArgs(cpuJobs[i], preferredGPUArgs()));
                    if (error.empty()) {
                        cpuChecks[i] = checkCook(cpuJobs[i], validation, firstWasTexconv);
                    }
                    return error;
                },
                [&](size_t i, const std::string& error) {
                    recordResult(baseName(cpuJobs[i].srcFile), error, cpuChecks[i]);
                });
        }
    }

    if (tab.stop) {
        tab.log(tr("gdc.cook.cookingCancelled"));
        tab.finish(tr("gdc.common.cancelled"));
        return;
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    tab.log(format(tr("gdc.cook.finishedInSTexturesCooked"), elapsed, cooked, total, failed));
    if (validation.validate) {
        tab.log(format(tr("gdc.cook.validationValidatedPassedWarnedFailed"), validated,
                       passed, warned, qualityFailed, headerOnly));
        if (validation.retry) {
            tab.log(format(tr("gdc.cook.retryRetriedImprovedKeptOriginal"), retried, improved, keptOriginal));
        }
    }
    tab.finish(format(tr("gdc.cook.doneCooked"), cooked, total));
}

void gpuControlsUI()
{
    std::vector<std::string> report, vulkanDevices;
    int configuredGPU, configuredVulkanDevice;
    std::string backend;
    bool checking;
    {
        std::lock_guard lock(app.mu);
        report = app.gpuReport;
        configuredGPU = app.gpuAdapter;
        configuredVulkanDevice = app.vulkanDevice;
        backend = app.textureBackend;
        vulkanDevices = app.vulkanDevices;
        checking = app.gpuChecking;
    }
    ImGui::Separator();

    static const std::vector<std::string> backendOptions = {"Compressonator (Vulkan GPU)", "Texconv"};
    int backendIndex = backend == textureBackendTexconv ? 1 : 0;
    ImGui::TextUnformatted(tr("gdc.cook.textureBackend"));
    ImGui::SameLine();
    if (comboStrings("##texture_backend", &backendIndex, backendOptions, px(320))) {
        {
            std::lock_guard lock(app.mu);
            app.textureBackend = backendIndex == 1 ? textureBackendTexconv : textureBackendCompressonator;
        }
        saveSettings();
    }

    if (backend == textureBackendCompressonator) {
        std::vector<std::string> vulkanOptions = {"gdc.common.automaticBest"};
        for (size_t index = 0; index < vulkanDevices.size(); ++index) {
            vulkanOptions.push_back(format("%zu: %s", index, vulkanDevices[index].c_str()));
        }
        int vulkanIndex = configuredVulkanDevice + 1;
        if (vulkanIndex < 0 || vulkanIndex >= int(vulkanOptions.size())) {
            vulkanIndex = 0;
        }
        ImGui::TextUnformatted(tr("gdc.cook.compressonatorGpu"));
        ImGui::SameLine();
        if (comboStrings("##compressonator_gpu", &vulkanIndex, vulkanOptions, px(320))) {
            {
                std::lock_guard lock(app.mu);
                app.vulkanDevice = vulkanIndex - 1;
            }
            saveSettings();
            rerunGPUCheck();
        }
    } else {
#if defined(_WIN32)
        const std::vector<std::string> options = gpuAdapterOptions();
        int comboIndex = gpuAdapterOptionIndex(configuredGPU);
        if (comboIndex < 0 || comboIndex >= int(options.size())) {
            comboIndex = 0;
        }
        ImGui::TextUnformatted(tr("gdc.cook.texconvGpu"));
        ImGui::SameLine();
        if (comboStrings("##texconv_gpu", &comboIndex, options, px(320))) {
            {
                std::lock_guard lock(app.mu);
                app.gpuAdapter = gpuAdapterIndexForOption(comboIndex);
            }
            saveSettings();
            rerunGPUCheck();
        }
        if (checking) {
            ImGui::SameLine();
            ImGui::TextUnformatted(tr("gdc.cook.testing"));
        }
#else
        (void)configuredGPU;
        if (checking) {
            ImGui::TextUnformatted(tr("gdc.cook.checkingHardware"));
        }
#endif
    }

    if (report.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colTextDim);
    ImGui::TextUnformatted(tr("gdc.cook.system"));
    for (const std::string& line : report) {
        ImGui::TextUnformatted(tr(line));
    }
    ImGui::PopStyleColor();
}

ImVec4 cookLogColor(const std::string& line)
{
    const std::string lower = toLower(line);
    if (hasPrefix(lower, "  ok:")) {
        return colCookOk;
    }
    if (hasPrefix(lower, "  fail:") || hasPrefix(lower, "error")) {
        return colCookErr;
    }
    if (hasPrefix(lower, "  warn:")) {
        return colWarn;
    }
    if (hasPrefix(lower, "    retry")) {
        return colTextDim;
    }
    if (hasPrefix(lower, "  skip:")) {
        return colTextDim;
    }
    return colText;
}

} // namespace

void cookTabInit()
{
    registerSettings(
        [](const SettingsJson& j) {
            if (!j.contains("cook") || !j["cook"].is_object()) {
                return;
            }
            const SettingsJson& s = j["cook"];
            CookValidation& v = cookTab.validation;
            v.validate = s.value("validate", v.validate);
            v.retry = s.value("retry", v.retry);
            v.thresholds.bc7 = s.value("targetBC7", v.thresholds.bc7);
            v.thresholds.bc5 = s.value("targetBC5", v.thresholds.bc5);
            v.thresholds.bc4 = s.value("targetBC4", v.thresholds.bc4);
        },
        [](SettingsJson& j) {
            const CookValidation& v = cookTab.validation;
            j["cook"] = {{"validate", v.validate},
                         {"retry", v.retry},
                         {"targetBC7", v.thresholds.bc7},
                         {"targetBC5", v.thresholds.bc5},
                         {"targetBC4", v.thresholds.bc4}};
        });
}

static void validationControlsUI(CookValidation& v)
{
    bool changed = ImGui::Checkbox(trId("gdc.cook.validateAfterCook", "cook_validate").c_str(), &v.validate);
    tooltip("gdc.cook.validateAfterCook.tooltip");
    ImGui::BeginDisabled(!v.validate);
    ImGui::Indent();
    changed |= ImGui::Checkbox(trId("gdc.cook.retryWithTexconvWhenQuality", "cook_retry").c_str(), &v.retry);
    tooltip("gdc.cook.retryWithTexconvWhenQuality.tooltip");
    if (ImGui::TreeNode(trId("gdc.cook.qualityTargetsPsnrDb", "cook_targets").c_str())) {
        auto target = [&](const char* label, double* value) {
            ImGui::SetNextItemWidth(px(140));
            if (ImGui::InputDouble(trId(label, label).c_str(), value, 0.5, 1.0, "%.1f")) {
                *value = std::clamp(*value, 10.0, 80.0);
                changed = true;
            }
        };
        target("gdc.cook.bc7ColorData", &v.thresholds.bc7);
        target("gdc.cook.bc5Normal", &v.thresholds.bc5);
        target("gdc.cook.bc4Data", &v.thresholds.bc4);
        ImGui::TextColored(colTextDim, tr("gdc.cook.belowTargetWarnsAndRetries"),
                           v.thresholds.floorMargin);
        ImGui::TreePop();
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    if (changed) {
        saveSettings();
    }
}

void cookTabUI()
{
    if (!ImGui::BeginTabItem(trId("gdc.cook.textureCooker", "tab_cook").c_str())) {
        return;
    }
    CookTab& c = cookTab;
    ImGui::TextUnformatted(tr("gdc.common.texturesDirectory"));
    ImGui::InputText("##cook_directory", &c.directory);
    ImGui::SameLine();
    if (ImGui::Button(trId("gdc.common.browse", "cook_browse").c_str())) {
        openDirPicker(&c.directory);
    }

    ImGui::Separator();
    ImGui::Checkbox(trId("gdc.cook.assumeDataTexturesAreGrayscale", "cook_auto_fmt").c_str(), &c.autoDetectFmt);
    tooltip("gdc.cook.assumeDataTexturesAreGrayscale.tooltip");
    validationControlsUI(c.validation);
    ImGui::Separator();

    std::string status;
    bool running;
    std::vector<std::string> logLines;
    {
        std::lock_guard lock(c.task.mu);
        status = c.task.status;
        running = c.task.running;
        logLines = c.task.logLines;
    }
    if (running) {
        if (ImGui::Button(trId("gdc.cook.stopCooker", "cook_run").c_str())) {
            c.task.stop = true;
        }
    } else if (ImGui::Button(trId("gdc.cook.runTextureCooker", "cook_run").c_str()) && !c.directory.empty()) {
        c.task.log("");
        std::thread(runCooker, c.directory, c.autoDetectFmt, c.validation).detach();
    }
    ImGui::Text(tr("gdc.common.status"), status.empty() ? tr("gdc.common.waiting") : status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted(tr("gdc.common.output"));
    logView("##cook_log", logLines, 280, cookLogColor);

    gpuControlsUI();
    ImGui::EndTabItem();
}
