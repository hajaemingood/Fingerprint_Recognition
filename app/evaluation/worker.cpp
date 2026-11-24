#include "worker.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <new>
#include <stdexcept>

namespace evaluation {
namespace {

std::runtime_error windowsError(const char* operation)
{
    return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

std::wstring quote(const std::wstring& value)
{
    const auto last = value.find_last_not_of(L'\\');
    const auto trailing = last == std::wstring::npos ? value.size() : value.size() - last - 1;
    return L"\"" + value + std::wstring(trailing, L'\\') + L"\"";
}

void copyPath(std::array<wchar_t, 32768>& target, const fs::path& path)
{
    const auto& name = path.native();
    if (name.size() >= target.size()) {
        throw std::runtime_error("Image path is too long");
    }
    std::copy(name.begin(), name.end(), target.begin());
    target[name.size()] = L'\0';
}

} // namespace

Handle workerEvent(const std::wstring& name, bool create)
{
    Handle event(create ? CreateEventW(nullptr, FALSE, FALSE, name.c_str())
                        : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name.c_str()));
    if (!event) {
        throw windowsError("Open worker event");
    }
    return event;
}

Handle workerMapping(const std::wstring& name, bool create)
{
    Handle mapping(create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                              0, sizeof(Exchange), name.c_str())
                          : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str()));
    if (!mapping) {
        throw windowsError("Open worker shared memory");
    }
    return mapping;
}

ExchangeView mapExchange(HANDLE mapping)
{
    ExchangeView result(static_cast<Exchange*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Exchange))));
    if (!result) {
        throw windowsError("Map worker shared memory");
    }
    return result;
}

fs::path workerExecutable(const fs::path& dll)
{
    std::ifstream file(dll, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
        throw std::runtime_error("Select a valid Windows DLL");
    }
    file.seekg(dos.e_lfanew);
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || signature != IMAGE_NT_SIGNATURE || !(header.Characteristics & IMAGE_FILE_DLL)) {
        throw std::runtime_error("Select a valid Windows DLL");
    }
    const wchar_t* filename = nullptr;
    switch (header.Machine) {
    case IMAGE_FILE_MACHINE_AMD64: filename = L"fingerprint_worker_x64.exe"; break;
    case IMAGE_FILE_MACHINE_I386: filename = L"fingerprint_worker_x86.exe"; break;
    case IMAGE_FILE_MACHINE_ARM64: filename = L"fingerprint_worker_arm64.exe"; break;
    default: throw std::runtime_error("Unsupported DLL architecture");
    }
    std::wstring current(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, current.data(), static_cast<DWORD>(current.size()));
    if (length == 0 || length >= current.size()) {
        throw windowsError("Locate evaluator executable");
    }
    current.resize(length);
    const auto path = fs::path(current).parent_path() / filename;
    if (!fs::is_regular_file(path)) {
        throw std::runtime_error("This DLL requires " + path.filename().u8string() + " next to the evaluator");
    }
    return path;
}

WorkerProcess::WorkerProcess(const fs::path& exe, const Settings& settings, const fs::path& templates)
{
    static std::atomic<unsigned> sequence{0};
    const auto prefix = L"Local\\AdvancedOOP.Evaluation." + std::to_wstring(GetCurrentProcessId())
                      + L"." + std::to_wstring(++sequence);
    request_ = workerEvent(prefix + L".request", true);
    done_ = workerEvent(prefix + L".done", true);
    mapping_ = workerMapping(prefix + L".data", true);
    exchange_ = mapExchange(mapping_.get());
    new (exchange_.get()) Exchange{};

    auto command = quote(exe.native()) + L" " + quote(prefix) + L" " + quote(settings.dll.native())
                 + L" " + quote(settings.database.native()) + L" " + quote(templates.native());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, templates.c_str(), &startup, &process)) {
        throw windowsError("Start evaluation worker");
    }
    process_.reset(process.hProcess);
    CloseHandle(process.hThread);
}

WorkerProcess::~WorkerProcess()
{
    if (running()) {
        InterlockedExchange(&exchange_->shutdown, 1);
        if (SetEvent(request_.get()) && WaitForSingleObject(process_.get(), 500) == WAIT_OBJECT_0) {
            return;
        }
    }
    stop();
}

bool WorkerProcess::running() const
{
    return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
}

void WorkerProcess::stop()
{
    if (running()) {
        TerminateProcess(process_.get(), ERROR_CANCELLED);
        WaitForSingleObject(process_.get(), 5000);
    }
    process_.reset();
}

Outcome WorkerProcess::compare(const Trial& trial, HANDLE cancellation, DWORD timeoutMs)
{
    copyPath(exchange_->enrolled, trial.enrolled);
    copyPath(exchange_->probe, trial.probe);
    exchange_->outcome = {};
    InterlockedExchange(&exchange_->stage, static_cast<LONG>(Stage::Load));
    if (!SetEvent(request_.get())) {
        throw windowsError("Send work request");
    }

    const HANDLE handles[] = {cancellation, done_.get(), process_.get()};
    const auto wait = WaitForMultipleObjects(3, handles, FALSE, timeoutMs);
    if (wait == WAIT_OBJECT_0 + 1) {
        return exchange_->outcome;
    }
    const auto stage = static_cast<Stage>(InterlockedCompareExchange(&exchange_->stage, 0, 0));
    if (wait == WAIT_OBJECT_0 + 2) {
        DWORD code = 0;
        GetExitCodeProcess(process_.get(), &code);
        auto result = exchange_->outcome;
        result.status = Status::Crash;
        result.stage = stage;
        result.error = static_cast<int>(code);
        return result;
    }
    if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
        stop();
        auto result = exchange_->outcome;
        result.status = wait == WAIT_TIMEOUT ? Status::Timeout : Status::Cancelled;
        result.stage = stage;
        return result;
    }
    throw windowsError("Wait for evaluation worker");
}

const char* name(Status status)
{
    switch (status) {
    case Status::Pending: return "pending";
    case Status::Ok: return "ok";
    case Status::Failed: return "failed";
    case Status::InvalidOutput: return "invalid_output";
    case Status::Timeout: return "timeout";
    case Status::Crash: return "crash";
    case Status::Identical: return "identical_image";
    case Status::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* name(Stage stage)
{
    switch (stage) {
    case Stage::Load: return "load";
    case Stage::Input: return "input";
    case Stage::Enroll: return "enroll";
    case Stage::Attempt: return "attempt";
    case Stage::Compare: return "compare";
    }
    return "unknown";
}

} // namespace evaluation
