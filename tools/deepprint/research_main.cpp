#include "research_data.h"
#include "cache.h"
#include "studies.h"
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if ((argc == 7 || argc == 8) && std::wstring(argv[1]) == L"verify")
        {
            double weight = argc == 8 ? std::stod(argv[7]) : .22;
            if (!(weight >= 0 && weight <= 1))
                throw std::runtime_error("Weight must be between 0 and 1");
            research::verifyBaseline(argv[2], argv[3], argv[4], argv[6], argv[5], weight);
            return 0;
        }
        auto mode = argc > 1 ? std::wstring(argv[1]) : L"";
        if (mode == L"roi" && argc == 7)
        {
            research::roiStudy(argv[2], argv[3], argv[4], argv[5], argv[6]);
            return 0;
        }
        if ((mode == L"screen" || mode == L"align") && (argc == 8 || (mode == L"screen" && argc == 7)))
        {
            research::screen(argv[2], argv[3], argv[4], argv[5], argv[6], mode == L"align",
                             argc == 8 ? argv[7] : L"");
            return 0;
        }
        if (mode == L"cache" && argc >= 8)
        {
            std::vector<research::fs::path> manifests(argv + 7, argv + argc);
            int sigma = std::stoi(argv[5]);
            if ((sigma != 30 && sigma != 40) ||
                (std::wstring(argv[6]) != L"standard" && std::wstring(argv[6]) != L"pilots"))
                throw std::runtime_error("Expected max sigma 30 or 40, and standard or pilots");
            research::cacheEmbeddings(argv[2], argv[3], argv[4], manifests, sigma,
                                      std::wstring(argv[6]) == L"standard");
            return 0;
        }
        std::cerr
            << "deepprint_research verify FVC PAIRS.csv DLL MODEL.onnx NEW_RESULTS.csv [TEXTURE_WEIGHT]\n"
            << "deepprint_research screen MODEL FVC PAIRS SPLIT NEW_OUT [DLL]\n"
            << "deepprint_research align MODEL FVC PAIRS SPLIT NEW_OUT DLL\n"
            << "deepprint_research cache MODEL FVC OUT 30|40 standard|pilots AUGMENTATION_DIR...\n"
            << "deepprint_research roi STANDARD_PAIRS ROI_PAIRS ID_SPLIT CAPTURE_SPLIT NEW_OUT\n";
        return 2;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
