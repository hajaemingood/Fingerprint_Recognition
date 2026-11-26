#include "studies.h"
#include "torch_tensor.h"
#include <iostream>
int wmain(int argc, wchar_t **argv)
{
    using namespace research;
    try
    {
        auto mode = argc > 1 ? fs::path(argv[1]).u8string() : "";
        studytorch::THSTorch_set_num_threads(4);
        if ((mode == "branches" || mode == "augmentation") && argc >= 7 && argc <= 9)
        {
            int steps = argc >= 8 ? std::stoi(argv[7]) : 0, batch = argc == 9 ? std::stoi(argv[8]) : 8;
            if (steps < 0 || batch < 1)
                throw std::runtime_error("Invalid training schedule");
            tailStudy(mode, argv[2], argv[3], argv[4], argv[5], argv[6], steps, batch);
            return 0;
        }
        if ((mode == "heads" || mode == "last-id" || mode == "unseen") && (argc == 8 || argc == 9))
        {
            int epochs = argc == 9 ? std::stoi(argv[8]) : 12;
            if (epochs < 1)
                throw std::runtime_error("Expected positive epoch count");
            legacyStudy(mode, argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], epochs);
            return 0;
        }
        std::cerr
            << "deepprint_train branches|augmentation MODEL FVC PAIRS SPLIT NEW_OUT [STEP_LIMIT [TRIPLETS]]\n"
            << "deepprint_train heads|last-id|unseen MODEL CACHE ID_SPLIT CAPTURE_SPLIT FOLDS NEW_OUT "
               "[EPOCHS]\n";
        return 2;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
