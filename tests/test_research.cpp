#include "research_data.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        // A tied genuine / impostor score moves together when the threshold passes it.
        const auto overlap = research::measure({{.8, true}, {.8, false}, {.9, true}, {.7, false}});
        if (overlap.eer != .25 || overlap.threshold != .8 || overlap.fmr != .5 || overlap.fnmr != 0 ||
            overlap.genuine != 2 || overlap.impostor != 2)
            throw std::runtime_error("Research EER crossing / tie regression");
        const auto perfect = research::measure({{.901, true}, {.900, false}});
        if (perfect.eer != 0 || perfect.threshold != .901)
            throw std::runtime_error("Rounded-score acceptance boundary regression");
        deepprint::Message fields{{1, 0, 300, {}}, {2, 2, 0, std::string("a\0b", 3)}, {5, 5, 0, "abcd"}};
        const auto bytes = deepprint::encode(fields);
        if (deepprint::encode(deepprint::decode(bytes)) != bytes)
            throw std::runtime_error("ONNX wire-format round trip failed");
        try
        {
            deepprint::decode(std::string("\x12\x03\x01", 3));
        }
        catch (const std::runtime_error &)
        {
            return 0;
        }
        throw std::runtime_error("Truncated ONNX field accepted");
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
