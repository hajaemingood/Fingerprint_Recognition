#include "worker.h"
#include "kisa_library.h"
#include "tiff_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>

namespace evaluation {
namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
using Cache = std::map<fs::path, Bytes>;
constexpr std::size_t templateCapacity = 1000000;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void message(Outcome& outcome, const char* text)
{
    std::snprintf(outcome.message, sizeof(outcome.message), "%s", text);
}

class AlgorithmWorker {
public:
    AlgorithmWorker(fs::path dll, fs::path database, fs::path output)
        : dllPath_(std::move(dll)), database_(std::move(database)), output_(std::move(output)) {}

    void compare(Exchange& exchange)
    {
        auto& outcome = exchange.outcome;
        try {
            if (!dll_) {
                enter(exchange, Stage::Load);
                dll_ = std::make_unique<KisaLibrary>(dllPath_);
            }
            enter(exchange, Stage::Input);
            const fs::path enrolled(exchange.enrolled.data());
            const fs::path probe(exchange.probe.data());
            const auto& first = image(enrolled);
            const auto& second = image(probe);
            if (first.type != second.type) {
                throw std::runtime_error("A pair must have the same image format");
            }
            if (first.pixels == second.pixels) {
                outcome.status = Status::Identical;
                return;
            }
            const auto* registration = getTemplate(exchange, enrolled, first, Stage::Enroll);
            if (!registration) {
                return;
            }
            const auto* comparison = getTemplate(exchange, probe, second, Stage::Attempt);
            if (!comparison) {
                return;
            }

            enter(exchange, Stage::Compare);
            std::fill(enrollBuffer_.begin(), enrollBuffer_.end(), 0);
            std::fill(attemptBuffer_.begin(), attemptBuffer_.end(), 0);
            std::copy(registration->begin(), registration->end(), enrollBuffer_.begin());
            std::copy(comparison->begin(), comparison->end(), attemptBuffer_.begin());
            outcome.score = std::numeric_limits<double>::quiet_NaN();
            outcome.compared = true;
            const auto start = Clock::now();
            const int status = dll_->compare(enrollBuffer_.data(), attemptBuffer_.data(), &outcome.score,
                                             &outcome.error, outcome.message);
            outcome.compareMs = elapsed(start);
            if (!returnedOk(outcome, status)) {
                return;
            }
            const double score = outcome.score;
            if (!std::isfinite(score) || score < 0 || score > 1
                || std::abs(score * 1000 - std::round(score * 1000)) > 1e-7) {
                outcome.status = Status::InvalidOutput;
                message(outcome, "DLL returned success with a missing or invalid score");
                return;
            }
            outcome.status = Status::Ok;
        } catch (const std::exception& error) {
            outcome.status = Status::Failed;
            message(outcome, error.what());
        }
    }

private:
    static void enter(Exchange& exchange, Stage stage)
    {
        exchange.outcome.stage = stage;
        exchange.outcome.error = 0;
        exchange.outcome.message[0] = '\0';
        InterlockedExchange(&exchange.stage, static_cast<LONG>(stage));
    }

    static bool returnedOk(Outcome& outcome, int status)
    {
        outcome.message[sizeof(outcome.message) - 1] = '\0';
        if (status == 0) {
            outcome.status = Status::Failed;
            return false;
        }
        if (status != 1 || outcome.error != 0) {
            outcome.status = Status::InvalidOutput;
            message(outcome, "DLL return code and error output are inconsistent");
            return false;
        }
        return true;
    }

    const fvc::GrayImage& image(const fs::path& relative)
    {
        const auto cached = images_.find(relative);
        if (cached != images_.end()) {
            return cached->second;
        }
        return images_.emplace(relative, fvc::readTiff(database_ / relative)).first->second;
    }

    const Bytes* getTemplate(Exchange& exchange, const fs::path& relative, const fvc::GrayImage& image, Stage stage)
    {
        const bool enroll = stage == Stage::Enroll;
        auto& cache = enroll ? registered_ : probes_;
        const auto found = cache.find(relative);
        if (found != cache.end()) {
            return &found->second;
        }
        enter(exchange, stage);
        auto& outcome = exchange.outcome;
        auto pixels = image.pixels;
        auto& buffer = enroll ? enrollBuffer_ : attemptBuffer_;
        std::fill(buffer.begin(), buffer.end(), 0);
        (enroll ? outcome.enrolled : outcome.attempted) = true;
        int size = 0;
        const auto start = Clock::now();
        const int status = (enroll ? dll_->enroll : dll_->attempt)(
            image.type, pixels.data(), buffer.data(), &size, &outcome.error, outcome.message);
        (enroll ? outcome.enrollMs : outcome.attemptMs) = elapsed(start);
        if (!returnedOk(outcome, status)) {
            return nullptr;
        }
        if (size <= 0 || static_cast<std::size_t>(size) >= templateCapacity) {
            outcome.status = Status::InvalidOutput;
            message(outcome, "DLL returned success with an invalid template size");
            return nullptr;
        }
        const auto path = output_ / (enroll ? "enroll" : "attempt") / relative;
        auto filename = path;
        filename += ".bin";
        auto temporary = filename;
        temporary += ".tmp";
        fs::create_directories(path.parent_path());
        std::ofstream stream(temporary, std::ios::binary);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(reinterpret_cast<const char*>(buffer.data()), size);
        stream.close();
        fs::rename(temporary, filename);
        if (enroll) {
            outcome.templateSize = static_cast<unsigned>(size);
        }
        return &cache.emplace(relative, Bytes(buffer.begin(), buffer.begin() + size)).first->second;
    }

    fs::path dllPath_;
    fs::path database_;
    fs::path output_;
    std::unique_ptr<KisaLibrary> dll_;
    std::map<fs::path, fvc::GrayImage> images_;
    Cache registered_;
    Cache probes_;
    Bytes enrollBuffer_ = Bytes(templateCapacity);
    Bytes attemptBuffer_ = Bytes(templateCapacity);
};

} // namespace
} // namespace evaluation

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        const std::wstring prefix = argv[1];
        const auto request = evaluation::workerEvent(prefix + L".request", false);
        const auto done = evaluation::workerEvent(prefix + L".done", false);
        const auto mapping = evaluation::workerMapping(prefix + L".data", false);
        const auto exchange = evaluation::mapExchange(mapping.get());
        evaluation::AlgorithmWorker worker(argv[2], argv[3], argv[4]);
        while (WaitForSingleObject(request.get(), INFINITE) == WAIT_OBJECT_0) {
            if (InterlockedCompareExchange(&exchange->shutdown, 0, 0) != 0) {
                return 0;
            }
            worker.compare(*exchange);
            if (!SetEvent(done.get())) {
                return 1;
            }
        }
    } catch (...) {
        return 1;
    }
    return 1;
}
