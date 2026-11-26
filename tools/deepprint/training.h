#pragma once
#include "torch_tensor.h"
#include "research_data.h"
#include <random>

namespace research
{
using studytorch::I;
using studytorch::Tensor;
struct Config
{
    std::string arm = "A", mode = "original", loss = "triplet", negatives = "hard";
    int rank = 0, steps = 12;
    double rate = .0001, strength = 1., weight = .22;
};
inline constexpr char textureInput[] = "/texture/texture.4/Flatten_output_0";
inline constexpr char minutiaInput[] = "/minutia_embedding/_2_flatten/Flatten_output_0";
inline constexpr char textureBlock[] = "/texture/texture.2/texture.2.1/Concat_output_0";
inline constexpr char minutiaBlock[] = "/minutia_embedding/_0_block/_0_block.2/Conv_output_0";
struct Inputs
{
    Tensor embed, texture, minutia, tb, mc;
    Inputs get(const std::vector<I> &indices, const std::string &arm) const;
};
struct FoldBank
{
    std::vector<I> train;
    std::vector<unsigned> fingers;
    std::map<std::string, Inputs> images;
};
using FoldBanks = std::map<std::string, FoldBank>;
struct Weights
{
    std::map<std::string, Tensor> tensors;
    explicit Weights(const fs::path &model);
    void loadTextureBlock(const fs::path &checkpoint, const fs::path &layout);
    void save(const fs::path &source, const fs::path &output,
              const std::map<std::string, Tensor> &delta) const;
};
class Tail
{
    Config config;
    std::map<std::string, Tensor> original, learned;
    Tensor projection(const Tensor &x, int i, double strength, bool residual) const;
    Tensor conv(const Tensor &x, int index) const;

  public:
    Tail(const Weights &w, Config c, std::mt19937 &rng);
    Tensor predict(const Inputs &input, double strength) const;
    std::map<std::string, Tensor> merged(double strength) const;
    std::vector<Tensor> parameters() const;
    Tail frozen() const;
};
using Observe = std::function<void(int, const Tail &)>;
void fit(Tail &tail, const Config &config, const FoldBanks &banks, int seed, int batch,
         const Observe &observe);
Vectors embeddings(const Tail &tail, const Inputs &input, const std::vector<I> &indices, const Config &config,
                   size_t total);
std::vector<I> allIndices(size_t count);
} // namespace research
