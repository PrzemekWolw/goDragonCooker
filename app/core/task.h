#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Wakes the UI loop from a background thread. The app installs the callback; headless tools leave it empty.
void setRedrawCallback(std::function<void()> callback);
void requestRedraw();

// Shared between the UI thread and one background run.
struct TaskState {
    std::mutex mu;
    std::string status;
    std::vector<std::string> logLines;
    bool running = false;
    std::atomic<bool> stop{false};

    void log(const std::string& msg);
    void setStatus(const std::string& status);
    void begin(const std::string& status);
    void finish(const std::string& status);
};

// Runs work(i) for i in [0, count) on up to `workers` threads, stopping early when the task is cancelled.
// record(i, error) is serialized.
void runPool(size_t count, unsigned workers, TaskState& task, const std::function<std::string(size_t)>& work,
             const std::function<void(size_t, const std::string&)>& record);
