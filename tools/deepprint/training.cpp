#include "training.h"
#include "checkpoint.h"
#include "matching/fingerprint_matcher.h"
#include <algorithm>
#include <numeric>
#include <set>

namespace research
{
using namespace studytorch;
namespace
{
const std::array<std::string, 2> heads{"texture.6", "minutia_embedding._4_linear"};
const std::string lastConv = "minutia_embedding._0_block.3";
std::string layer(int n)
{
    return "onnx::Conv_" + std::to_string(n);
}
Tensor convolution(const Tensor &x, const Tensor &w, const Tensor &b, I stride)
{
    I pad[2] = {w.size(2) / 2, w.size(3) / 2}, dilation = 1;
    return Tensor(THSTensor_conv2d(x.h(), w.h(), b.h(), &stride, 1, pad, 2, &dilation, 1, 1));
}
Tensor pool(const Tensor &x, I kernel, I stride, I padding, bool maximum)
{
    I dilation = 1;
    if (maximum)
        return Tensor(THSTensor_max_pool2d(x.h(), &kernel, 1, &stride, 1, &padding, 1, &dilation, 1, false));
    return Tensor(THSTensor_avg_pool2d(x.h(), &kernel, 1, &stride, 1, &padding, 1, false, false, 0));
}
bool residualArm(const std::string &arm)
{
    return arm.rfind("residual-", 0) == 0;
}
bool active(const std::string &arm, int branch)
{
    return arm == "A" || arm == "both" || arm == "residual-both" ||
           (branch == 0 && (arm == "B" || arm == "texture" || arm == "residual-texture")) ||
           (branch == 1 && (arm == "C" || arm == "minutia" || arm == "residual-minutia"));
}
Tensor optionalRows(const Tensor &v, const std::vector<I> &indices)
{
    return v ? select(v, indices) : Tensor{};
}
} // namespace
std::vector<I> allIndices(size_t n)
{
    std::vector<I> ids(n);
    std::iota(ids.begin(), ids.end(), 0);
    return ids;
}
Inputs Inputs::get(const std::vector<I> &indices, const std::string &arm) const
{
    return {select(embed, indices), optionalRows(texture, indices), optionalRows(minutia, indices),
            arm == "B" ? optionalRows(tb, indices) : Tensor{},
            arm == "C" ? optionalRows(mc, indices) : Tensor{}};
}

Weights::Weights(const fs::path &model)
{
    using namespace deepprint;
    auto proto = decode(readFile(model));
    auto graph = decode(field(proto, 7).bytes);
    proto.clear();
    for (auto &f : graph)
        if (f.number == 5)
        {
            auto t = decode(f.bytes);
            auto name = field(t, 8).bytes;
            bool used = false;
            for (auto &head : heads)
                used |= name.rfind(head + ".", 0) == 0;
            used |= name.rfind(lastConv + ".", 0) == 0;
            for (int i = 2507; i <= 2534; i += 3)
                used |= name == layer(i) || name == layer(i + 1);
            if (!used)
                continue;
            auto &raw = field(t, 9).bytes;
            std::vector<float> vals(raw.size() / 4);
            std::memcpy(vals.data(), raw.data(), vals.size() * 4);
            Dimensions dims;
            for (auto &d : t)
                if (d.number == 1)
                    dims.push_back(d.integer);
            tensors[name] = tensor(vals, dims);
        }
}
void Weights::save(const fs::path &source, const fs::path &output,
                   const std::map<std::string, Tensor> &delta) const
{
    using namespace deepprint;
    auto proto = decode(readFile(source));
    auto graph = decode(field(proto, 7).bytes);
    field(proto, 7).bytes.clear();
    size_t updated = 0;
    for (auto &f : graph)
        if (f.number == 5)
        {
            auto t = decode(f.bytes);
            auto it = delta.find(field(t, 8).bytes);
            if (it == delta.end())
                continue;
            auto v = it->second.values();
            auto &raw = field(t, 9).bytes;
            if (raw.size() != v.size() * 4)
                throw std::runtime_error("Candidate tensor size mismatch");
            std::memcpy(raw.data(), v.data(), raw.size());
            f.bytes = encode(t);
            ++updated;
        }
    if (updated != delta.size() || !updated)
        throw std::runtime_error("Missing candidate initializer");
    field(proto, 7).bytes = encode(graph);
    graph.clear();
    writeNew(output, encode(proto));
}

void Weights::loadTextureBlock(const fs::path &path, const fs::path &layout)
{
    if (deepprint::sha256(path) != deepprint::checkpointHash)
        throw std::runtime_error("Unexpected branch-training checkpoint");
    auto table = deepprint::readLayout(layout);
    std::ifstream file(path, std::ios::binary);
    for (int i = 2507; i <= 2534; i += 3)
    {
        auto name = layer(i);
        auto &desc = table.at(name);
        auto &fused = tensors.at(name);
        Dimensions dims{fused.size(), fused.size(1), fused.size(2), fused.size(3)};
        auto kernel = deepprint::load(file, desc.kernel,
                                      std::accumulate(dims.begin(), dims.end(), I{1}, std::multiplies<I>()));
        auto gamma = deepprint::load(file, desc.gamma, desc.channels),
             var = deepprint::load(file, desc.variance, desc.channels);
        auto mean = deepprint::load(file, desc.mean, desc.channels),
             beta = deepprint::load(file, desc.beta, desc.channels);
        auto exported = fused.values();
        auto bias = tensors.at(layer(i + 1)).values();
        size_t stride = kernel.size() / desc.channels;
        std::vector<float> inverse(desc.channels);
        for (size_t j = 0; j < desc.channels; ++j)
        {
            inverse[j] = 1 / std::sqrt(var[j] + .001f);
            double scale = gamma[j] * inverse[j];
            for (size_t k = 0; k < stride; ++k)
                if (std::abs(exported[j * stride + k] - kernel[j * stride + k] * scale) > 3e-5)
                    throw std::runtime_error("Checkpoint / ONNX kernel mismatch");
            if (std::abs(bias[j] - (beta[j] - mean[j] * scale)) > 3e-5)
                throw std::runtime_error("Checkpoint / ONNX bias mismatch");
        }
        tensors[name + ".raw"] = studytorch::tensor(kernel, dims);
        for (auto &entry : std::map<std::string, std::vector<float>>{
                 {".gamma", gamma}, {".beta", beta}, {".mean", mean}, {".var", var}, {".inv", inverse}})
            tensors[name + entry.first] = studytorch::tensor(entry.second, {I(desc.channels)});
    }
}

Tail::Tail(const Weights &w, Config c, std::mt19937 &rng) : config(std::move(c)), original(w.tensors)
{
    auto copy = [&](const std::string &name) { learned[name] = original.at(name).snapshot(); };
    for (int i = 0; i < 2; ++i)
        if (active(config.arm, i))
        {
            auto key = heads[i];
            if (config.rank)
            {
                I width = residualArm(config.arm) ? 256 : original.at(key + ".weight").size(1);
                std::uniform_real_distribution<float> init(-1 / std::sqrt(float(width)),
                                                           1 / std::sqrt(float(width)));
                std::vector<float> vals(config.rank * width);
                for (auto &v : vals)
                    v = init(rng);
                learned[key + ".down"] = tensor(vals, {config.rank, width});
                learned[key + ".up"] = tensor(std::vector<float>(256 * config.rank), {256, config.rank});
            }
            else
            {
                copy(key + ".weight");
                copy(key + ".bias");
            }
        }
    if (config.arm == "B")
        for (int i = 2507; i <= 2534; i += 3)
            for (auto suffix : {".raw", ".gamma", ".beta"})
                copy(layer(i) + suffix);
    if (config.arm == "C")
    {
        copy(lastConv + ".weight");
        copy(lastConv + ".bias");
    }
    if (learned.empty())
        throw std::runtime_error("No trainable parameters for " + config.arm);
    for (auto &[name, value] : learned)
        value.trainable();
}

Tensor Tail::projection(const Tensor &x, int i, double strength, bool residual) const
{
    auto k = heads[i];
    auto base = residual ? x : linear(x, original.at(k + ".weight"), original.at(k + ".bias"));
    if (!active(config.arm, i))
        return norm(base);
    if (config.rank)
        return norm(base + linear(linear(x, learned.at(k + ".down")), learned.at(k + ".up")) * strength);
    auto changed = linear(x, learned.at(k + ".weight"), learned.at(k + ".bias"));
    return norm(strength == 1 ? changed : base + (changed - base) * strength);
}
Tensor Tail::conv(const Tensor &x, int index) const
{
    auto k = layer(index);
    auto y = convolution(x, learned.at(k + ".raw"), {}, 1);
    return relu(
        Tensor(THSNN_batch_norm(y.h(), original.at(k + ".mean").h(), original.at(k + ".var").h(),
                                learned.at(k + ".gamma").h(), learned.at(k + ".beta").h(), false, 0, .001)));
}
Tensor Tail::predict(const Inputs &in, double strength) const
{
    if (residualArm(config.arm))
        return cat({projection(in.embed.slice(1, 0, 256), 0, strength, true),
                    projection(in.embed.slice(1, 256, 512), 1, strength, true)},
                   1);
    auto tx = in.texture, mi = in.minutia;
    if (config.arm == "B")
    {
        auto b1 = conv(in.tb, 2510), b2 = conv(conv(conv(in.tb, 2519), 2522), 2525);
        auto merged = cat({conv(in.tb, 2507), conv(b1, 2513), conv(b1, 2516), conv(b2, 2528), conv(b2, 2531),
                           conv(pool(in.tb, 3, 1, 1, false), 2534)},
                          1);
        tx = pool(merged, 8, 8, 0, false).reshape({in.tb.size(), 1536});
    }
    else if (config.arm == "C")
    {
        auto c = convolution(in.mc, learned.at(lastConv + ".weight"), learned.at(lastConv + ".bias"), 2);
        mi = pool(c, 9, 1, 0, true).reshape({in.mc.size(), 1024});
    }
    return cat({config.arm == "C" ? in.embed.slice(1, 0, 256) : projection(tx, 0, strength, false),
                config.arm == "B" ? in.embed.slice(1, 256, 512) : projection(mi, 1, strength, false)},
               1);
}
std::map<std::string, Tensor> Tail::merged(double strength) const
{
    std::map<std::string, Tensor> result;
    for (auto &[key, w] : learned)
        if (original.count(key) && key.rfind("onnx::Conv_", 0) != 0)
            result[key] = (original.at(key) + (w - original.at(key)) * strength).snapshot();
    if (config.arm == "B")
        for (int i = 2507; i <= 2534; i += 3)
        {
            auto k = layer(i);
            auto scale = learned.at(k + ".gamma") * original.at(k + ".inv");
            result[k] = (learned.at(k + ".raw") * scale.reshape({scale.size(), 1, 1, 1})).snapshot();
            result[layer(i + 1)] = (learned.at(k + ".beta") - original.at(k + ".mean") * scale).snapshot();
        }
    if (config.rank)
        for (int i = 0; i < 2; ++i)
            if (active(config.arm, i))
            {
                auto k = heads[i];
                auto change = mm(learned.at(k + ".up"), learned.at(k + ".down")) * strength;
                auto w = original.at(k + ".weight"), b = original.at(k + ".bias");
                if (residualArm(config.arm))
                {
                    result[k + ".weight"] = (w + mm(change, w)).snapshot();
                    result[k + ".bias"] = (b + mm(change, b.reshape({256, 1})).reshape({256})).snapshot();
                }
                else
                    result[k + ".weight"] = (w + change).snapshot();
            }
    return result;
}
std::vector<Tensor> Tail::parameters() const
{
    std::vector<Tensor> r;
    for (auto &p : learned)
        r.push_back(p.second);
    return r;
}
Tail Tail::frozen() const
{
    auto copy = *this;
    for (auto &[key, value] : copy.learned)
        value = learned.at(key).snapshot();
    return copy;
}

namespace
{
struct TripletPool
{
    std::vector<I> positives, hard, random;
};
I pick(const std::vector<I> &v, std::mt19937 &rng)
{
    if (v.empty())
        throw std::runtime_error("No eligible triplet samples");
    return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
}
Tensor contrastive(const Tensor &trained, const std::vector<I> &shots, const std::vector<unsigned> &ids,
                   float w)
{
    auto t = trained.slice(1, 0, 256), m = trained.slice(1, 256, 512);
    auto logits = (mm(t, transpose(t)) * w + mm(m, transpose(m)) * (1 - w)) * (1 / .07f);
    const I n = shots.size();
    std::vector<float> mask(n * n), positives(n * n);
    std::vector<I> activeRows;
    for (I i = 0; i < n; ++i)
    {
        int count = 0;
        for (I j = 0; j < n; ++j)
        {
            if (shots[i] == shots[j])
                mask[i * n + j] = -1e9f;
            else if (ids[shots[i]] == ids[shots[j]])
            {
                positives[i * n + j] = 1;
                ++count;
            }
        }
        if (count)
        {
            activeRows.push_back(i);
            for (I j = 0; j < n; ++j)
                positives[i * n + j] /= count;
        }
    }
    auto allowed = logits + tensor(mask, {n, n});
    return mean(
        select(Tensor(THSTensor_logsumexp(allowed.h(), 1, false)) - sum1(logits * tensor(positives, {n, n})),
               activeRows));
}
} // namespace
void fit(Tail &tail, const Config &config, const FoldBanks &banks, int seed, int batch,
         const Observe &observe)
{
    std::mt19937 rng(seed), aug(seed + 1);
    std::map<std::string, std::map<I, TripletPool>> pools;
    auto params = tail.parameters();
    std::vector<Tensor> before;
    for (auto &p : params)
        before.push_back(p.snapshot());
    AdamW optimizer(params, config.rate);
    for (auto &[db, bank] : banks)
    {
        auto flat = bank.images.at("original").embed.values();
        Vectors base(flat.size() / 512);
        std::memcpy(base.data(), flat.data(), flat.size() * 4);
        for (auto a : bank.train)
        {
            auto &pool = pools[db][a];
            std::vector<std::pair<double, I>> ranked;
            for (auto j : bank.train)
                if (j != a)
                {
                    if (bank.fingers.at(a) == bank.fingers.at(j))
                        pool.positives.push_back(j);
                    else
                    {
                        pool.random.push_back(j);
                        double score = fingerprint::fuseBranchScores(
                            fingerprint::compareBranches(base[a], base[j]), config.weight);
                        if (residualArm(config.arm))
                            score = std::round(score * 1000) / 1000;
                        ranked.emplace_back(score, j);
                    }
                }
            std::stable_sort(ranked.begin(), ranked.end(), [](auto a, auto b) { return a.first < b.first; });
            for (size_t k = ranked.size() - std::min<size_t>(6, ranked.size()); k < ranked.size(); ++k)
                pool.hard.push_back(ranked[k].second);
        }
    }
    for (int step = 1; step <= config.steps; ++step)
    {
        optimizer.clear();
        for (auto &[db, bank] : banks)
        {
            std::vector<I> ids(3 * batch);
            for (int k = 0; k < batch; ++k)
            {
                I a = ids[k] = pick(bank.train, rng);
                auto &pool = pools.at(db).at(a);
                ids[batch + k] = pick(pool.positives, rng);
                ids[2 * batch + k] =
                    pick(config.negatives == "mixed" && rng() % 2 ? pool.random : pool.hard, rng);
            }
            auto &orig = bank.images.at("original");
            auto inputs = orig.get(ids, config.arm);
            if (config.mode != "original")
            {
                std::vector<std::string> choices;
                for (auto &[name, unused] : bank.images)
                    if (name != "original" && (config.mode == "all" || name == config.mode ||
                                               (config.mode == "gaussian" && name.find("noise-") == 0) ||
                                               (config.mode == "rotation" && name.find("rotation") == 0)))
                        choices.push_back(name);
                if (choices.empty())
                    throw std::runtime_error("Missing augmentation " + config.mode);
                std::vector<std::string> modes(ids.size(), "original");
                if (residualArm(config.arm))
                {
                    std::vector<std::string> noise, pose;
                    for (auto &key : choices)
                        (key.rfind("noise-", 0) == 0 ? noise : pose).push_back(key);
                    std::bernoulli_distribution apply(.75), useNoise(db == "FVC2002_DB2_B" ? .75 : .35);
                    for (int k = 0; k < batch; ++k)
                        if (apply(aug))
                        {
                            const auto &pool =
                                config.mode == "all" ? (useNoise(aug) ? noise : pose) : choices;
                            if (pool.empty())
                                throw std::runtime_error("Incomplete augmentation pool");
                            modes[batch + k] = modes[2 * batch + k] = pool[aug() % pool.size()];
                        }
                }
                else
                {
                    auto changed = allIndices(batch * 2);
                    std::shuffle(changed.begin(), changed.end(), aug);
                    for (int j = 0; j < (batch * 3) / 2; ++j)
                        modes[batch + changed[j]] = config.mode;
                }
                std::vector<Inputs> variants;
                for (size_t k = 0; k < ids.size(); ++k)
                {
                    auto &source = bank.images.at(modes[k]);
                    variants.push_back(source.get({ids[k]}, config.arm));
                }
                auto join = [&](Tensor Inputs::*part) {
                    std::vector<Tensor> v;
                    for (auto &sample : variants)
                        if (sample.*part)
                            v.push_back(sample.*part);
                    return v.empty() ? Tensor{} : cat(v, 0);
                };
                inputs = {join(&Inputs::embed), join(&Inputs::texture), join(&Inputs::minutia),
                          join(&Inputs::tb), join(&Inputs::mc)};
            }
            auto y = tail.predict(inputs, 1);
            auto a = y.slice(0, 0, batch), p = y.slice(0, batch, 2 * batch),
                 n = y.slice(0, 2 * batch, 3 * batch);
            auto separation = config.loss == "supcon"
                                  ? contrastive(y, ids, bank.fingers, config.weight)
                                  : mean(relu(number(.12f) + similarity(a, n, config.weight) -
                                              similarity(a, p, config.weight)));
            auto drift = a - inputs.embed.slice(0, 0, batch);
            auto objective = (separation + mean(sum1(drift * drift)) * .05f) * (1.f / banks.size());
            if (!std::isfinite(objective.values().at(0)))
                throw std::runtime_error("Nonfinite objective");
            THSTensor_backward(objective.h());
            check();
        }
        if (step == 1)
            for (auto &p : params)
            {
                auto grad = Tensor(THSTensor_grad(p.h())).values();
                if (!std::all_of(grad.begin(), grad.end(), [](float v) { return std::isfinite(v); }))
                    throw std::runtime_error("Nonfinite parameter gradient");
            }
        optimizer.step();
        if (observe)
            observe(step, tail);
    }
    bool updated = false;
    for (size_t i = 0; i < params.size(); ++i)
    {
        auto values = params[i].values();
        if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }))
            throw std::runtime_error("Optimizer produced a nonfinite parameter");
        if (!updated)
            updated = values != before[i].values();
    }
    if (!updated)
        throw std::runtime_error("No selected parameter changed");
}
Vectors embeddings(const Tail &tail, const Inputs &in, const std::vector<I> &indices, const Config &config,
                   size_t total)
{
    Vectors result(total);
    for (size_t offset = 0; offset < indices.size(); offset += 4)
    {
        std::vector<I> group(indices.begin() + offset,
                             indices.begin() + std::min(indices.size(), offset + 4));
        auto flat = tail.predict(in.get(group, config.arm), config.strength).values();
        for (size_t j = 0; j < group.size(); ++j)
            std::copy_n(flat.begin() + j * 512, 512, result.at(group[j]).begin());
    }
    return result;
}
} // namespace research
