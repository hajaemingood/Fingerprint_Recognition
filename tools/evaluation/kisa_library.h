#pragma once

#include "kisa_api.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <type_traits>

class KisaLibrary {
    struct ReleaseLibrary {
        void operator()(HMODULE module) const { FreeLibrary(module); }
    };

    std::unique_ptr<std::remove_pointer_t<HMODULE>, ReleaseLibrary> module_;

    template <class Function>
    Function find(const char* name)
    {
        const auto address = GetProcAddress(module_.get(), name);
        if (!address) {
            throw std::runtime_error(std::string("Missing DLL export: ") + name);
        }
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(address));
        return function;
    }

public:
    explicit KisaLibrary(const std::filesystem::path& path)
        : module_(LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        if (!module_) {
            const auto error = GetLastError();
            throw std::runtime_error("Cannot load " + path.u8string() + " (Windows error "
                                     + std::to_string(error) + ")");
        }
        enroll = find<decltype(enroll)>("KISA_Enroll");
        attempt = find<decltype(attempt)>("KISA_Attempt");
        compare = find<decltype(compare)>("KISA_Compare");
    }

    decltype(&KISA_Enroll) enroll;
    decltype(&KISA_Attempt) attempt;
    decltype(&KISA_Compare) compare;
};
