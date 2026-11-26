#include "cache.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace research
{
namespace
{
using PairName = std::pair<std::string, std::string>;
using Scores = std::map<PairName, VerificationScore>;
using Conditions = std::map<std::string, Scores>;
constexpr char db[] = "FVC2004_DB1_B";
Conditions readScores(const fs::path &path)
{
    Conditions result;
    for (auto &row : csv(path))
    {
        if (row.at("database") != db || row.at("status") != "ok")
            throw std::runtime_error("Incomplete ROI evaluation");
        auto names = std::make_pair(basename(row.at("enrolled")), basename(row.at("probe_original")));
        if (!result[conditionName(row)]
                 .emplace(names, VerificationScore{std::stod(row.at("score")), row.at("genuine") == "1"})
                 .second)
            throw std::runtime_error("Duplicate ROI pair");
    }
    return result;
}
double atOnePercent(const std::vector<VerificationScore> &scores)
{
    std::vector<int> impostors;
    for (auto &s : scores)
        if (!s.genuine)
            impostors.push_back(std::lround(1000 * s.score));
    if (impostors.empty())
        throw std::runtime_error("No impostor scores");
    std::sort(impostors.rbegin(), impostors.rend());
    return (impostors[impostors.size() / 100] + 1) / 1000.;
}
Metrics at(const std::vector<VerificationScore> &values, double t)
{
    auto m = measure(values);
    auto rates = ratesAtThreshold(values, {size_t(m.genuine), size_t(m.impostor)}, t).value();
    m.threshold = t;
    m.fmr = rates.fmr;
    m.fnmr = rates.fnmr;
    return m;
}
} // namespace
void roiStudy(const fs::path &standard, const fs::path &roi, const fs::path &split, const fs::path &captures,
              const fs::path &out)
{
    std::map<unsigned, std::string> membership;
    for (auto &row : csv(split))
        if (row.at("database") == db)
            membership.emplace(std::stoul(row.at("finger")), row.at("split"));
    std::set<std::string> gallery, probes;
    for (auto &row : csv(captures))
        if (row.at("database") == db)
        {
            auto role = row.at("role");
            if (role == "train_and_enroll" || role == "unseen_enroll")
                gallery.insert(row.at("file"));
            if (role == "unseen_probe")
                probes.insert(row.at("file"));
        }
    const std::map<std::string, Conditions> models{{"roi", readScores(roi)},
                                                   {"standard", readScores(standard)}};
    const auto &reference = models.at("standard").at("original");
    std::map<std::string, std::vector<PairName>> groups;
    for (auto &[pair, value] : reference)
    {
        groups["all"].push_back(pair);
        auto g = membership.at(std::stoul(pair.first));
        if (g == membership.at(std::stoul(pair.second)))
            groups[g].push_back(pair);
        if (gallery.count(pair.first) && probes.count(pair.second))
            groups["last_id"].push_back(pair);
    }
    if (groups["all"].size() != 3160 || groups["last_id"].size() != 70 ||
        groups["development"].size() != 780 || groups["confirmation"].size() != 780)
        throw std::runtime_error("Wrong ROI split");
    auto subset = [&](const Scores &values, const std::string &key) {
        if (values.size() != reference.size())
            throw std::runtime_error("Missing ROI pairs");
        std::vector<VerificationScore> scores;
        for (auto &pair : groups.at(key))
        {
            auto value = values.at(pair);
            if (value.genuine != reference.at(pair).genuine)
                throw std::runtime_error("ROI label mismatch");
            scores.push_back(value);
        }
        return scores;
    };
    auto standardT = atOnePercent(subset(reference, "development"));
    std::ostringstream report, diagnostics;
    report << "database,scorer,group,scope,augmentation_settings,image_variants,genuine_pairs,impostor_pairs,"
              "eer,threshold,fmr,fnmr\n";
    diagnostics << "database,scorer,group,mode,genuine_pairs,impostor_pairs,eer,threshold,fmr,fnmr\n";
    const auto profile = settings(30);
    for (auto label : {"standard", "roi"})
    {
        auto &conditions = models.at(label);
        if (conditions.size() != 17)
            throw std::runtime_error("Expected original plus sixteen variants");
        auto t = atOnePercent(subset(conditions.at("original"), "development"));
        for (auto &[group, pairs] : groups)
        {
            auto original = subset(conditions.at("original"), group);
            std::string prefix = std::string(db) + "," + label + "," + group + ",";
            report << prefix << "original,0,1," << at(original, t).columns() << '\n';
            Metrics mean{};
            mean.threshold = t;
            for (auto &setting : profile)
                for (auto &name : setting)
                {
                    auto m = at(subset(conditions.at(name), group), t);
                    double weight = 1. / (setting.size() * profile.size());
                    mean.genuine += m.genuine;
                    mean.impostor += m.impostor;
                    mean.eer += weight * m.eer;
                    mean.fmr += weight * m.fmr;
                    mean.fnmr += weight * m.fnmr;
                }
            report << prefix << "augmentation_mean,11,16," << mean.columns() << '\n';
            if (group == "last_id" || group == "confirmation")
            {
                diagnostics << prefix << "standard_development_threshold,"
                            << at(original, standardT).columns() << '\n';
                diagnostics << prefix << "observed_fmr_cap," << at(original, atOnePercent(original)).columns()
                            << '\n';
            }
        }
    }
    fvc::createOutputDirectory(out);
    deepprint::writeNew(out / "evaluation.csv", report.str());
    deepprint::writeNew(out / "low_fmr_diagnostic.csv", diagnostics.str());
}
} // namespace research
