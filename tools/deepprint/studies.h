#pragma once
#include "research_data.h"
namespace research
{
void screen(const fs::path &model, const fs::path &root, const fs::path &pairs, const fs::path &split,
            const fs::path &out, bool alignment, const fs::path &dll);
void tailStudy(const std::string &study, const fs::path &model, const fs::path &root, const fs::path &pairs,
               const fs::path &split, const fs::path &out, int steps, int batch);
void legacyStudy(const std::string &study, const fs::path &model, const fs::path &cache,
                 const fs::path &idSplit, const fs::path &captures, const fs::path &folds,
                 const fs::path &out, int maxEpoch);
} // namespace research
