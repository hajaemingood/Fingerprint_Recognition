#include "research_data.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>
#include <tuple>

namespace research
{
std::vector<float> align(const std::vector<float> &image, const std::string &mode)
{
    constexpr int side = 299, block = 16, cells = side / block;
    std::array<std::array<bool, cells>, cells> active{};
    std::array<int, cells> rows{}, cols{};
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x)
        {
            double sum = 0, squares = 0;
            int dark = 0;
            for (int j = 0; j < block; ++j)
                for (int i = 0; i < block; ++i)
                {
                    auto v = std::nearbyint(255 * image[(y * block + j) * side + x * block + i]);
                    sum += v;
                    squares += v * v;
                    dark += v < 180;
                }
            constexpr int n = block * block;
            active[y][x] = squares / n - (sum / n) * (sum / n) >= 144 && dark >= .08 * n;
            if (active[y][x])
            {
                ++rows[y];
                ++cols[x];
            }
        }
    double cx = 0, cy = 0, xx = 0, xy = 0, yy = 0;
    int n = 0;
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x)
            if (active[y][x] && rows[y] >= 2 && cols[x] >= 2)
            {
                double a = x * block + 8, b = y * block + 8;
                cx += a;
                cy += b;
                xx += a * a;
                xy += a * b;
                yy += b * b;
                ++n;
            }
    if (n < 16)
        return image;
    cx /= n;
    cy /= n;
    xx = xx / n - cx * cx;
    xy = xy / n - cx * cy;
    yy = yy / n - cy * cy;
    double strength = mode == "half-center" ? .5 : 1;
    auto tx = std::nearbyint(std::clamp((149.5 - cx) * strength, -18., 18.));
    auto ty = std::nearbyint(std::clamp((149.5 - cy) * strength, -18., 18.));
    double radians = 0;
    if (mode == "center-roll")
    {
        double delta = std::hypot(xx - yy, 2 * xy), angle = .5 * std::atan2(2 * xy, yy - xx);
        if (xx + yy + delta >= 1.25 * (xx + yy - delta) && std::abs(angle) <= std::acos(-1.) / 6)
            radians = std::clamp(angle, -std::acos(-1.) / 15, std::acos(-1.) / 15);
    }
    else if (mode != "center" && mode != "half-center")
        throw std::runtime_error("Unknown alignment " + mode);
    double c = std::cos(radians), s = std::sin(radians);
    auto pixel = [&](int x, int y) {
        return x >= 0 && x < side && y >= 0 && y < side ? image[y * side + x] : 1.f;
    };
    std::vector<float> out(image.size());
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
        {
            double dx = x + .5 - cx - tx, dy = y + .5 - cy - ty;
            double col = c * dx + s * dy + cx - .5, row = -s * dx + c * dy + cy - .5;
            int i = std::floor(col), j = std::floor(row);
            double u = col - i, v = row - j;
            out[y * side + x] = float((1 - v) * ((1 - u) * pixel(i, j) + u * pixel(i + 1, j)) +
                                      v * ((1 - u) * pixel(i, j + 1) + u * pixel(i + 1, j + 1)));
        }
    return out;
}

void screen(const fs::path &model, const fs::path &root, const fs::path &pairFile, const fs::path &split,
            const fs::path &out, bool alignment, const fs::path &dll)
{
    auto banks = dataset(root, pairFile);
    std::map<std::pair<std::string, unsigned>, std::string> labels;
    for (auto &r : csv(split))
        labels.emplace(std::make_pair(r.at("database"), std::stoul(r.at("finger"))), r.at("split"));
    auto weights = std::vector<int>{22};
    if (!alignment)
        for (int w = 0; w <= 100; w += 5)
            weights.push_back(w);
    std::map<std::string, std::vector<std::string>> variants{
        {"FVC2000_DB3_B", {"standard"}},
        {"FVC2002_DB2_B", {"standard", "bilateral-06", "bilateral-12", "bilateral-20", "bilateral-30"}},
        {"FVC2004_DB1_B", {"roi-10", "roi-00", "roi-05", "roi-15", "roi-20", "center", "roi-10-bicubic"}}};
    if (alignment)
        for (auto &[db, modes] : variants)
            if (db != "FVC2000_DB3_B")
            {
                std::string base = db == "FVC2002_DB2_B" ? "bilateral-12" : "roi-20";
                modes = {base, base + "+half-center", base + "+center", base + "+center-roll"};
            }
    std::map<std::tuple<std::string, std::string, int, std::string>, Metrics> metrics;
    std::ostringstream dev, finals;
    dev << "database,preprocessing,texture_weight,genuine_pairs,impostor_pairs,eer,eer_threshold,fmr,fnmr\n";
    finals << "candidate,database,preprocessing,texture_weight,split,genuine_pairs,impostor_pairs,eer,eer_"
              "threshold,fmr,fnmr\n";
    Inference engine(model);
    std::map<std::string, std::vector<VerificationScore>> references;
    if (dll.empty())
        for (auto &r : csv(pairFile))
            references[r.at("database")].push_back({std::stod(r.at("score")), r.at("genuine") == "1"});
    for (auto &[db, b] : banks)
    {
        std::map<std::string, std::vector<Pair>> pairs;
        if (!dll.empty())
            references[db] = nativeScores(root, b, dll);
        std::map<std::string, std::vector<VerificationScore>> nativeGroups;
        pairs["all"] = b.pairs;
        for (size_t index = 0; index < b.pairs.size(); ++index)
        {
            auto &p = b.pairs[index];
            auto label = labels.at({db, b.samples[p.a].finger});
            auto score = references.at(db).at(index);
            nativeGroups["all"].push_back(score);
            if (label == labels.at({db, b.samples[p.b].finger}))
            {
                pairs[label].push_back(p);
                nativeGroups[label].push_back(score);
            }
        }
        for (auto &[label, scores] : nativeGroups)
            metrics[{db, "native", 22, label}] = measure(scores);
        for (auto &mode : variants.at(db))
        {
            Vectors vectors;
            for (auto &sample : b.samples)
            {
                auto input = imageInput(fvc::readTiff(root / sample.relativePath), mode);
                auto plus = mode.find('+');
                if (plus != std::string::npos)
                    input = align(input, mode.substr(plus + 1));
                auto v = engine.run(input, {"embedding"}).at("embedding");
                if (v.size() != 512)
                    throw std::runtime_error("Incorrect model output");
                vectors.emplace_back();
                std::copy(v.begin(), v.end(), vectors.back().begin());
            }
            for (auto w : weights)
                for (auto &[group, selected] : pairs)
                {
                    auto m = measure(compare(vectors, vectors, selected, w / 100.));
                    metrics[{db, mode, w, group}] = m;
                    if (group == "development")
                        dev << db << ',' << mode << ',' << w / 100. << ',' << m.columns() << '\n';
                }
            std::cout << db << " " << mode << std::endl;
        }
    }
    auto best = [&](const std::string &db, int w) {
        auto &modes = variants.at(db);
        return *std::min_element(modes.begin(), modes.end(), [&](auto &a, auto &b) {
            return metrics.at({db, a, w, "development"}).eer < metrics.at({db, b, w, "development"}).eer;
        });
    };
    auto rank = [&](int w) {
        double largest = 0, sum = 0;
        for (auto &[db, b] : banks)
        {
            double e = metrics.at({db, best(db, w), w, "development"}).eer;
            largest = std::max(largest, e);
            sum += e;
        }
        return std::make_tuple(largest, sum, std::abs(w - 22));
    };
    int tuned =
        *std::min_element(weights.begin(), weights.end(), [&](int a, int b) { return rank(a) < rank(b); });
    for (auto &[db, b] : banks)
    {
        std::vector<std::pair<std::string, int>> choices{
            {"native_baseline", 22}, {alignment ? "alignment" : "preprocessing_only", 22}};
        if (!alignment)
            choices.emplace_back("preprocessing_and_fusion", tuned);
        for (auto &[name, w] : choices)
            for (auto group : {"development", "confirmation", "all"})
            {
                auto mode = name == "native_baseline" ? variants.at(db).front() : best(db, w);
                finals << name << ',' << db << ',' << mode << ',' << w / 100. << ',' << group << ','
                       << metrics.at({db, name == "native_baseline" ? "native" : mode, w, group}).columns()
                       << '\n';
            }
    }
    fvc::createOutputDirectory(out);
    deepprint::writeNew(out / "development.csv", dev.str());
    deepprint::writeNew(out / "evaluation.csv", finals.str());
}
} // namespace research
