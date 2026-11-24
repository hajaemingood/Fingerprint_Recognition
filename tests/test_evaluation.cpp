#include "evaluation/run.h"
#include "tiff_image.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

namespace {

namespace fs = std::filesystem;
using namespace evaluation;

void require(bool condition, const char* failure)
{
    if (!condition) {
        throw std::runtime_error(failure);
    }
}

std::string read(const fs::path& path)
{
    std::ifstream stream(path);
    require(static_cast<bool>(stream), "Cannot read evaluation output");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void pairs(const fs::path& target, const std::vector<std::string>& probes)
{
    std::ofstream csv(target);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv << "\xef\xbb\xbf" "enrolled,probe\n";
    for (const auto& probe : probes) {
        csv << "101_1.tif," << probe << '\n';
    }
}

void image(const fs::path& folder, const char* name, unsigned char value)
{
    fvc::writeTiff(folder / name, {2, 300, 300, std::vector<unsigned char>(90000, value)});
}

void verifyPlan(const fs::path& root)
{
    Settings settings;
    settings.database = root;
    const auto all = createPlan(settings);
    require(all.size() == 9480, "Unexpected all-pair FVC count");
    require(std::count_if(all.begin(), all.end(), [](const auto& trial) { return trial.genuine; }) == 840,
            "Unexpected genuine FVC count");
    const auto repeated = createPlan(settings);
    require(std::equal(all.begin(), all.end(), repeated.begin(), [](const auto& first, const auto& second) {
        return first.enrolled == second.enrolled && first.probe == second.probe;
    }), "The same seed did not reproduce the plan");

    settings.database = root / "FVC2000_DB3_B";
    settings.enrollment = {Rule::First, 1};
    settings.genuine = {Rule::Random, 3};
    settings.impostor = {Rule::Impression, 2};
    const auto selected = createPlan(settings);
    require(selected.size() == 120, "Unexpected custom-rule pair count");
    require(std::all_of(selected.begin(), selected.end(), [](const auto& trial) {
        return trial.enrolled.stem().wstring().substr(4) == L"1"
            && (trial.genuine || trial.probe.stem().wstring().substr(4) == L"2");
    }), "Enrollment or impostor selection changed");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        require(argc == 3, "Provide the fixture DLL and FVC root");
        verifyPlan(argv[2]);

        const auto root = fs::current_path() / "evaluation-test"
            / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Settings settings;
        settings.dll = argv[1];
        settings.database = root / "DB";
        settings.templates = root / "templates";
        settings.logs = root / "logs";
        settings.reports = root / "reports";
        settings.latestReport = root / "published" / "fingerprint_evaluation.html";
        settings.pairFile = root / "pairs.csv";
        fs::create_directories(settings.database);
        fs::create_directories(settings.latestReport.parent_path());
        std::ofstream(settings.latestReport) << "Previous report";
        image(settings.database, "101_1.tif", 70);
        image(settings.database, "101_2.tif", 80);
        image(settings.database, "102_1.tif", 20);
        image(settings.database, "102_2.tif", 30);
        image(settings.database, "102_3.tif", 40);
        image(settings.database, "102_4.tif", 50);
        image(settings.database, "103_1.tif", 70);
        image(settings.database, "104_1.tif", 90);

        pairs(settings.pairFile, {"101_1.tif"});
        bool rejected = false;
        try {
            createPlan(settings);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "A self-comparison was accepted");

        pairs(settings.pairFile, {"101_2.tif", "102_1.tif", "102_2.tif", "102_3.tif",
                                   "102_4.tif", "103_1.tif", "104_1.tif"});
        settings.workers = 2;
        settings.timeoutMilliseconds = 2000;
        settings.reportTiming = false;
        Handle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(static_cast<bool>(cancel), "Cannot create cancellation event");
        Progress progress;
        const auto result = run(settings, cancel.get(), progress);
        require(result.completed == 7 && result.failed == 4 && !result.cancelled, "Unexpected evaluation totals");
        const auto published = read(settings.latestReport);
        require(result.reportPublishError.empty() && published == read(result.report),
                "The latest report was not published");
        require(progress.finished == 7 && progress.total == 7, "The workers did not process the whole plan");
        const auto log = read(result.log);
        for (const char* status : {",timeout,attempt,", ",crash,attempt,", ",invalid_output,attempt,",
                                   ",invalid_output,compare,", ",identical_image,input,", ",ok,compare,0.75,"}) {
            require(log.find(status) != std::string::npos, status);
        }
        require(read(result.report).find("템플릿 크기 (평균") == std::string::npos, "Report selection was ignored");
        require(read(result.log.parent_path() / "plan.csv").find("enrolled,probe") == 0, "Missing saved plan");

        pairs(settings.pairFile, {"102_1.tif"});
        settings.timeoutMilliseconds = 30000;
        Progress interrupted;
        auto running = std::async(std::launch::async, [&]() { return run(settings, cancel.get(), interrupted); });
        Sleep(300);
        const auto start = std::chrono::steady_clock::now();
        SetEvent(cancel.get());
        const auto stopped = running.get();
        require(stopped.cancelled && read(stopped.log).find(",cancelled,") != std::string::npos,
                "Stopping did not record cancelled work");
        require(read(settings.latestReport) == published, "A cancelled evaluation replaced the latest report");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(5),
                "Stopping waited for the DLL call");
        std::cout << "Evaluation rules, worker isolation, cancellation, and reporting passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
