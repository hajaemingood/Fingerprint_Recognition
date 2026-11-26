#include "onnx_model.h"
#include "checkpoint.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace deepprint;

namespace
{
std::vector<float> materialize(std::ifstream &checkpoint, const Source &s, std::size_t count)
{
    if (s.operation == "copy")
        return load(checkpoint, s.kernel, count);
    if (s.operation != "bias" && s.operation != "kernel")
        throw std::runtime_error("Unknown tensor operation");
    auto gamma = load(checkpoint, s.gamma, s.channels);
    auto variance = load(checkpoint, s.variance, s.channels);
    for (std::size_t c = 0; c < s.channels; ++c)
        gamma[c] /= std::sqrt(variance[c] + 0.001f);
    if (s.operation == "bias")
    {
        if (count != s.channels)
            throw std::runtime_error("Invalid bias shape");
        auto bias = load(checkpoint, s.beta, count);
        auto mean = load(checkpoint, s.mean, count);
        for (std::size_t c = 0; c < count; ++c)
            bias[c] -= mean[c] * gamma[c];
        return bias;
    }
    if (count % s.channels)
        throw std::runtime_error("Invalid convolution shape");
    auto weights = load(checkpoint, s.kernel, count);
    const auto stride = count / s.channels;
    for (std::size_t c = 0; c < s.channels; ++c)
        for (std::size_t i = 0; i < stride; ++i)
            weights[c * stride + i] *= gamma[c];
    return weights;
}

void convert(const fs::path &checkpointPath, const fs::path &architecture, const fs::path &output)
{
    if (sha256(checkpointPath) != checkpointHash)
        throw std::runtime_error("Unexpected checkpoint SHA-256");
    std::ifstream checkpoint(checkpointPath, std::ios::binary);
    auto sources = readLayout(architecture / "parameters.tsv");
    auto model = decode(readFile(architecture / "texminu.graph"));
    auto graph = decode(field(model, 7).bytes);
    for (auto &f : graph)
    {
        if (f.number != 5)
            continue; // GraphProto.initializer
        auto tensor = decode(f.bytes);
        auto source = sources.find(field(tensor, 8).bytes);
        if (source == sources.end())
        {
            (void)field(tensor, 9); // Fixed filter or resize coefficients already present.
            continue;
        }
        std::size_t count = 1;
        for (const auto &dim : tensor)
            if (dim.number == 1)
            {
                if (!dim.integer || dim.integer > 70000000 / count)
                    throw std::runtime_error("Invalid shape");
                count *= dim.integer;
            }
        auto data = materialize(checkpoint, source->second, count);
        tensor.push_back(
            {9, 2, 0, std::string(reinterpret_cast<const char *>(data.data()), count * sizeof(float))});
        f.bytes = encode(tensor);
        sources.erase(source);
    }
    if (!sources.empty())
        throw std::runtime_error("Checkpoint layout differs from model graph");
    field(model, 7).bytes = encode(graph);
    graph.clear();
    writeNew(output, encode(model));
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: export_deepprint CHECKPOINT.pyt ARCHITECTURE_DIR NEW_OUTPUT.onnx\n";
        return 2;
    }
    try
    {
        const fs::path output = argv[3];
        auto metadata = output;
        metadata.replace_extension(".json");
        if (output == metadata || fs::exists(output) || fs::exists(metadata))
            throw std::runtime_error("Choose new ONNX and metadata output paths");
        convert(argv[1], argv[2], output);
        const auto hash = sha256(output);
        try
        {
            writeNew(
                metadata,
                std::string("{\n  \"upstream_revision\": \"7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13\",\n") +
                    "  \"checkpoint_sha256\": \"" + checkpointHash + "\",\n  \"onnx_sha256\": \"" + hash +
                    "\",\n" +
                    "  \"input\": [1,1,299,299],\n  \"embedding\": [\"texture:256\",\"minutiae:256\"],\n" +
                    "  \"binarization\": {\"ridge_width\": 1.8}\n}\n");
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove(output, ignored);
            throw;
        }
        std::cout << "Exported 394 learned tensors. ONNX SHA-256 " << hash << '\n';
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
