#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>

struct FingerprintImage
{
    // Shared image-format ID; pixels are top-down, tightly packed 8-bit grayscale.
    int type = 0;
    std::vector<unsigned char> pixels;
};

// The caller initializes COM on the current thread.
// Success replaces image; failure leaves it unchanged and supplies an error.
bool loadFingerprintImage(const wchar_t *path, FingerprintImage &image, std::wstring &error);

// Demo data flow: DB images -> rule-generated pairs -> DLL scores -> graph data.
// The app treats DLL templates as opaque bytes. Algorithm code stays in src/.
namespace demo
{
namespace fs = std::filesystem;
enum class RegistrationRule
{
    First,
    Random,
    Impression,
    All
};
enum class ComparisonRule
{
    All,
    First,
    Random,
    Impression
};
struct Settings
{
    fs::path database;
    RegistrationRule registration = RegistrationRule::First;
    ComparisonRule genuine = ComparisonRule::All;
    ComparisonRule impostor = ComparisonRule::First;
    int registrationIndex = 1, genuineIndex = 2, impostorIndex = 1;
    int randomCount = 3;
    unsigned seed = 2026;
    int parallelism = 2;
};
struct ImageFile
{
    fs::path path;
    int finger = 0, impression = 0;
};
struct Pair
{
    size_t registration = 0, attempt = 0;
    bool genuine = false;
};
struct Plan
{
    Settings settings;
    std::vector<ImageFile> images;
    std::vector<Pair> pairs;
    size_t fingers = 0, registrations = 0;
    // Nonempty for an exact list imported from CSV; only these rows are evaluated.
    fs::path sourceFile;
};
enum class Status
{
    Pending,
    Success,
    EnrollmentError,
    AttemptError,
    CompareError,
    ImageError,
    Cancelled,
    Running
};
struct Result
{
    Status status = Status::Pending;
    double score = 0, milliseconds = 0;
    int worker = 0;
    size_t completionOrder = 0;
    std::wstring detail;
};
struct Statistics
{
    size_t genuine = 0, impostor = 0, genuineOk = 0, impostorOk = 0, completed = 0, failed = 0;
    double genuineMean = std::numeric_limits<double>::quiet_NaN();
    double impostorMean = std::numeric_limits<double>::quiet_NaN();
    double fmr = std::numeric_limits<double>::quiet_NaN(), fnmr = fmr;
    double eer = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> genuineScores, impostorScores;
    // threshold, FMR, FNMR; nonfinite values denote a missing comparison class.
    struct Point
    {
        double threshold, fmr, fnmr;
    };
    std::vector<Point> curve;
};
Plan buildPlan(const Settings &settings);
void savePlan(const Plan &plan, const fs::path &file);
Plan loadPlan(const fs::path &file, const Settings &fallback);
// Results keep their original row indices even when parallel jobs finish out of order.
// onResult is serialized, so clients do not need to make the callback itself reentrant.
struct EvaluationTiming
{
    double totalMilliseconds = 0, preparationMilliseconds = 0, matchingMilliseconds = 0;
};
EvaluationTiming evaluate(const Plan &plan, std::atomic<bool> &cancel,
                          const std::function<void(size_t, const Result &)> &onResult,
                          const std::function<void(size_t, int)> &onStarted = {});
Statistics summarize(const Plan &plan, const std::vector<Result> &results, double threshold);
} // namespace demo
