cmake_minimum_required(VERSION 3.16)
project(IndustryAlgorithm LANGUAGES CXX)

add_library(IndustryAlgorithm SHARED
    src/api/kisa_api.cpp
    src/feature/feature_extractor.cpp
    src/matching/fingerprint_matcher.cpp
    src/feature/template_manager.cpp
)
target_compile_features(IndustryAlgorithm PRIVATE cxx_std_17)
target_include_directories(IndustryAlgorithm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE src
)
set_target_properties(IndustryAlgorithm PROPERTIES PREFIX "")
if(MINGW)
    target_link_options(IndustryAlgorithm PRIVATE -static -static-libstdc++)
endif()

add_executable(fingerprint_compare WIN32
    app/main.cpp
    app/comparison_window.cpp
    app/fingerprint_image.cpp
)
target_compile_features(fingerprint_compare PRIVATE cxx_std_17)
target_link_libraries(fingerprint_compare PRIVATE IndustryAlgorithm ole32 windowscodecs uuid comdlg32 gdi32)
if(MSVC)
    target_compile_options(fingerprint_compare PRIVATE /utf-8)
elseif(MINGW)
    target_link_options(fingerprint_compare PRIVATE -municode -static -static-libstdc++)
endif()

install(TARGETS IndustryAlgorithm fingerprint_compare RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_pipeline tests/test_pipeline.cpp)
    target_compile_features(test_pipeline PRIVATE cxx_std_17)
    target_link_libraries(test_pipeline PRIVATE IndustryAlgorithm)
    if(MINGW)
        target_link_options(test_pipeline PRIVATE -static -static-libstdc++)
    endif()
    add_test(NAME fingerprint_pipeline COMMAND test_pipeline)
endif()
