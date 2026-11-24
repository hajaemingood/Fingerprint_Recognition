#pragma once

#include "plan.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <memory>

namespace evaluation {

enum class Stage : LONG { Load, Input, Enroll, Attempt, Compare };
enum class Status { Pending, Ok, Failed, InvalidOutput, Timeout, Crash, Identical, Cancelled };

struct Outcome {
    Status status = Status::Pending;
    Stage stage = Stage::Load;
    int error = 0;
    char message[256]{};
    alignas(8) double score = 0.0;
    double enrollMs = 0.0;
    double attemptMs = 0.0;
    double compareMs = 0.0;
    unsigned templateSize = 0;
    bool enrolled = false;
    bool attempted = false;
    bool compared = false;
};

struct Exchange {
    std::array<wchar_t, 32768> enrolled;
    std::array<wchar_t, 32768> probe;
    volatile LONG stage;
    volatile LONG shutdown;
    Outcome outcome;
};
static_assert(sizeof(Outcome) == 312 && sizeof(Exchange) == 131392,
              "The worker and evaluator must share the same record layout");

struct CloseHandleDeleter {
    void operator()(void* value) const { CloseHandle(value); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

struct UnmapView {
    void operator()(Exchange* value) const { UnmapViewOfFile(value); }
};
using ExchangeView = std::unique_ptr<Exchange, UnmapView>;

Handle workerEvent(const std::wstring& name, bool create);
Handle workerMapping(const std::wstring& name, bool create);
ExchangeView mapExchange(HANDLE mapping);
fs::path workerExecutable(const fs::path& dll);

class WorkerProcess {
public:
    WorkerProcess(const fs::path& exe, const Settings& settings, const fs::path& templates);
    ~WorkerProcess();
    Outcome compare(const Trial& trial, HANDLE cancellation, DWORD timeoutMs);
    bool running() const;

private:
    void stop();

    Handle process_;
    Handle request_;
    Handle done_;
    Handle mapping_;
    ExchangeView exchange_;
};

const char* name(Status status);
const char* name(Stage stage);

} // namespace evaluation
