#include "core/task.h"

#include <algorithm>
#include <thread>

static std::function<void()> redrawCallback;

void setRedrawCallback(std::function<void()> callback)
{
    redrawCallback = std::move(callback);
}

void requestRedraw()
{
    if (redrawCallback) {
        redrawCallback();
    }
}

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
        logLines.clear();
    }
    requestRedraw();
}

void startTask(TaskState& task, const std::string& status, std::function<void()> work)
{
    task.begin(status);
    std::thread(std::move(work)).detach();
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

void runPool(size_t count, unsigned workers, TaskState& task, const std::function<std::string(size_t)>& work,
             const std::function<void(size_t, const std::string&)>& record)
{
    std::atomic<size_t> next{0};
    std::mutex recordMu;
    std::vector<std::thread> threads;
    workers = std::max(1u, std::min<unsigned>(workers, unsigned(count)));
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&] {
            while (!task.stop) {
                const size_t index = next++;
                if (index >= count) {
                    return;
                }
                const std::string error = work(index);
                std::lock_guard lock(recordMu);
                record(index, error);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}
