#pragma once

#include "run.h"

namespace evaluation {

void writePlan(const fs::path& path, const std::vector<Trial>& trials);
void writeLog(const fs::path& path, const std::vector<Trial>& trials, const std::vector<Outcome>& outcomes);
void writeReport(const RunResult& result, const Settings& settings, const fs::path& templates,
                 const std::vector<Trial>& trials, const std::vector<Outcome>& outcomes);
void publishReport(const fs::path& from, const fs::path& to);

} // namespace evaluation
