#pragma once
#include "onnx_model.h"
#include "feature/fingerprint_features.h"
#include "fvc_dataset.h"
#include "tiff_image.h"
#include "verification_metrics.h"
#include <array>
#include <functional>
#include <map>

namespace research
{
namespace fs = std::filesystem;
using Row = std::map<std::string, std::string>;
std::vector<Row> csv(const fs::path &path);
std::string basename(const std::string &value);
struct Metrics
{
    int genuine{}, impostor{};
    double eer{}, threshold{}, fmr{}, fnmr{};
    std::string columns() const;
};
Metrics measure(const std::vector<VerificationScore> &scores);
std::vector<float> imageInput(const fvc::GrayImage &image, const std::string &mode);
std::string deployed(int type);
struct Pair
{
    size_t a, b;
    bool genuine;
};
struct Bank
{
    std::string name;
    std::vector<fvc::Sample> samples;
    std::map<std::string, size_t> byName;
    std::vector<Pair> pairs;
};
using Banks = std::map<std::string, Bank>;
Banks dataset(const fs::path &root, const fs::path &pairs = {});
using Vectors = std::vector<fingerprint::Features>;
std::vector<VerificationScore> compare(const Vectors &gallery, const Vectors &probes,
                                       const std::vector<Pair> &pairs, double weight);
using Outputs = std::map<std::string, std::vector<float>>;
class Inference
{
  public:
    Inference(const fs::path &model, int threads = 4);
    ~Inference();
    Inference(const Inference &) = delete;
    Outputs run(std::vector<float> &input, const std::vector<std::string> &outputs);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
using Dimensions = std::vector<std::int64_t>;
void exposeOutputs(const fs::path &model, const fs::path &output,
                   const std::map<std::string, Dimensions> &names);
void verifyBaseline(const fs::path &fvc, const fs::path &pairs, const fs::path &dll, const fs::path &out,
                    const fs::path &onnx, double textureWeight = .22);
std::vector<VerificationScore> nativeScores(const fs::path &fvc, const Bank &bank, const fs::path &dll);
} // namespace research
