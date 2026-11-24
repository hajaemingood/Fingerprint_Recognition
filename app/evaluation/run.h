#pragma once

#include "worker.h"

#include <atomic>

namespace evaluation {

struct Progress {
    std::atomic<std::size_t> total{0};
    std::atomic<std::size_t> finished{0};
};

struct RunResult {
    fs::path log;
    fs::path report;
    std::string reportPublishError;
    std::size_t completed = 0;
    std::size_t failed = 0;
    bool cancelled = false;
};

RunResult run(Settings settings, HANDLE cancellation, Progress& progress);

} // namespace evaluation
