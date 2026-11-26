#pragma once
#include "research_data.h"
namespace research
{
struct EmbeddingBank
{
    std::vector<std::string> names;
    Vectors vectors;
    std::vector<double> timings;
};
EmbeddingBank readCache(const fs::path &path);
void cacheEmbeddings(const fs::path &model, const fs::path &fvc, const fs::path &output,
                     const std::vector<fs::path> &manifests, int maxSigma, bool standardOnly);
std::string conditionName(const Row &row);
std::vector<std::vector<std::string>> settings(int maxSigma);
void roiStudy(const fs::path &standard, const fs::path &roi, const fs::path &split, const fs::path &captures,
              const fs::path &output);
} // namespace research
