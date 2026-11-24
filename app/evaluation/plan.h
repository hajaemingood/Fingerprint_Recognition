#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace evaluation {

namespace fs = std::filesystem;

enum class Rule { All, First, Impression, Random };

struct Selection {
    Rule rule = Rule::All;
    unsigned value = 1;
};

struct Settings {
    fs::path dll;
    fs::path database;
    fs::path templates;
    fs::path logs;
    fs::path reports;
    fs::path latestReport;
    fs::path pairFile;
    Selection enrollment;
    Selection genuine;
    Selection impostor;
    std::uint32_t seed = 42;
    unsigned workers = 1;
    unsigned timeoutMilliseconds = 30000;
    bool reportAccuracy = true;
    bool reportFailures = true;
    bool reportTiming = true;
};

struct Trial {
    std::string database;
    fs::path enrolled;
    fs::path probe;
    bool genuine;
};

std::vector<Trial> createPlan(const Settings& settings);

} // namespace evaluation
