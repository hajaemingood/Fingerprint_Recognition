#include "cache.h"
#include "studies.h"
#include "training.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

namespace research
{
namespace
{
using Lists = std::map<std::string, std::vector<I>>;
using PairLists = std::map<std::string, std::vector<Pair>>;
using Results = std::map<std::string, std::map<std::string, std::vector<VerificationScore>>>;
using Conditions = std::vector<std::vector<std::string>>;
struct Fold
{
    Lists train;
    PairLists pairs;
};
struct Candidate
{
    Config config;
    double score = 2, fnmr = 2, threshold = 0;
    int epoch = 1;
};
struct Experiment
{
    FoldBanks banks;
    std::map<std::string, std::vector<std::string>> names;
    Conditions profile;
    std::vector<Fold> folds;
    Lists refit;
    std::map<std::string, PairLists> final;
};
std::vector<Pair> cartesian(const FoldBank &b, const std::vector<I> &gallery, const std::vector<I> &probes,
                            bool triangular)
{
    std::vector<Pair> result;
    for (auto i : gallery)
        for (auto j : probes)
        {
            if (triangular && i >= j)
                continue;
            if (i == j)
                throw std::runtime_error("Enrollment/probe overlap");
            result.push_back({size_t(i), size_t(j), b.fingers[i] == b.fingers[j]});
        }
    return result;
}
Experiment setup(const std::string &study, const fs::path &cache, const fs::path &split,
                 const fs::path &captures, const fs::path &heldFolds)
{
    Experiment e;
    const bool heads = study == "heads", unseen = study == "unseen";
    e.profile = settings(heads ? 40 : 30);
    auto ids = csv(split), roles = csv(captures);
    auto folds = unseen ? csv(heldFolds) : std::vector<Row>{};
    e.folds.resize(heads ? 5 : unseen ? 2 : 1);
    for (std::string db : {"FVC2002_DB2_B", "FVC2004_DB1_B"})
    {
        auto &b = e.banks[db];
        std::vector<std::string> conditions{"original"};
        for (auto &group : e.profile)
            conditions.insert(conditions.end(), group.begin(), group.end());
        for (auto &key : conditions)
        {
            auto item = readCache(cache / db / "standard" / (key + ".npz"));
            if (key == "original")
                e.names[db] = item.names;
            if (item.names != e.names.at(db))
                throw std::runtime_error("Inconsistent cache names");
            std::vector<float> values(item.vectors.size() * 512);
            std::memcpy(values.data(), item.vectors.data(), values.size() * 4);
            b.images[key].embed = studytorch::tensor(values, {I(item.names.size()), 512});
        }
        auto &names = e.names.at(db);
        for (auto &name : names)
            b.fingers.push_back(std::stoul(name));
        if (names.size() != 80)
            throw std::runtime_error("Expected 80 captures in " + db);
        if (heads)
        {
            std::map<unsigned, std::string> labels;
            std::vector<unsigned> dev;
            for (auto &r : ids)
                if (r.at("database") == db)
                {
                    unsigned id = std::stoul(r.at("finger"));
                    labels.emplace(id, r.at("split"));
                    if (r.at("split") == "development")
                        dev.push_back(id);
                }
            std::sort(dev.begin(), dev.end());
            if (dev.size() != 5 || labels.size() != 10)
                throw std::runtime_error("Wrong development split");
            std::vector<I> confirm;
            for (auto i : allIndices(names.size()))
                (labels.at(b.fingers[i]) == "development" ? e.refit[db] : confirm).push_back(i);
            e.final["confirmation"][db] = cartesian(b, confirm, confirm, true);
            for (size_t f = 0; f < 5; ++f)
            {
                std::vector<I> val;
                for (auto i : e.refit[db])
                    (b.fingers[i] == dev[f] || b.fingers[i] == dev[(f + 1) % 5] ? val : e.folds[f].train[db])
                        .push_back(i);
                e.folds[f].pairs[db] = cartesian(b, val, val, true);
            }
        }
        else
        {
            std::map<std::string, Row> labels;
            for (auto &r : roles)
                if (r.at("database") == db)
                    labels.emplace(r.at("file"), r);
            std::map<std::string, std::vector<I>> byRole;
            std::vector<I> inner;
            for (auto i : allIndices(names.size()))
            {
                auto &r = labels.at(names[i]);
                if (r.at("training") == "1")
                    e.refit[db].push_back(i);
                if (r.at("inner_training") == "1")
                    inner.push_back(i);
                byRole[r.at("role")].push_back(i);
            }
            if (inner.size() != 45 || e.refit[db].size() != 54 || byRole["unseen_probe"].size() != 7)
                throw std::runtime_error("Invalid held-ID protocol");
            auto gallery = byRole.at("train_and_enroll");
            e.final["seen"][db] = cartesian(b, gallery, byRole.at("seen_probe"), false);
            auto allGallery = gallery;
            allGallery.push_back(byRole.at("unseen_enroll").at(0));
            e.final["unseen"][db] = cartesian(b, allGallery, byRole.at("unseen_probe"), false);
            if (!unseen)
            {
                e.folds[0].train[db] = inner;
                e.folds[0].pairs[db] = cartesian(b, inner, byRole.at("development_probe"), false);
            }
            else
                for (auto &r : folds)
                    if (r.at("database") == db)
                    {
                        auto &fold = e.folds.at(std::stoul(r.at("fold")) - 1);
                        std::istringstream stream(r.at("validation_ids"));
                        std::string token;
                        std::set<unsigned> held;
                        while (std::getline(stream, token, '|'))
                            held.insert(std::stoul(token));
                        std::vector<I> probes;
                        for (auto i : e.refit[db])
                        {
                            if (!held.count(b.fingers[i]))
                                fold.train[db].push_back(i);
                            else if (std::find(gallery.begin(), gallery.end(), i) == gallery.end())
                                probes.push_back(i);
                        }
                        fold.pairs[db] = cartesian(b, gallery, probes, false);
                    }
        }
    }
    return e;
}
Results evaluate(const Experiment &e, const PairLists &pairs, const Tail *model, const Config &c,
                 const Conditions &settings)
{
    Results result;
    for (auto &[db, b] : e.banks)
    {
        std::set<I> ids;
        for (auto p : pairs.at(db))
        {
            ids.insert(p.a);
            ids.insert(p.b);
        }
        std::vector<I> selected(ids.begin(), ids.end());
        auto project = [&](const std::string &condition) {
            auto &in = b.images.at(condition);
            if (model)
                return embeddings(*model, in, selected, c, b.fingers.size());
            auto flat = in.embed.values();
            Vectors v(b.fingers.size());
            std::memcpy(v.data(), flat.data(), flat.size() * 4);
            return v;
        };
        auto gallery = project("original");
        result[db]["original"] = compare(gallery, gallery, pairs.at(db), c.weight);
        for (auto &group : settings)
            for (auto &condition : group)
                result[db][condition] = compare(gallery, project(condition), pairs.at(db), c.weight);
    }
    return result;
}
double threshold(const std::vector<Results> &folds)
{
    double t = 0;
    for (auto &[db, unused] : folds.at(0))
    {
        std::vector<int> scores;
        for (auto &fold : folds)
            for (auto s : fold.at(db).at("original"))
                if (!s.genuine)
                    scores.push_back(std::lround(1000 * s.score));
        std::sort(scores.rbegin(), scores.rend());
        t = std::max(t, (scores.at(scores.size() / 100) + 1) / 1000.);
    }
    return t;
}
Metrics rates(const std::vector<VerificationScore> &scores, double t)
{
    auto m = measure(scores);
    auto r = ratesAtThreshold(scores, {size_t(m.genuine), size_t(m.impostor)}, t).value();
    m.threshold = t;
    m.fmr = r.fmr;
    m.fnmr = r.fnmr;
    return m;
}
Metrics average(const std::map<std::string, std::vector<VerificationScore>> &scores,
                const Conditions &profile, double t)
{
    Metrics avg{};
    avg.threshold = t;
    for (auto &group : profile)
        for (auto &key : group)
        {
            auto m = rates(scores.at(key), t);
            double w = 1. / (profile.size() * group.size());
            avg.eer += w * m.eer;
            avg.fnmr += w * m.fnmr;
            avg.fmr += w * m.fmr;
            avg.genuine += m.genuine;
            avg.impostor += m.impostor;
        }
    return avg;
}
std::pair<double, double> assess(const Results &result, const Conditions &profile, double t)
{
    double sum = 0, fnmr = 0;
    for (auto &[db, scores] : result)
    {
        auto original = rates(scores.at("original"), t);
        sum += (original.eer + average(scores, profile, t).eer) * .5 / result.size();
        fnmr += original.fnmr / result.size();
    }
    return {sum, fnmr};
}
void assign(Experiment &e, const Lists &ids)
{
    for (auto &[db, b] : e.banks)
        b.train = ids.at(db);
}
} // namespace

void legacyStudy(const std::string &study, const fs::path &model, const fs::path &cache, const fs::path &ids,
                 const fs::path &captures, const fs::path &folds, const fs::path &out, int epochs)
{
    auto digest = deepprint::sha256(model);
    if (fs::is_regular_file(cache / "source.txt"))
    {
        if (deepprint::readFile(cache / "source.txt").substr(0, 64) != digest)
            throw std::runtime_error("Cache/model SHA-256 mismatch");
    }
    else
    {
        auto text = deepprint::readFile(cache / "source.json");
        if (!std::regex_search(text, std::regex("\"model_sha256\"\\s*:\\s*\"" + digest + "\"")))
            throw std::runtime_error("Cache/model SHA-256 mismatch");
    }
    auto e = setup(study, cache, ids, captures, folds);
    const bool heads = study == "heads", unseen = study == "unseen";
    Conditions objective = heads ? Conditions{{"noise-10-s42"},
                                              {"noise-40-s42"},
                                              {"noise-40-s43"},
                                              {"rotation+10"},
                                              {"brightness-15"}}
                                 : e.profile;
    fvc::createOutputDirectory(out);
    Weights w(model);
    Config base;
    base.rank = 4;
    base.rate = .003;
    base.weight = heads ? .22 : .5;
    std::ostringstream dev, final;
    dev << "arm,training_augmentation,strength,epochs,objective,original_fnmr,threshold\n";
    final << "group,scorer,database,scope,genuine_pairs,impostor_pairs,eer,threshold,fmr,fnmr\n";
    auto describe = [&](const Candidate &c) {
        dev << c.config.arm << ',' << c.config.mode << ',' << c.config.strength << ',' << c.epoch << ','
            << c.score << ',' << c.fnmr << ',' << c.threshold << '\n';
    };
    std::vector<Results> ref;
    for (auto &f : e.folds)
        ref.push_back(evaluate(e, f.pairs, nullptr, base, objective));
    Candidate baseline;
    baseline.config = base;
    baseline.config.arm = "frozen";
    baseline.threshold = threshold(ref);
    baseline.epoch = 0;
    baseline.score = baseline.fnmr = 0;
    for (auto &r : ref)
    {
        auto s = assess(r, objective, baseline.threshold);
        baseline.score += s.first / ref.size();
        baseline.fnmr += s.second / ref.size();
    }
    describe(baseline);
    std::vector<Candidate> trials;
    auto run = [&](Config c, const std::vector<double> &strengths, const std::vector<int> &milestones) {
        std::map<std::pair<double, int>, std::vector<Results>> byStep;
        std::vector<int> bestEpoch;
        std::vector<Results> chosen;
        for (size_t f = 0; f < e.folds.size(); ++f)
        {
            assign(e, e.folds[f].train);
            int seed = 42 + int(f) + (unseen ? 1 : 0);
            std::mt19937 rng(seed);
            Tail tail(w, c, rng);
            c.steps = 4 * milestones.back();
            double best = 2;
            int epoch = 0;
            Results result;
            fit(tail, c, e.banks, seed, 48, [&](int step, const Tail &net) {
                if (step % 4 || std::find(milestones.begin(), milestones.end(), step / 4) == milestones.end())
                    return;
                for (auto strength : strengths)
                {
                    Config cc = c;
                    cc.strength = strength;
                    auto measured = evaluate(e, e.folds[f].pairs, &net, cc, objective);
                    if (unseen)
                        byStep[{strength, step / 4}].push_back(std::move(measured));
                    else
                    {
                        double score = assess(measured, objective, .85).first;
                        if (score < best - 1e-10)
                        {
                            best = score;
                            epoch = step / 4;
                            result = std::move(measured);
                        }
                    }
                }
            });
            if (!unseen)
            {
                bestEpoch.push_back(epoch);
                chosen.push_back(std::move(result));
            }
        }
        if (!unseen)
        {
            std::sort(bestEpoch.begin(), bestEpoch.end());
            byStep[{1, bestEpoch[bestEpoch.size() / 2]}] = std::move(chosen);
        }
        for (auto &[key, rs] : byStep)
        {
            Candidate t;
            t.config = c;
            t.config.strength = key.first;
            t.epoch = key.second;
            t.threshold = threshold(rs);
            t.score = t.fnmr = 0;
            for (auto &r : rs)
            {
                auto s = assess(r, objective, t.threshold);
                t.score += s.first / rs.size();
                t.fnmr += s.second / rs.size();
            }
            describe(t);
            trials.push_back(t);
        }
    };
    std::vector<int> milestones;
    for (int n = 1; n <= epochs; ++n)
        if (!unseen || n == 1 || n == 3 || n == 6 || n == 12 || n == epochs)
            milestones.push_back(n);
    for (auto arm : unseen ? std::vector<std::string>{"residual-texture", "residual-both"}
                           : std::vector<std::string>{"residual-minutia", "residual-both"})
        for (auto mode :
             unseen ? std::vector<std::string>{"original"} : std::vector<std::string>{"original", "all"})
        {
            auto c = base;
            c.arm = arm;
            c.mode = mode;
            run(c, unseen ? std::vector<double>{.1, .25, .5, 1} : std::vector<double>{1}, milestones);
        }
    auto less = [](const Candidate &a, const Candidate &b) {
        return std::tie(a.score, a.fnmr, a.config.strength, a.epoch) <
               std::tie(b.score, b.fnmr, b.config.strength, b.epoch);
    };
    Candidate selected = *std::min_element(trials.begin(), trials.end(), less);
    if (unseen)
    {
        auto anchor = selected;
        for (auto mode : {"rotation", "gaussian"})
        {
            auto c = anchor.config;
            c.mode = mode;
            run(c, {c.strength}, {anchor.epoch});
        }
        auto allowed = trials;
        allowed.erase(
            std::remove_if(allowed.begin(), allowed.end(),
                           [&](auto &c) { return c.score >= baseline.score || c.fnmr >= baseline.fnmr; }),
            allowed.end());
        const auto &pool = allowed.empty() ? trials : allowed;
        selected = *std::min_element(pool.begin(), pool.end(), less);
    }
    deepprint::writeNew(out / "development.csv", dev.str());
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> preprocessing;
    std::ostringstream pilotReport, confirmation;
    if (heads)
    {
        pilotReport
            << "database,preprocessing,condition,genuine_pairs,impostor_pairs,eer,threshold,fmr,fnmr\n";
        confirmation << "database,scorer,preprocessing,condition,genuine_pairs,impostor_pairs,eer,threshold,"
                        "fmr,fnmr,cached_attempt_p95_ms\n";
        for (auto &[db, bank] : e.banks)
        {
            const bool bilateral = db == "FVC2002_DB2_B";
            auto conditions = bilateral ? std::vector<std::string>{"original", "noise-10-s42", "noise-40-s42"}
                                        : std::vector<std::string>{"original", "rotation-10", "rotation+10"};
            auto modes = bilateral ? std::vector<std::string>{"standard", "bilateral"}
                                   : std::vector<std::string>{"standard", "center", "roi"};
            const auto pairs = cartesian(bank, e.refit.at(db), e.refit.at(db), true);
            double best = 2;
            for (auto &mode : modes)
            {
                auto gallery = readCache(cache / db / mode / "original.npz");
                double sum = 0;
                for (auto &condition : conditions)
                {
                    auto probes = readCache(cache / db / mode / (condition + ".npz"));
                    if (probes.names != e.names.at(db) || gallery.names != probes.names)
                        throw std::runtime_error("Pilot order mismatch");
                    auto m = rates(compare(gallery.vectors, probes.vectors, pairs, .22), .966);
                    sum += m.eer / conditions.size();
                    pilotReport << db << ',' << mode << ',' << condition << ',' << m.columns() << '\n';
                }
                if (sum < best - 1e-10)
                {
                    best = sum;
                    preprocessing[db] = {mode, conditions};
                }
            }
        }
        deepprint::writeNew(out / "preprocessing_development.csv", pilotReport.str());
    }
    auto emit = [&](const std::string &label, const Candidate &c, const Tail *tail) {
        for (auto &[group, pairs] : e.final)
            for (auto &[db, sc] : evaluate(e, pairs, tail, c.config, e.profile))
            {
                std::string prefix = group + "," + label + "," + db + ",";
                final << prefix << "original," << rates(sc.at("original"), c.threshold).columns() << '\n';
                if (heads)
                {
                    for (auto &[key, v] : sc)
                    {
                        if (key != "original")
                            final << prefix << key << ',' << rates(v, c.threshold).columns() << '\n';
                        confirmation << db << ',' << label << ",standard," << key << ','
                                     << rates(v, c.threshold).columns() << ',';
                        if (!tail)
                        {
                            auto stored = readCache(cache / db / "standard" / (key + ".npz"));
                            std::set<size_t> indices;
                            for (auto p : pairs.at(db))
                            {
                                indices.insert(p.a);
                                indices.insert(p.b);
                            }
                            std::vector<double> times;
                            for (auto i : indices)
                                times.push_back(stored.timings.at(i));
                            std::sort(times.begin(), times.end());
                            double rank = .95 * (times.size() - 1);
                            size_t lo = static_cast<size_t>(rank), hi = std::min(lo + 1, times.size() - 1);
                            confirmation << times[lo] + (rank - lo) * (times[hi] - times[lo]);
                        }
                        confirmation << '\n';
                    }
                    auto &pilot = preprocessing.at(db);
                    if (pilot.first != "standard" && label != "training_ablation")
                    {
                        auto project = [&](const std::string &key) {
                            auto stored = readCache(cache / db / pilot.first / (key + ".npz"));
                            if (!tail)
                                return stored.vectors;
                            std::vector<float> flat(stored.vectors.size() * 512);
                            std::memcpy(flat.data(), stored.vectors.data(), flat.size() * 4);
                            Inputs in;
                            in.embed = studytorch::tensor(flat, {I(stored.vectors.size()), 512});
                            return embeddings(*tail, in, allIndices(stored.vectors.size()), c.config,
                                              stored.vectors.size());
                        };
                        auto gallery = project("original");
                        for (auto &condition : pilot.second)
                            confirmation
                                << db << ',' << label << ',' << pilot.first << ',' << condition << ','
                                << rates(compare(gallery, project(condition), pairs.at(db), c.config.weight),
                                         c.threshold)
                                       .columns()
                                << ",\n";
                    }
                }
                else
                    final << prefix << "augmentation_mean," << average(sc, e.profile, c.threshold).columns()
                          << '\n';
            }
    };
    emit("pretrained", baseline, nullptr);
    if (!heads && !unseen)
    {
        auto ref22 = baseline;
        ref22.config.weight = .22;
        ref22.threshold = threshold({evaluate(e, e.folds.at(0).pairs, nullptr, ref22.config, {})});
        emit("reference_22_78", ref22, nullptr);
    }
    auto refit = [&](Candidate c, const std::string &label) {
        assign(e, e.refit);
        c.config.steps = 4 * c.epoch;
        std::mt19937 rng(42);
        Tail t(w, c.config, rng);
        fit(t, c.config, e.banks, 42, 48, {});
        emit(label, c, &t);
        if (label == "candidate")
        {
            auto candidate = out / "deepprint_texminu_512.onnx";
            w.save(model, candidate, t.merged(c.config.strength));
            fs::path root = "data/fvc";
            if (fs::exists(cache / "source.txt"))
            {
                std::istringstream info(deepprint::readFile(cache / "source.txt"));
                std::string line;
                std::getline(info, line);
                std::getline(info, line);
                root = fs::u8path(line);
            }
            else
            {
                auto info = deepprint::readFile(cache / "source.json");
                std::smatch match;
                if (std::regex_search(info, match, std::regex("\"fvc\"\\s*:\\s*\"([^\"]+)\"")))
                {
                    auto path = fs::u8path(std::regex_replace(match[1].str(), std::regex("\\\\\\\\"), "\\"));
                    if (fs::is_directory(path))
                        root = path;
                }
            }
            Inference engine(candidate);
            std::ostringstream parity;
            parity << "database,capture,max_embedding_delta\n";
            for (auto &[db, b] : e.banks)
                for (I i : {I{0}, I(e.names.at(db).size() - 1)})
                {
                    auto input = imageInput(fvc::readTiff(root / db / e.names.at(db).at(i)), "standard");
                    auto actual = engine.run(input, {"embedding"}).at("embedding");
                    auto expected =
                        t.predict(b.images.at("original").get({i}, c.config.arm), c.config.strength).values();
                    if (actual.size() != 512 || expected.size() != 512)
                        throw std::runtime_error("Invalid projected output");
                    double delta = 0;
                    for (size_t k = 0; k < 512; ++k)
                        delta = std::max(delta, double(std::abs(actual[k] - expected[k])));
                    if (delta > .0005)
                        throw std::runtime_error("Residual projection export mismatch");
                    parity << db << ',' << e.names.at(db).at(i) << ',' << delta << '\n';
                }
            deepprint::writeNew(out / "export-parity.csv", parity.str());
        }
    };
    refit(selected, "candidate");
    if (!unseen)
    {
        auto it = std::find_if(trials.begin(), trials.end(), [&](auto &t) {
            return t.config.arm == selected.config.arm && t.config.mode != selected.config.mode;
        });
        if (it != trials.end())
            refit(*it, "training_ablation");
    }
    else if (selected.config.strength < 1)
    {
        auto it = std::find_if(trials.begin(), trials.end(), [&](auto &t) {
            return t.config.arm == selected.config.arm && t.config.mode == selected.config.mode &&
                   t.epoch == selected.epoch && t.config.strength == 1;
        });
        if (it != trials.end())
            refit(*it, "full_strength");
    }
    deepprint::writeNew(out / "evaluation.csv", final.str());
    if (heads)
        deepprint::writeNew(out / "confirmation.csv", confirmation.str());
}
} // namespace research
