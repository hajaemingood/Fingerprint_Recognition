#include "run.h"
#include "report.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>

namespace evaluation {
namespace {

fs::path absolutePath(const fs::path& path)
{
    if (path.empty()) {
        throw std::runtime_error("Choose the DLL, database and all three output folders");
    }
    return fs::weakly_canonical(fs::absolute(path));
}

std::string runId()
{
    static std::atomic<unsigned> sequence{0};
    const auto clock = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(clock).count();
    return "run-" + std::to_string(milliseconds) + "-" + std::to_string(GetCurrentProcessId())
         + "-" + std::to_string(++sequence);
}

bool cancelled(HANDLE event)
{
    return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

} // namespace

RunResult run(Settings settings, HANDLE cancellation, Progress& progress)
{
    settings.dll = absolutePath(settings.dll);
    settings.database = absolutePath(settings.database);
    settings.templates = absolutePath(settings.templates);
    settings.logs = absolutePath(settings.logs);
    settings.reports = absolutePath(settings.reports);
    if (!settings.latestReport.empty()) {
        settings.latestReport = absolutePath(settings.latestReport);
    }
    if (!settings.pairFile.empty()) {
        settings.pairFile = absolutePath(settings.pairFile);
    }
    if (settings.workers == 0 || settings.workers > 16 || settings.timeoutMilliseconds == 0) {
        throw std::runtime_error("Use 1 to 16 workers and a positive timeout");
    }

    const auto worker = workerExecutable(settings.dll);
    const auto trials = createPlan(settings);
    progress.total = trials.size();
    const auto id = runId();
    const auto templates = settings.templates / id;
    const auto logs = settings.logs / id;
    const auto reports = settings.reports / id;
    fs::create_directories(templates);
    fs::create_directories(logs);
    fs::create_directories(reports);
    writePlan(logs / "plan.csv", trials);

    std::vector<Outcome> outcomes(trials.size());
    std::atomic<std::size_t> next{0};
    std::mutex errorMutex;
    std::exception_ptr workerError;
    auto execute = [&](unsigned number) {
        try {
            auto folder = templates / ("worker-" + std::to_string(number));
            fs::create_directory(folder);
            std::unique_ptr<WorkerProcess> process;
            while (!cancelled(cancellation)) {
                const auto index = next.fetch_add(1);
                if (index >= trials.size()) {
                    break;
                }
                if (!process || !process->running()) {
                    process = std::make_unique<WorkerProcess>(worker, settings, folder);
                }
                outcomes[index] = process->compare(trials[index], cancellation, settings.timeoutMilliseconds);
                ++progress.finished;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (!workerError) {
                workerError = std::current_exception();
            }
            SetEvent(cancellation);
        }
    };

    std::vector<std::thread> threads;
    try {
        const auto count = std::min<std::size_t>(settings.workers, trials.size());
        for (unsigned i = 1; i <= count; ++i) {
            threads.emplace_back(execute, i);
        }
    } catch (...) {
        SetEvent(cancellation);
        for (auto& thread : threads) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : threads) {
        thread.join();
    }

    RunResult result;
    result.log = logs / "pairs.csv";
    result.report = reports / "report.html";
    for (auto& outcome : outcomes) {
        if (outcome.status == Status::Pending) {
            outcome.status = Status::Cancelled;
        }
        if (outcome.status != Status::Cancelled) {
            ++result.completed;
            if (outcome.status != Status::Ok && outcome.status != Status::Identical) {
                ++result.failed;
            }
        }
    }
    result.cancelled = result.completed != trials.size();
    writeLog(result.log, trials, outcomes);
    writeReport(result, settings, templates, trials, outcomes);
    if (workerError) {
        std::rethrow_exception(workerError);
    }
    if (!result.cancelled && !settings.latestReport.empty()) {
        try {
            publishReport(result.report, settings.latestReport);
        } catch (const std::exception& error) {
            result.reportPublishError = error.what();
        }
    }
    return result;
}

} // namespace evaluation
