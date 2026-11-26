#include "studies.h"
#include "training.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace research
{
using namespace studytorch;
namespace
{
using Ids = std::map<std::string, std::vector<I>>;
using Embeddings = std::map<std::string, Vectors>;
using Marks = std::map<std::string, std::map<unsigned, std::string>>;
using Key = std::tuple<double, double, int>;
struct Trial
{
    Config config;
    int weight = 22;
    Key key{2, 2, 101};
    std::map<std::string, Tensor> params;
};

fvc::GrayImage augment(fvc::GrayImage image, const std::string &mode, unsigned seed)
{
    if (mode == "original")
        return image;
    std::mt19937 rng(seed);
    auto uniform = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    std::vector<float> pixels(image.pixels.begin(), image.pixels.end());
    if (mode == "geometry" || mode == "mixed")
    {
        int w = image.width, h = image.height;
        double rad = uniform(-5, 5) * std::acos(-1.) / 180, c = std::cos(rad), s = std::sin(rad);
        double tx = uniform(-.03, .03) * w, ty = uniform(-.03, .03) * h, cx = (w - 1) / 2., cy = (h - 1) / 2.;
        auto pixel = [&](int x, int y) {
            return (x >= 0 && y >= 0 && x < w && y < h) ? image.pixels[y * w + x] : 255;
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                double dx = x - cx - tx, dy = y - cy - ty;
                double a = c * dx - s * dy + cx, b = s * dx + c * dy + cy;
                int i = std::floor(a), j = std::floor(b);
                double u = a - i, v = b - j;
                pixels[y * w + x] = float((1 - v) * ((1 - u) * pixel(i, j) + u * pixel(i + 1, j)) +
                                          v * ((1 - u) * pixel(i, j + 1) + u * pixel(i + 1, j + 1)));
            }
    }
    if (mode == "tone" || mode == "mixed")
    {
        double scale = uniform(.9, 1.1), shift = uniform(-10, 10);
        for (auto &v : pixels)
            if (v < 250)
                v = float((v - 127.5) * scale + 127.5 + shift);
    }
    if (mode == "noise")
    {
        std::normal_distribution<float> noise(0, float(uniform(2, 5)));
        for (auto &v : pixels)
            v += noise(rng);
    }
    for (size_t i = 0; i < pixels.size(); ++i)
        image.pixels[i] = static_cast<unsigned char>(std::nearbyint(std::clamp(pixels[i], 0.f, 255.f)));
    return image;
}
Inputs extract(Inference &engine, const fs::path &root, const Bank &b, const std::vector<I> &ids, bool blocks,
               const std::string &mode, unsigned seed)
{
    std::vector<std::string> names{"embedding", textureInput, minutiaInput};
    if (blocks)
    {
        names.push_back(textureBlock);
        names.push_back(minutiaBlock);
    }
    std::map<std::string, std::vector<float>> raw;
    for (auto i : ids)
    {
        auto img = augment(fvc::readTiff(root / b.samples[i].relativePath), mode, seed + unsigned(i));
        auto in = imageInput(img, deployed(img.type));
        auto result = engine.run(in, names);
        for (auto &[name, v] : result)
        {
            auto &data = raw[name];
            if (data.empty())
                data.resize(b.samples.size() * v.size());
            std::copy(v.begin(), v.end(), data.begin() + i * v.size());
        }
    }
    auto get = [&](const std::string &name, I channels) -> Tensor {
        if (!raw.count(name))
            return {};
        auto &v = raw.at(name);
        I count = b.samples.size(), width = v.size() / count;
        if (!channels)
            return tensor(v, {count, width});
        I side = std::lround(std::sqrt(double(width) / channels));
        return tensor(v, {count, channels, side, side});
    };
    return {get("embedding", 0), get(textureInput, 0), get(minutiaInput, 0), get(textureBlock, 1536),
            get(minutiaBlock, 896)};
}
Ids indices(const Banks &banks, const Marks &marks, const std::set<std::string> &labels)
{
    Ids result;
    for (auto &[db, b] : banks)
    {
        auto &list = result[db];
        for (size_t i = 0; i < b.samples.size(); ++i)
            if (labels.count(marks.at(db).at(b.samples[i].finger)))
                list.push_back(i);
    }
    return result;
}
std::vector<Pair> within(const Bank &b, const std::vector<I> &selected)
{
    std::set<I> scope(selected.begin(), selected.end());
    std::vector<Pair> pairs;
    for (auto &p : b.pairs)
        if (scope.count(p.a) && scope.count(p.b))
            pairs.push_back(p);
    return pairs;
}
Vectors rawVectors(const Inputs &inputs)
{
    auto values = inputs.embed.values();
    Vectors v(values.size() / 512);
    std::memcpy(v.data(), values.data(), values.size() * 4);
    return v;
}
Embeddings predict(const Tail *tail, const Config &c, const FoldBanks &data, const Ids &selected)
{
    Embeddings result;
    for (auto &[db, b] : data)
    {
        auto &input = b.images.at("original");
        result[db] =
            tail ? embeddings(*tail, input, selected.at(db), c, b.fingers.size()) : rawVectors(input);
    }
    return result;
}
std::vector<int> weights()
{
    std::vector<int> w{22};
    for (int n = 0; n <= 100; n += 5)
        w.push_back(n);
    return w;
}
std::vector<Metrics> assess(const Banks &banks, const Embeddings &values, const Ids &ids, int w)
{
    std::vector<Metrics> result;
    for (auto &[db, b] : banks)
    {
        auto subset = within(b, ids.at(db));
        auto &v = values.at(db);
        result.push_back(measure(compare(v, v, subset, w / 100.)));
    }
    return result;
}
Key report(const Banks &banks, const std::vector<Embeddings> &folds, const std::vector<Ids> &val,
           const Config &c, int w, std::ostream &out, const std::string &stage)
{
    std::vector<double> means(banks.size());
    for (size_t f = 0; f < folds.size(); ++f)
    {
        auto rows = assess(banks, folds[f], val[f], w);
        size_t j = 0;
        for (auto &[db, b] : banks)
        {
            const auto &m = rows[j];
            means[j] += m.eer / folds.size();
            ++j;
            out << stage << ',' << f + 1 << ',' << c.arm << ',' << c.mode << ',' << c.rank << ',' << c.steps
                << ',' << c.rate << ',' << c.negatives << ',' << c.loss << ',' << c.strength << ','
                << w / 100. << ',' << db << ',' << m.columns() << '\n';
        }
    }
    return {*std::max_element(means.begin(), means.end()),
            std::accumulate(means.begin(), means.end(), 0.) / means.size(), std::abs(w - 22)};
}
void saveParameters(const fs::path &path, const std::map<std::string, Tensor> &params)
{
    std::ostringstream index;
    for (auto &[name, t] : params)
    {
        auto v = t.values();
        std::string file = name;
        std::replace(file.begin(), file.end(), ':', '_');
        index << name << ',' << file << ".f32," << v.size() << '\n';
        deepprint::writeNew(path / (file + ".f32"), {reinterpret_cast<char *>(v.data()), v.size() * 4});
    }
    deepprint::writeNew(path / "tensors.csv", "name,file,floats\n" + index.str());
}
} // namespace

void tailStudy(const std::string &study, const fs::path &model, const fs::path &root, const fs::path &pairs,
               const fs::path &splits, const fs::path &out, int limit, int batch)
{
    const bool branches = study == "branches";
    auto banks = dataset(root, pairs);
    auto splitRows = csv(splits);
    std::vector<Marks> marks(branches ? 1 : 3);
    for (auto &r : splitRows)
    {
        size_t fold = branches ? 0 : std::stoul(r.at("fold")) - 1;
        auto &m = marks.at(fold)[r.at("database")];
        if (!m.emplace(std::stoul(r.at("finger")), r.at(branches ? "split" : "role")).second)
            throw std::runtime_error("Duplicate split row");
    }
    fvc::createOutputDirectory(out);
    deepprint::writeNew(out / "split.csv", deepprint::readFile(splits));
    deepprint::writeNew(out / "inputs.csv", "input,sha256\nmodel," + deepprint::sha256(model) + "\npairs," +
                                                deepprint::sha256(pairs) + "\nsplit," +
                                                deepprint::sha256(splits) + "\n");
    std::map<std::string, Dimensions> extra{{textureInput, {1, 1536}}, {minutiaInput, {1, 1024}}};
    if (branches)
    {
        extra[textureBlock] = {1, 1536, 8, 8};
        extra[minutiaBlock] = {1, 896, 18, 18};
    }
    auto featureModel = out / "activations.onnx";
    exposeOutputs(model, featureModel, extra);
    Inference engine(featureModel);
    Weights initial(model);
    if (branches)
        initial.loadTextureBlock("build/dependencies/best_model.pyt",
                                 "tools/deepprint/architecture/parameters.tsv");
    auto dev = indices(banks, marks.front(), {"train", "validation"}),
         confirm = indices(banks, marks.front(), {"confirmation"});
    std::vector<Ids> val, train;
    for (auto &m : marks)
    {
        val.push_back(indices(banks, m, {"validation"}));
        train.push_back(indices(banks, m, {"train"}));
        if (indices(banks, m, {"confirmation"}) != confirm)
            throw std::runtime_error("Confirmation IDs changed between folds");
        for (auto &[db, b] : banks)
            if (train.back().at(db).size() != 32 || val.back().at(db).size() != 24 ||
                confirm.at(db).size() != 24)
                throw std::runtime_error("Expected a 4/3/3 finger split");
    }
    FoldBanks data;
    unsigned seed = 3007;
    for (auto &[db, b] : banks)
    {
        auto &item = data[db];
        for (auto &s : b.samples)
            item.fingers.push_back(s.finger);
        auto modes = branches ? std::vector<std::string>{"original"}
                              : std::vector<std::string>{"original", "geometry", "tone", "mixed", "noise"};
        for (auto &mode : modes)
        {
            item.images[mode] = extract(engine, root, b, dev.at(db), branches, mode, seed);
            std::cout << db << " cached " << mode << std::endl;
        }
        seed += 1000;
    }
    std::ostringstream rows;
    rows << "stage,fold,arm,augmentation,rank,steps,lr,negatives,loss,strength,texture_weight,database,"
            "genuine_pairs,impostor_pairs,eer,eer_threshold,fmr,fnmr\n";
    Trial frozen, winner;
    std::optional<Tail> trained;
    Config baseline;
    baseline.arm = "frozen";
    baseline.steps = 0;
    std::vector<Embeddings> base;
    for (auto &ids : val)
        base.push_back(predict(nullptr, baseline, data, ids));
    for (int w : weights())
    {
        auto key = report(banks, base, val, baseline, w, rows, "baseline");
        if (key < frozen.key)
        {
            frozen.key = key;
            frozen.weight = w;
        }
    }
    auto selectTrain = [&](size_t f) {
        for (auto &[db, b] : data)
            b.train = train[f].at(db);
    };
    if (branches)
    {
        selectTrain(0);
        for (auto arm : {"A", "B", "C"})
        {
            Config c;
            c.arm = arm;
            c.rate = c.arm == "A" ? 1e-4 : 2e-5;
            c.steps = limit ? limit : 12;
            std::mt19937 rng(3007);
            Tail tail(initial, c, rng);
            fit(tail, c, data, 3007, batch, [&](int step, const Tail &t) {
                if (step % 4 && step != c.steps)
                    return;
                auto cc = c;
                cc.steps = step;
                auto validation = predict(&t, cc, data, val[0]);
                saveParameters(out / "checkpoints" / (cc.arm + "-" + std::to_string(step)), t.merged(1));
                for (auto &[db, v] : validation)
                {
                    std::string raw;
                    for (auto i : val[0].at(db))
                        raw.append(reinterpret_cast<const char *>(v[i].data()), 2048);
                    deepprint::writeNew(out / (cc.arm + "-" + std::to_string(step) + "-" + db + ".f32"), raw);
                }
                auto best = winner.key;
                for (int w : weights())
                {
                    auto key = report(banks, {validation}, val, cc, w, rows, "branch");
                    if (key < winner.key)
                        winner = {cc, w, key, {}};
                }
                if (winner.key < best)
                {
                    trained = t.frozen();
                    winner.params = t.merged(1);
                }
                std::cout << "Trained " << arm << " step " << step << std::endl;
            });
        }
    }
    else
    {
        using CacheKey =
            std::tuple<std::string, std::string, int, int, double, std::string, std::string, double>;
        std::map<CacheKey, std::vector<Embeddings>> cached;
        auto evaluate = [&](const Config &c) -> const std::vector<Embeddings> & {
            CacheKey key{c.arm, c.mode, c.rank, c.steps, c.rate, c.loss, c.negatives, c.strength};
            if (cached.count(key))
                return cached.at(key);
            auto &result = cached[key];
            for (size_t f = 0; f < val.size(); ++f)
            {
                selectTrain(f);
                std::mt19937 rng(3007 + f);
                Tail t(initial, c, rng);
                fit(t, c, data, 3007 + f, batch, {});
                result.push_back(predict(&t, c, data, val[f]));
            }
            return result;
        };
        std::vector<Config> considered;
        auto choose = [&](const std::vector<Config> &options, const std::string &stage) {
            Key best{2, 2, 101};
            Config chosen;
            for (auto &c : options)
            {
                auto k = report(banks, evaluate(c), val, c, 22, rows, stage);
                if (k < best)
                {
                    best = k;
                    chosen = c;
                }
                considered.push_back(c);
            }
            std::cout << "Validated " << stage << std::endl;
            return chosen;
        };
        std::vector<Config> grid;
        for (auto rate : {3e-5, 1e-4})
            for (int steps : limit ? std::vector<int>{limit} : std::vector<int>{12, 48, 120})
            {
                Config c;
                c.rate = rate;
                c.steps = steps;
                grid.push_back(c);
            }
        auto selected = choose(grid, "schedule");
        grid.clear();
        for (auto mode : {"original", "geometry", "tone", "mixed", "noise"})
        {
            auto c = selected;
            c.mode = mode;
            grid.push_back(c);
        }
        selected = choose(grid, "augmentation");
        grid.clear();
        for (auto arm : {"texture", "minutia", "A"})
        {
            auto c = selected;
            c.arm = arm;
            grid.push_back(c);
        }
        selected = choose(grid, "arm");
        grid.clear();
        for (auto rank : {0, 4, 8})
        {
            auto c = selected;
            c.rank = rank;
            grid.push_back(c);
        }
        selected = choose(grid, "rank");
        grid = {selected};
        auto mixed = selected;
        mixed.negatives = "mixed";
        grid.push_back(mixed);
        mixed.loss = "supcon";
        grid.push_back(mixed);
        selected = choose(grid, "loss");
        grid.clear();
        for (auto strength : {1., .25, .5})
        {
            auto c = selected;
            c.strength = strength;
            grid.push_back(c);
        }
        selected = choose(grid, "strength");
        const auto &fixed = evaluate(selected);
        const auto before = report(banks, base, val, baseline, 22, rows, "baseline_fixed");
        const auto after = report(banks, fixed, val, selected, 22, rows, "selected_fixed");
        int improved = 0;
        double gain = 0;
        std::ostringstream gate;
        gate << "fold,baseline_FVC2002_EER,candidate_FVC2002_EER\n";
        const auto &fvc02 = banks.at("FVC2002_DB2_B");
        for (size_t f = 0; f < val.size(); ++f)
        {
            auto selectedPairs = within(fvc02, val[f].at(fvc02.name));
            auto rate = [&](const Embeddings &embeddings) {
                const auto &v = embeddings.at(fvc02.name);
                return measure(compare(v, v, selectedPairs, .22)).eer;
            };
            double original = rate(base[f]), candidate = rate(fixed[f]);
            improved += original - candidate > 1e-12;
            gain += (original - candidate) / val.size();
            gate << f + 1 << ',' << original << ',' << candidate << '\n';
        }
        deepprint::writeNew(out / "tail-gate-by-fold.csv", gate.str());
        deepprint::writeNew(out / "tail-gate.txt",
                            improved >= 2 && gain >= .005 && after < before ? "passed\n" : "not passed\n");
        std::vector<std::pair<Key, Config>> finalists;
        std::set<CacheKey> seen;
        for (auto &c : considered)
            if (c.steps == selected.steps &&
                seen.emplace(c.arm, c.mode, c.rank, c.steps, c.rate, c.loss, c.negatives, c.strength).second)
                finalists.emplace_back(report(banks, evaluate(c), val, c, 22, rows, "shortlist"), c);
        std::stable_sort(finalists.begin(), finalists.end(),
                         [](auto &a, auto &b) { return a.first < b.first; });
        for (size_t i = 0; i < std::min<size_t>(3, finalists.size()); ++i)
            for (int w : weights())
            {
                auto &c = finalists[i].second;
                auto key = report(banks, evaluate(c), val, c, w, rows, "fusion");
                if (key < winner.key)
                    winner = {c, w, key, {}};
            }
        for (auto &[db, b] : data)
            b.train = dev.at(db);
        std::mt19937 rng(3007);
        Tail t(initial, winner.config, rng);
        fit(t, winner.config, data, 3007, batch, {});
        winner.params = t.merged(winner.config.strength);
        trained = t.frozen();
    }
    deepprint::writeNew(out / "validation.csv", rows.str());
    auto &c = winner.config;
    std::ostringstream selected;
    selected << "arm,steps,lr,mode,rank,negatives,loss,strength,texture_weight,baseline_texture_weight\n"
             << c.arm << ',' << c.steps << ',' << c.rate << ',' << c.mode << ',' << c.rank << ','
             << c.negatives << ',' << c.loss << ',' << c.strength << ',' << winner.weight / 100. << ','
             << frozen.weight / 100. << '\n';
    deepprint::writeNew(out / "selection.csv", selected.str());
    saveParameters(out / "checkpoint", winner.params);
    auto candidate = out / "deepprint_texminu_512.onnx";
    initial.save(model, candidate, winner.params);
    Inference learned(candidate);
    std::ostringstream final;
    std::ostringstream parity;
    parity << "database,max_embedding_delta,max_score_delta,comparison_pairs\n";
    final << "case,split,database,texture_weight,genuine_pairs,impostor_pairs,eer,eer_threshold,fmr,fnmr\n";
    for (auto &[db, b] : banks)
    {
        Vectors before, after, nativeTorch;
        double maxDelta = 0;
        for (auto &s : b.samples)
        {
            auto img = fvc::readTiff(root / s.relativePath);
            auto x = imageInput(img, deployed(img.type));
            std::vector<std::string> names{"embedding", textureInput, minutiaInput};
            if (c.arm == "B")
                names.push_back(textureBlock);
            if (c.arm == "C")
                names.push_back(minutiaBlock);
            auto raw = engine.run(x, names);
            auto inputTensor = [&](const std::string &k, I channels) -> Tensor {
                if (!raw.count(k))
                    return {};
                auto &v = raw.at(k);
                if (!channels)
                    return tensor(v, {1, I(v.size())});
                I side = std::lround(std::sqrt(double(v.size()) / channels));
                return tensor(v, {1, channels, side, side});
            };
            Inputs in{inputTensor("embedding", 0), inputTensor(textureInput, 0), inputTensor(minutiaInput, 0),
                      inputTensor(textureBlock, 1536), inputTensor(minutiaBlock, 896)};
            auto expected = trained->predict(in, c.strength).values();
            auto actual = learned.run(x, {"embedding"}).at("embedding");
            if (actual.size() != 512 || raw.at("embedding").size() != 512 || expected.size() != 512)
                throw std::runtime_error("Invalid export");
            before.emplace_back();
            after.emplace_back();
            nativeTorch.emplace_back();
            std::copy_n(raw.at("embedding").begin(), 512, before.back().begin());
            std::copy_n(actual.begin(), 512, after.back().begin());
            std::copy_n(expected.begin(), 512, nativeTorch.back().begin());
            for (size_t k = 0; k < 512; ++k)
                maxDelta = std::max(maxDelta, double(std::abs(actual[k] - expected[k])));
        }
        double scoreDelta = 0;
        auto expected = compare(nativeTorch, nativeTorch, b.pairs, winner.weight / 100.);
        auto actual = compare(after, after, b.pairs, winner.weight / 100.);
        for (size_t k = 0; k < expected.size(); ++k)
            scoreDelta = std::max(scoreDelta, std::abs(expected[k].score - actual[k].score));
        parity << db << ',' << maxDelta << ',' << scoreDelta << ',' << actual.size() << '\n';
        if (maxDelta > .002 || scoreDelta > .00200001)
            throw std::runtime_error(db + " LibTorch / ONNX export mismatch");
        for (auto split : {"confirmation", "all"})
        {
            auto selectedPairs = std::string(split) == "all" ? b.pairs : within(b, confirm.at(db));
            for (auto label : {"baseline-22", "baseline-tuned", "candidate-22", "candidate-tuned"})
            {
                bool base = std::string(label).find("baseline") == 0;
                int w = std::string(label).find("tuned") != std::string::npos
                            ? (base ? frozen.weight : winner.weight)
                            : 22;
                auto &v = base ? before : after;
                final << label << ',' << split << ',' << db << ',' << w / 100. << ','
                      << measure(compare(v, v, selectedPairs, w / 100.)).columns() << '\n';
            }
        }
    }
    deepprint::writeNew(out / "evaluation.csv", final.str());
    deepprint::writeNew(out / "export-parity.csv", parity.str());
}
} // namespace research
