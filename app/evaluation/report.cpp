#include "report.h"
#include "verification_metrics.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>

namespace evaluation {
namespace {

std::ofstream output(const fs::path& path)
{
    std::ofstream stream(path);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    return stream;
}

std::string html(std::string_view value)
{
    std::string escaped;
    for (const char character : value) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

void csv(std::ostream& out, const char* text)
{
    out << '"';
    for (std::size_t i = 0; i < 256 && text[i] != '\0'; ++i) {
        if (text[i] == '"') {
            out << '"';
        }
        out << text[i];
    }
    out << '"';
}

void ratio(std::ostream& out, double value, std::size_t total, const char* unit = "")
{
    if (total == 0) {
        out << "—";
    } else {
        out << value / total << unit;
    }
}

struct Stats {
    VerificationCounts totals;
    VerificationCounts scored;
    VerificationCounts matched;
    std::vector<VerificationScore> scores;
    std::size_t enrollments = 0;
    std::size_t enrollFailed = 0;
    std::size_t timedEnrollments = 0;
    std::size_t attempts = 0;
    std::size_t comparisons = 0;
    std::size_t templates = 0;
    std::size_t totalBytes = 0;
    unsigned maxBytes = 0;
    double enrollMs = 0.0;
    double attemptMs = 0.0;
    double compareMs = 0.0;

    void add(const Trial& trial, const Outcome& outcome)
    {
        if (outcome.status == Status::Identical || outcome.status == Status::Cancelled) {
            return;
        }
        ++(trial.genuine ? totals.genuine : totals.impostor);
        if (outcome.enrolled) {
            ++enrollments;
            if (outcome.enrollMs > 0) {
                ++timedEnrollments;
                enrollMs += outcome.enrollMs;
            }
            if (outcome.templateSize == 0) {
                ++enrollFailed;
            } else {
                ++templates;
                totalBytes += outcome.templateSize;
                maxBytes = std::max(maxBytes, outcome.templateSize);
            }
        }
        if (outcome.attempted && outcome.attemptMs > 0) {
            ++attempts;
            attemptMs += outcome.attemptMs;
        }
        if (outcome.compared && outcome.compareMs > 0) {
            ++comparisons;
            compareMs += outcome.compareMs;
        }
        if (outcome.stage == Stage::Attempt || outcome.stage == Stage::Compare) {
            ++(trial.genuine ? matched.genuine : matched.impostor);
        }
        if (outcome.status == Status::Ok) {
            ++(trial.genuine ? scored.genuine : scored.impostor);
            scores.push_back({outcome.score, trial.genuine});
        }
    }
};

void accuracy(std::ostream& out, const Stats& stats)
{
    const auto point = equalErrorOperatingPoint(stats.scores, stats.totals);
    const auto successful = equalErrorOperatingPoint(stats.scores, stats.scored);
    out << "<tr><th>EER</th><td>";
    if (point) {
        out << point->averageError() * 100 << "%</td></tr><tr><th>임계값</th><td>"
            << std::setprecision(3) << point->threshold << std::setprecision(2)
            << "</td></tr><tr><th>FMR · FNMR</th><td>"
            << 100 * point->rates.fmr << "% · " << 100 * point->rates.fnmr << "%";
    } else {
        out << "—";
    }
    out << "</td></tr><tr><th>EER*</th><td>";
    if (successful) {
        out << successful->averageError() * 100 << "%";
    } else {
        out << "—";
    }
    out << "</td></tr>";
}

void failures(std::ostream& out, const Stats& stats)
{
    out << "<tr><th>등록 실패율 FTE</th><td>";
    ratio(out, 100.0 * stats.enrollFailed, stats.enrollments, "%");
    out << " (" << stats.enrollFailed << '/' << stats.enrollments << ")</td></tr>"
        << "<tr><th>매칭 실패율 REJ (본인 · 타인)</th><td>";
    ratio(out, 100.0 * (stats.matched.genuine - stats.scored.genuine), stats.matched.genuine, "%");
    out << " · ";
    ratio(out, 100.0 * (stats.matched.impostor - stats.scored.impostor), stats.matched.impostor, "%");
    out << "</td></tr>";
}

void timing(std::ostream& out, const Stats& stats)
{
    out << "<tr><th>평균 단계 시간 (Enroll · Attempt · Compare)</th><td>";
    ratio(out, stats.enrollMs, stats.timedEnrollments, " ms");
    out << " · ";
    ratio(out, stats.attemptMs, stats.attempts, " ms");
    out << " · ";
    ratio(out, stats.compareMs, stats.comparisons, " ms");
    out << "</td></tr><tr><th>등록 템플릿 크기 (평균 · 최대)</th><td>";
    ratio(out, static_cast<double>(stats.totalBytes), stats.templates, " B");
    out << " · " << stats.maxBytes << " B</td></tr>";
}

} // namespace

void writePlan(const fs::path& path, const std::vector<Trial>& trials)
{
    auto out = output(path);
    out << "enrolled,probe\n";
    for (const auto& trial : trials) {
        out << trial.enrolled.generic_u8string() << ',' << trial.probe.generic_u8string() << '\n';
    }
    out.close();
}

void writeLog(const fs::path& path, const std::vector<Trial>& trials, const std::vector<Outcome>& outcomes)
{
    auto out = output(path);
    out << "sequence,database,enrolled,probe,genuine,status,stage,score,error,message,"
           "enroll_ms,attempt_ms,compare_ms,enroll_bytes\n" << std::setprecision(9);
    for (std::size_t i = 0; i < trials.size(); ++i) {
        const auto& trial = trials[i];
        const auto& result = outcomes[i];
        out << i + 1 << ',' << trial.database << ',' << trial.enrolled.generic_u8string() << ','
            << trial.probe.generic_u8string() << ',' << trial.genuine << ',' << name(result.status)
            << ',' << name(result.stage) << ',';
        if (result.status == Status::Ok) {
            out << result.score;
        }
        out << ',' << result.error << ',';
        csv(out, result.message);
        out << ',' << result.enrollMs << ',' << result.attemptMs << ',' << result.compareMs
            << ',' << result.templateSize << '\n';
    }
    out.close();
}

void writeReport(const RunResult& result, const Settings& settings, const fs::path& templates,
                 const std::vector<Trial>& trials, const std::vector<Outcome>& outcomes)
{
    std::map<std::string, Stats> byDatabase;
    std::map<Status, std::size_t> statuses;
    for (std::size_t i = 0; i < trials.size(); ++i) {
        byDatabase[trials[i].database].add(trials[i], outcomes[i]);
        ++statuses[outcomes[i].status];
    }

    auto out = output(result.report);
    out << "<!doctype html><html lang=\"ko\"><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<title>지문 인식 성능 평가</title><style>"
           "body{font:16px system-ui;color:#182434;background:#f5f7fa;margin:auto;padding:2rem;max-width:1000px}"
           "h1,h2{color:#123d60}p{overflow-wrap:anywhere}table{width:100%;background:white;border-collapse:collapse}"
           "th,td{text-align:left;padding:.65rem .8rem;border-bottom:1px solid #d9e1e8}"
           "th{width:55%}code{overflow-wrap:anywhere}</style>"
           "<h1>지문 인식 성능 평가</h1><p><strong>"
        << (result.cancelled ? "중단" : "완료") << "</strong> · 처리 " << result.completed << " / " << trials.size()
        << "쌍 · 성공 " << statuses[Status::Ok] << " · 실패 " << result.failed << " · 동일 이미지 제외 "
        << statuses[Status::Identical] << "</p><p>시간 초과 " << statuses[Status::Timeout]
        << " · 비정상 종료 " << statuses[Status::Crash] << " · 출력 오류 " << statuses[Status::InvalidOutput]
        << "</p><p>알고리즘 <code>" << html(settings.dll.u8string()) << "</code><br>DB <code>"
        << html(settings.database.u8string()) << "</code><br>워커 " << settings.workers
        << "개 · 타임아웃 " << settings.timeoutMilliseconds << " ms · 시드 " << settings.seed
        << "</p><p>로그 <code>" << html(result.log.u8string()) << "</code><br>템플릿 <code>"
        << html(templates.u8string()) << "</code></p>" << std::fixed << std::setprecision(2);
    for (const auto& entry : byDatabase) {
        const auto& stats = entry.second;
        out << "<h2>" << html(entry.first) << "</h2><table><tr><th>본인 · 타인 쌍</th><td>"
            << stats.totals.genuine << " · " << stats.totals.impostor << "</td></tr>";
        if (settings.reportAccuracy && !result.cancelled) {
            accuracy(out, stats);
        }
        if (settings.reportFailures) {
            failures(out, stats);
        }
        if (settings.reportTiming) {
            timing(out, stats);
        }
        out << "</table>";
    }
    out << "</html>\n";
    out.close();
}

void publishReport(const fs::path& from, const fs::path& to)
{
    fs::create_directories(to.parent_path());
    auto temporary = to;
    temporary += L"." + from.parent_path().filename().wstring() + L".tmp";

    try {
        fs::copy_file(from, temporary, fs::copy_options::overwrite_existing);
        fs::rename(temporary, to);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace evaluation
