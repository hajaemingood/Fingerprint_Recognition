#include "research_data.h"
#include "feature/deepprint_preprocessing.h"
#include "feature/template_manager.h"
#include "matching/fingerprint_matcher.h"
#include "kisa_library.h"
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace research
{
namespace
{
std::vector<std::string> fields(const std::string &line)
{
    std::vector<std::string> parts(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];
        if (c == '"')
        {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"')
            {
                parts.back() += c;
                ++i;
            }
            else
                quoted = !quoted;
        }
        else if (c == ',' && !quoted)
            parts.emplace_back();
        else if (c != '\r')
            parts.back() += c;
    }
    if (quoted)
        throw std::runtime_error("Unterminated quoted CSV value");
    return parts;
}
} // namespace

std::vector<Row> csv(const fs::path &path)
{
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error("Cannot read CSV " + path.u8string());
    if (line.compare(0, 3, "\xef\xbb\xbf") == 0)
        line.erase(0, 3);
    auto headers = fields(line);
    std::vector<Row> rows;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        auto values = fields(line);
        if (values.size() != headers.size())
            throw std::runtime_error("Bad CSV column count in " + path.u8string());
        Row row;
        for (size_t i = 0; i < headers.size(); ++i)
            row.emplace(headers[i], std::move(values[i]));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string basename(const std::string &path)
{
    return fs::u8path(path).filename().u8string();
}

std::string Metrics::columns() const
{
    std::ostringstream s;
    s << std::setprecision(16) << genuine << ',' << impostor << ',' << eer << ',' << threshold << ',' << fmr
      << ',' << fnmr;
    return s.str();
}

Metrics measure(const std::vector<VerificationScore> &scores)
{
    std::array<int, 1001> own{}, other{};
    int g = 0, i = 0;
    for (auto &x : scores)
    {
        if (!std::isfinite(x.score) || x.score < 0 || x.score > 1)
            throw std::runtime_error("Invalid score");
        ++(x.genuine ? own : other)[std::lround(x.score * 1000)];
        ++(x.genuine ? g : i);
    }
    if (!g || !i)
        throw std::runtime_error("Both genuine and impostor pairs are required");
    Metrics previous{g, i, .5, 0, 1, 0};
    int rejected = 0, accepted = i;
    for (int t = 0; t <= 1001; ++t)
    {
        double f = double(accepted) / i, n = double(rejected) / g;
        Metrics current{g, i, (f + n) / 2, t / 1000., f, n};
        if (f == n)
            return current;
        if (n > f)
            return previous.eer <= current.eer ? previous : current;
        previous = current;
        if (t <= 1000)
        {
            rejected += own[t];
            accepted -= other[t];
        }
    }
    throw std::logic_error("No EER crossing");
}

std::string deployed(int type)
{
    return type == 2 ? "bilateral-12" : type == 3 ? "roi-20" : "standard";
}

std::vector<float> imageInput(const fvc::GrayImage &image, const std::string &mode)
{
    using Mode = fingerprint::DeepPrintPreprocessing;
    const auto base = mode.substr(0, mode.find('+'));
    Mode policy = Mode::Standard;
    int sigma = 12, margin = 10;
    if (base == "center")
        policy = Mode::ForegroundCenter;
    else if (base.rfind("roi-", 0) == 0)
    {
        policy = Mode::ForegroundCrop;
        margin = std::stoi(base.substr(4));
    }
    else if (base.rfind("bilateral-", 0) == 0)
    {
        policy = Mode::Bilateral;
        sigma = std::stoi(base.substr(10));
    }
    else if (base != "standard")
        throw std::runtime_error("Unknown input mode " + mode);
    if (sigma <= 0 || margin < 0)
        throw std::runtime_error("Invalid preprocessing parameters");
    return fingerprint::preprocessDeepPrintVariant(image.pixels.data(), {int(image.width), int(image.height)},
                                                   policy, sigma, margin,
                                                   base.find("bicubic") != std::string::npos);
}

Banks dataset(const fs::path &root, const fs::path &pairPath)
{
    Banks banks;
    for (const auto &sample : fvc::scanDataset(root, true))
    {
        auto &b = banks[sample.database];
        b.name = sample.database;
        b.byName.emplace(sample.relativePath.filename().u8string(), b.samples.size());
        b.samples.push_back(sample);
    }
    if (pairPath.empty())
    {
        for (auto &[name, b] : banks)
            for (size_t a = 0; a < b.samples.size(); ++a)
                for (size_t j = a + 1; j < b.samples.size(); ++j)
                    b.pairs.push_back({a, j, b.samples[a].finger == b.samples[j].finger});
    }
    else
    {
        std::map<std::string, std::set<std::pair<size_t, size_t>>> seen;
        for (const auto &r : csv(pairPath))
        {
            auto &b = banks.at(r.at("database"));
            size_t a = b.byName.at(basename(r.at("enrolled"))), j = b.byName.at(basename(r.at("probe")));
            bool same = b.samples[a].finger == b.samples[j].finger;
            if (r.at("status") != "ok" || a == j || !seen[b.name].insert(std::minmax(a, j)).second ||
                r.at("genuine") != (same ? "1" : "0"))
                throw std::runtime_error("Invalid benchmark pair");
            b.pairs.push_back({a, j, same});
        }
        for (auto &[name, b] : banks)
            if (b.pairs.size() != 3160 || b.samples.size() != 80)
                throw std::runtime_error("Incomplete FVC benchmark " + name);
    }
    return banks;
}

std::vector<VerificationScore> compare(const Vectors &gallery, const Vectors &probes,
                                       const std::vector<Pair> &pairs, double weight)
{
    std::vector<VerificationScore> scores;
    scores.reserve(pairs.size());
    for (const auto &p : pairs)
    {
        auto branches = fingerprint::compareBranches(gallery.at(p.a), probes.at(p.b));
        scores.push_back(
            {std::round(fingerprint::fuseBranchScores(branches, weight) * 1000) / 1000, p.genuine});
    }
    return scores;
}

struct Inference::Impl
{
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "DeepPrint research"};
    Ort::Session session{nullptr};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Impl(const fs::path &model, int threads)
    {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(threads);
        options.SetInterOpNumThreads(1);
        session = Ort::Session(env, model.c_str(), options);
    }
};
Inference::Inference(const fs::path &model, int threads) : impl(std::make_unique<Impl>(model, threads)) {}
Inference::~Inference() = default;

Outputs Inference::run(std::vector<float> &input, const std::vector<std::string> &outputs)
{
    const std::array<int64_t, 4> shape{1, 1, 299, 299};
    auto tensor = Ort::Value::CreateTensor<float>(impl->memory, input.data(), input.size(), shape.data(), 4);
    std::vector<const char *> names;
    for (auto &name : outputs)
        names.push_back(name.c_str());
    const char *image = "image";
    auto result = impl->session.Run(Ort::RunOptions{nullptr}, &image, &tensor, 1, names.data(), names.size());
    Outputs data;
    for (size_t j = 0; j < names.size(); ++j)
    {
        auto n = result[j].GetTensorTypeAndShapeInfo().GetElementCount();
        const auto *ptr = result[j].GetTensorData<float>();
        if (!std::all_of(ptr, ptr + n, [](float v) { return std::isfinite(v); }))
            throw std::runtime_error("Invalid ONNX output");
        data.emplace(names[j], std::vector<float>(ptr, ptr + n));
    }
    return data;
}

void exposeOutputs(const fs::path &model, const fs::path &output,
                   const std::map<std::string, Dimensions> &names)
{
    using namespace deepprint;
    auto proto = decode(readFile(model));
    auto graph = decode(field(proto, 7).bytes);
    for (auto &[name, dims] : names)
    {
        Message shape;
        for (auto dim : dims)
            shape.push_back({1, 2, 0, encode({{1, 0, static_cast<uint64_t>(dim), {}}})});
        auto tensorType = encode({{1, 0, 1, {}}, {2, 2, 0, encode(shape)}});
        auto type = encode({{1, 2, 0, tensorType}});
        graph.push_back({12, 2, 0, encode({{1, 2, 0, name}, {2, 2, 0, type}})});
    }
    field(proto, 7).bytes = encode(graph);
    graph.clear();
    writeNew(output, encode(proto));
}

std::vector<VerificationScore> nativeScores(const fs::path &root, const Bank &b, const fs::path &dll)
{
    KisaLibrary native(dll);
    std::vector<std::vector<unsigned char>> templates;
    for (auto &s : b.samples)
    {
        auto img = fvc::readTiff(root / s.relativePath);
        std::vector<unsigned char> data(1000000);
        int n = 0, error = 0;
        char msg[256]{};
        if (native.enroll(img.type, img.pixels.data(), data.data(), &n, &error, msg) != 1 || error ||
            n <= 0 || n > 1000000)
            throw std::runtime_error("KISA_Enroll failed for " + s.relativePath.u8string());
        data.resize(n);
        templates.push_back(std::move(data));
    }
    std::vector<VerificationScore> scores;
    for (auto &p : b.pairs)
    {
        double score = 0;
        int error = 0;
        char msg[256]{};
        if (native.compare(templates[p.a].data(), templates[p.b].data(), &score, &error, msg) != 1 || error)
            throw std::runtime_error("KISA_Compare failed");
        scores.push_back({score, p.genuine});
    }
    return scores;
}

void verifyBaseline(const fs::path &root, const fs::path &pairs, const fs::path &dll, const fs::path &out,
                    const fs::path &model, double textureWeight)
{
    auto banks = dataset(root, pairs);
    KisaLibrary native(dll);
    Inference session(model);
    std::ostringstream rows;
    rows << "database,comparison_failures,genuine_pairs,impostor_pairs,eer,eer_threshold,fmr,fnmr,max_"
            "embedding_delta,max_score_delta\n";
    for (auto &[name, b] : banks)
    {
        std::vector<std::vector<unsigned char>> gallery, probes;
        Vectors vectors;
        double embDelta = 0, scoreDelta = 0;
        for (auto &sample : b.samples)
        {
            auto image = fvc::readTiff(root / sample.relativePath);
            auto input = imageInput(image, deployed(image.type));
            auto values = session.run(input, {"embedding"}).at("embedding");
            if (values.size() != 512)
                throw std::runtime_error("Expected 512 floats");
            fingerprint::Features v;
            std::copy(values.begin(), values.end(), v.begin());
            vectors.push_back(v);
            for (int role : {0, 1})
            {
                std::vector<unsigned char> bytes(1000000);
                int size = 0, error = 0;
                char message[256]{};
                auto fn = role ? native.attempt : native.enroll;
                if (fn(image.type, image.pixels.data(), bytes.data(), &size, &error, message) != 1 || error ||
                    size != fingerprint::kTemplateSize)
                    throw std::runtime_error("KISA feature extraction failed: " +
                                             sample.relativePath.u8string());
                bytes.resize(size);
                fingerprint::Features actual{};
                if (!fingerprint::decodeTemplate(bytes.data(), actual))
                    throw std::runtime_error("Invalid template");
                for (size_t i = 0; i < v.size(); ++i)
                    embDelta = std::max(embDelta, double(std::abs(v[i] - actual[i])));
                (role ? probes : gallery).push_back(std::move(bytes));
            }
        }
        auto scores = compare(vectors, vectors, b.pairs, textureWeight);
        std::vector<VerificationScore> nativeScores;
        for (size_t i = 0; i < b.pairs.size(); ++i)
        {
            auto p = b.pairs[i];
            int error = 0;
            char msg[256]{};
            double score = 0;
            if (native.compare(gallery[p.a].data(), probes[p.b].data(), &score, &error, msg) != 1 || error)
                throw std::runtime_error("KISA comparison failed");
            scoreDelta = std::max(scoreDelta, std::abs(scores[i].score - score));
            nativeScores.push_back({score, p.genuine});
        }
        const auto metric = measure(nativeScores);
        rows << name << ",0," << metric.columns() << ',' << embDelta << ',' << scoreDelta << '\n';
        std::cout << name << ": " << metric.columns() << std::endl;
        if (embDelta > 5e-4 || scoreDelta > .00100001)
            throw std::runtime_error("ONNX / KISA parity failed");
    }
    deepprint::writeNew(out, rows.str());
}
} // namespace research
