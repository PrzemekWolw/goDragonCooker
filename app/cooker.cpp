#include "app.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

#include "imgui_stdlib.h"

// ---------------------------------------------------------------------------
// Task state
// ---------------------------------------------------------------------------

void TaskState::log(const std::string& msg)
{
    std::lock_guard lock(mu);
    logLines.push_back(msg);
    if (logLines.size() > 500) {
        logLines.erase(logLines.begin(), logLines.end() - 500);
    }
}

void TaskState::setStatus(const std::string& value)
{
    {
        std::lock_guard lock(mu);
        status = value;
    }
    requestRedraw();
}

void TaskState::begin(const std::string& value)
{
    {
        std::lock_guard lock(mu);
        running = true;
        status = value;
        stop = false;
    }
    requestRedraw();
}

void TaskState::finish(const std::string& value)
{
    {
        std::lock_guard lock(mu);
        running = false;
        status = value;
    }
    requestRedraw();
}

// ---------------------------------------------------------------------------
// Texture Cooker
// ---------------------------------------------------------------------------

// Matches the file suffix to a material slot (color/data/normal). Returns empty if no role matched.
static std::string textureRole(const std::string& name)
{
    const std::string lower = toLower(name);
    if (imageFormat(lower).empty()) {
        return "";
    }
    const std::string ext = u8str(u8path(lower).extension());
    for (const char* role : {"color", "data", "normal"}) {
        if (hasSuffix(lower, "." + std::string(role) + ext)) {
            return role;
        }
    }
    return "";
}

static bool isFloatHDR(const std::string& name)
{
    const std::string lower = toLower(name);
    return hasSuffix(lower, ".exr") || hasSuffix(lower, ".hdr");
}

static bool isExpensiveCodec(const std::string& format)
{
    return hasPrefix(format, "BC6H_") || hasPrefix(format, "BC7_");
}

// autoDetect (legacy default) always picks grayscale BC4; otherwise grayscale sources get BC4 and color sources BC7.
static std::string resolveDataFormat(const ImgInfo& info, bool autoDetect)
{
    if (autoDetect || info.colorType == ColorType::Grayscale || info.colorType == ColorType::GrayAlpha) {
        return "BC4_UNORM";
    }
    return "BC7_UNORM";
}

static const char* colorSpaceName(ColorSpace space)
{
    switch (space) {
    case ColorSpace::Linear: return "linear";
    case ColorSpace::SRGB: return "sRGB";
    default: return "unknown";
    }
}

static std::vector<std::string> cookArgs(const CookJob& job)
{
    std::vector<std::string> args = {"-y", "-pow2"};
    if (job.sepa) args.push_back("-sepalpha");
    if (job.ignoreSRGB) args.push_back("--ignore-srgb");
    if (job.inputSRGB) args.push_back("-srgbi");
    if (job.srgb) args.push_back("-srgbo");
    if (hasPrefix(job.format, "BC7_")) {
        args.insert(args.end(), {"-bc", "x"});
    }
    if (isExpensiveCodec(job.format)) {
        const std::vector<std::string> gpuArgs = preferredGPUArgs();
        args.insert(args.end(), gpuArgs.begin(), gpuArgs.end());
    }
    args.insert(args.end(), {"-o", u8str(u8path(job.srcFile).parent_path()), "-f", job.format, "--", job.srcFile});
    return args;
}

static std::string baseName(const std::string& path)
{
    return u8str(u8path(path).filename());
}

// Runs work() over jobs on up to `workers` threads, stopping early when the task is cancelled.
static void runPool(const std::vector<CookJob>& jobs, unsigned workers, TaskState& task,
                    const std::function<std::string(const CookJob&)>& work,
                    const std::function<void(const std::string&, const std::string&)>& record)
{
    std::atomic<size_t> next{0};
    std::mutex recordMu;
    std::vector<std::thread> threads;
    workers = std::max(1u, std::min<unsigned>(workers, unsigned(jobs.size())));
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            while (!task.stop) {
                const size_t index = next++;
                if (index >= jobs.size()) {
                    return;
                }
                const std::string error = work(jobs[index]);
                std::lock_guard lock(recordMu);
                record(baseName(jobs[index].srcFile), error);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

static void runCooker(std::string directory, bool autoDetectFmt)
{
    TaskState& tab = cookTab.task;
    tab.begin("Seeking files...");
    std::string backend;
    int vulkanDevice;
    {
        std::lock_guard lock(app.mu);
        backend = app.textureBackend;
        vulkanDevice = app.vulkanDevice;
    }

    tab.log("Starting processing");
    tab.log("Seeking files");

    std::vector<CookJob> jobs;
    std::error_code ec;
    const fs::path root = u8path(directory);
    if (!fs::is_directory(root, ec)) {
        tab.log("Walk error: cannot read " + directory);
        tab.finish("Error");
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
            tab.log(format("  SKIP: %s (not 8-bit, got %d-bit)", name.c_str(), img.bits));
            continue;
        }

        CookJob job;
        job.srcFile = u8str(path);
        if (role == "color") {
            job.format = "BC7_UNORM_SRGB";
            job.srgb = job.sepa = true;
            job.inputSRGB = img.colorSpace != ColorSpace::Linear;
        } else if (role == "data") {
            job.format = isFloatHDR(name) ? "BC7_UNORM" : resolveDataFormat(img, autoDetectFmt);
            job.ignoreSRGB = true;
        } else {
            job.format = "BC5_UNORM";
            job.ignoreSRGB = true;
        }

        fs::path dds = path;
        if (fs::exists(dds.replace_extension(".dds"), ec)) {
            continue;
        }
        jobs.push_back(job);
        tab.log(format("  Queue: %s (%s, %s)", name.c_str(), job.format.c_str(), colorSpaceName(img.colorSpace)));
    }
    if (ec) {
        tab.log("Walk error: " + ec.message());
        tab.finish("Error");
        return;
    }

    const size_t total = jobs.size();
    if (total == 0) {
        tab.log("No files to cook.");
        tab.finish("Nothing to do");
        return;
    }

    std::vector<CookJob> codecJobs, cpuJobs;
    for (const CookJob& job : jobs) {
        (isExpensiveCodec(job.format) ? codecJobs : cpuJobs).push_back(job);
    }

    tab.setStatus("Cooking textures...");

    const auto t0 = std::chrono::steady_clock::now();
    size_t done = 0, cooked = 0, failed = 0, batchCount = 0;
    auto recordResult = [&](const std::string& name, const std::string& error) {
        ++done;
        if (!error.empty()) {
            tab.log(format("  FAIL: %s - %s", name.c_str(), trim(error).c_str()));
            ++failed;
        } else {
            tab.log(format("  OK: %s", name.c_str()));
            ++cooked;
        }
        if (++batchCount >= 16 || done == total) {
            tab.setStatus(format("Cooking: %zu/%zu (%zu cooked, %zu failed)", done, total, cooked, failed));
            batchCount = 0;
        }
    };

    if (backend == textureBackendCompressonator) {
        const unsigned workers = std::min<unsigned>(vulkanPipelineWorkerCount, unsigned(total));
        tab.log(format("Cooking %zu files with Compressonator Vulkan (%u GPU workers, %u pipeline workers).", total,
                       vulkanWorkerCount, workers));
        runPool(jobs, workers, tab, [&](const CookJob& job) { return vulkanCompress(job, vulkanDevice); },
                recordResult);
    } else {
        tab.log(format("Cooking %zu files: %zu BC6H/BC7 first, then %zu CPU-batch", total, codecJobs.size(),
                       cpuJobs.size()));

        bool useCPUCodec = false;
        for (const CookJob& job : codecJobs) {
            if (tab.stop) {
                tab.log("Cooking cancelled.");
                tab.finish("Cancelled");
                return;
            }
            const std::vector<std::string> args = cookArgs(job);
            std::string error;
            if (useCPUCodec) {
                error = texconvRun(args, true);
            } else if (!(error = texconvRun(args)).empty()) {
                useCPUCodec = true;
                const std::string fallbackError = texconvRun(args, true);
                if (fallbackError.empty()) {
                    error.clear();
                    tab.log(format("  GPU codec failed for %s; using CPU codec for remaining BC6H/BC7 textures.",
                                   baseName(job.srcFile).c_str()));
                } else {
                    error += "; CPU fallback: " + fallbackError;
                }
            }
            recordResult(baseName(job.srcFile), error);
        }

        if (!cpuJobs.empty()) {
            const unsigned workers =
                std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), unsigned(cpuJobs.size())));
            tab.log(format("Running %zu CPU-batch textures with %u workers.", cpuJobs.size(), workers));
            runPool(cpuJobs, workers, tab, [](const CookJob& job) { return texconvRun(cookArgs(job)); },
                    recordResult);
        }
    }

    if (tab.stop) {
        tab.log("Cooking cancelled.");
        tab.finish("Cancelled");
        return;
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    tab.log(format("Finished in %.2fs (%zu/%zu textures cooked, %zu failed)", elapsed, cooked, total, failed));
    tab.finish(format("Done: %zu/%zu cooked", cooked, total));
}

// ---------------------------------------------------------------------------
// GPU check
// ---------------------------------------------------------------------------

static std::vector<std::string> checkTexconvGPU()
{
    std::error_code ec;
    const auto stamp = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path(ec) / format("go-cooker-gpu-check-%llu", stamp);
    fs::create_directories(dir, ec);
    if (ec) {
        return {"Texconv BC7: FAILED (" + ec.message() + ")"};
    }
    const fs::path input = dir / "gpu-check.tga";
    {
        // Uncompressed 4x4 32-bit TGA, top-left origin, BGRA (64, 96, 128, 255).
        std::vector<unsigned char> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 0, 32, 0x28};
        for (int i = 0; i < 16; ++i) {
            tga.insert(tga.end(), {64, 96, 128, 255});
        }
        std::ofstream file(input, std::ios::binary);
        file.write(reinterpret_cast<const char*>(tga.data()), std::streamsize(tga.size()));
        if (!file) {
            fs::remove_all(dir, ec);
            return {"Texconv BC7: FAILED (cannot write test image)"};
        }
    }

    int configuredGPU;
    {
        std::lock_guard lock(app.mu);
        configuredGPU = app.gpuAdapter;
    }
    std::vector<int> indices = configuredGPU >= 0 ? std::vector<int>{configuredGPU} : gpuAdapterCandidates();
    if (indices.empty()) {
        indices = {-1};
    }

    std::vector<std::string> result;
    std::string lastError;
    for (int index : indices) {
        std::vector<std::string> args = {"-y", "-f", "BC7_UNORM", "-bc", "x"};
        if (index >= 0) {
            args.insert(args.end(), {"-gpu", std::to_string(index)});
        }
        args.insert(args.end(), {"-o", u8str(dir), "--", u8str(input)});
        lastError = texconvRun(args);
        if (lastError.empty()) {
#if defined(_WIN32)
            setSelectedGPU(index);
            result = {format("Texconv BC7 GPU: OK (adapter %d: %s)", index, gpuAdapterName(index).c_str())};
#else
            result = {"Texconv BC7: OK"};
#endif
            break;
        }
    }
    fs::remove_all(dir, ec);
    if (result.empty()) {
        result = {"Texconv BC7 GPU: FAILED", "  " + lastError};
    }
    return result;
}

static std::vector<std::string> runGPUCheck()
{
    std::vector<std::string> report = platformGPUInfo();
    report.push_back("Texconv version: " + texconvVersion());
    for (const std::string& line : checkTexconvGPU()) {
        report.push_back(line);
    }

    std::vector<std::string> devices;
    const std::string deviceError = vulkanDevices(devices);
    int selectedDevice;
    {
        std::lock_guard lock(app.mu);
        app.vulkanDevices = devices;
        selectedDevice = app.vulkanDevice;
    }
    report.push_back("Compressonator version: " + vulkanVersion());
    std::string contextError;
    if (!deviceError.empty()) {
        report.push_back("Compressonator Vulkan: FAILED (" + deviceError + ")");
    } else if (!(contextError = ensureVulkanContext(selectedDevice)).empty()) {
        report.push_back("Compressonator Vulkan: FAILED (" + contextError + ")");
    } else {
        std::string deviceName = vulkanContextDeviceName();
        if (deviceName.empty() && selectedDevice >= 0 && selectedDevice < int(devices.size())) {
            deviceName = devices[selectedDevice];
        }
        report.push_back("Compressonator Vulkan: OK (" + (deviceName.empty() ? "unknown" : deviceName) + ")");
    }
    return report;
}

void runStartupGPUCheck()
{
    {
        std::lock_guard lock(app.mu);
        app.gpuChecking = true;
    }
    if (std::string error = texconvInit(); !error.empty()) {
        std::lock_guard lock(app.mu);
        app.texconvWarn = error;
        app.warnShown = true;
    }
    std::vector<std::string> report = runGPUCheck();
    {
        std::lock_guard lock(app.mu);
        app.gpuReport = report;
        app.gpuChecking = false;
    }
    requestRedraw();
}

static void rerunGPUCheck()
{
    {
        std::lock_guard lock(app.mu);
        if (app.gpuChecking) {
            return;
        }
        app.gpuChecking = true;
        app.gpuReport = {"Testing selected GPU..."};
    }
    std::thread([] {
        std::vector<std::string> report = runGPUCheck();
        {
            std::lock_guard lock(app.mu);
            app.gpuReport = report;
            app.gpuChecking = false;
        }
        requestRedraw();
    }).detach();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void gpuControlsUI()
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
    ImGui::TextUnformatted("Texture backend:");
    ImGui::SameLine();
    if (comboStrings("##texture_backend", &backendIndex, backendOptions, px(320))) {
        {
            std::lock_guard lock(app.mu);
            app.textureBackend = backendIndex == 1 ? textureBackendTexconv : textureBackendCompressonator;
        }
        saveSettings();
    }

    if (backend == textureBackendCompressonator) {
        std::vector<std::string> vulkanOptions = {"Automatic (best)"};
        for (size_t index = 0; index < vulkanDevices.size(); ++index) {
            vulkanOptions.push_back(format("%zu: %s", index, vulkanDevices[index].c_str()));
        }
        int vulkanIndex = configuredVulkanDevice + 1;
        if (vulkanIndex < 0 || vulkanIndex >= int(vulkanOptions.size())) {
            vulkanIndex = 0;
        }
        ImGui::TextUnformatted("Compressonator GPU:");
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
        ImGui::TextUnformatted("Texconv GPU:");
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
            ImGui::TextUnformatted("Testing...");
        }
#else
        (void)configuredGPU;
        if (checking) {
            ImGui::TextUnformatted("Checking hardware...");
        }
#endif
    }

    if (report.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colTextDim);
    ImGui::TextUnformatted("System:");
    for (const std::string& line : report) {
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopStyleColor();
}

void cookTabUI()
{
    if (!ImGui::BeginTabItem("Texture Cooker")) {
        return;
    }
    CookTab& c = cookTab;
    ImGui::TextUnformatted("Textures directory:");
    ImGui::InputText("##cook_directory", &c.directory);
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        openDirPicker(&c.directory);
    }

    ImGui::Separator();
    ImGui::Checkbox("Assume .data textures are grayscale (BC4)", &c.autoDetectFmt);
    tooltip("Data textures will be cooked as grayscale to reduce memory usage regardless of input format mode.");
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
        if (ImGui::Button("Stop Cooker")) {
            c.task.stop = true;
        }
    } else if (ImGui::Button("Run Texture Cooker") && !c.directory.empty()) {
        c.task.log("");
        std::thread(runCooker, c.directory, c.autoDetectFmt).detach();
    }
    ImGui::Text("Status: %s", status.empty() ? "Waiting..." : status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Output:");
    if (ImGui::BeginChild("##cook_log", ImVec2(0, 280), ImGuiChildFlags_Borders)) {
        for (const std::string& line : logLines) {
            const std::string lower = toLower(line);
            ImVec4 col = colText;
            if (hasPrefix(lower, "  ok:")) {
                col = colCookOk;
            } else if (hasPrefix(lower, "  fail:") || hasPrefix(lower, "error")) {
                col = colCookErr;
            } else if (hasPrefix(lower, "  skip:")) {
                col = colTextDim;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    gpuControlsUI();
    ImGui::EndTabItem();
}
