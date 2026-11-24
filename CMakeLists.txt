cmake_minimum_required(VERSION 3.16)
project(IndustryAlgorithm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FINGERPRINT_DEEPPRINT "Build the KISA DLL with the DeepPrint ONNX model" OFF)
option(FINGERPRINT_BUILD_GUI "Build the existing fingerprint comparison GUI" ON)
set(FINGERPRINT_ONNXRUNTIME_ROOT "" CACHE PATH "Extracted Windows x64 ONNX Runtime NuGet package")
set(FINGERPRINT_DEEPPRINT_MODEL "" CACHE FILEPATH "Exported DeepPrint ONNX model")
set(FINGERPRINT_DEEPPRINT_REFERENCE_DIR "" CACHE PATH "Reference vectors from the DeepPrint exporter")

set(feature_extractor src/feature/feature_extractor.cpp)
if(FINGERPRINT_DEEPPRINT)
    set(feature_extractor
        src/feature/deepprint_extractor.cpp
        src/feature/deepprint_preprocessing.cpp
    )
endif()

add_library(IndustryAlgorithm SHARED
    src/api/kisa_api.cpp
    ${feature_extractor}
    src/matching/fingerprint_matcher.cpp
    src/feature/template_manager.cpp
)
target_include_directories(IndustryAlgorithm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE src
)
set_target_properties(IndustryAlgorithm PROPERTIES PREFIX "")
if(MINGW)
    target_link_options(IndustryAlgorithm PRIVATE -static -static-libstdc++)
endif()

if(FINGERPRINT_DEEPPRINT)
    set(ort_include "${FINGERPRINT_ONNXRUNTIME_ROOT}/build/native/include")
    set(ort_runtime "${FINGERPRINT_ONNXRUNTIME_ROOT}/runtimes/win-x64/native")
    set(ort_dll "${ort_runtime}/onnxruntime.dll")
    if(NOT FINGERPRINT_ONNXRUNTIME_ROOT OR NOT EXISTS "${ort_include}/onnxruntime_cxx_api.h"
       OR NOT EXISTS "${ort_dll}" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DeepPrint requires a Windows x64 build and FINGERPRINT_ONNXRUNTIME_ROOT pointing to the extracted Microsoft.ML.OnnxRuntime NuGet package")
    endif()
    if(NOT EXISTS "${FINGERPRINT_DEEPPRINT_MODEL}")
        message(FATAL_ERROR "Set FINGERPRINT_DEEPPRINT_MODEL to the ONNX file from tools/deepprint/export_model.py")
    endif()

    target_compile_definitions(IndustryAlgorithm PRIVATE FINGERPRINT_DEEPPRINT)
    target_include_directories(IndustryAlgorithm SYSTEM PRIVATE "${ort_include}")
    set_property(TARGET IndustryAlgorithm APPEND PROPERTY LINK_DEPENDS
        "${FINGERPRINT_DEEPPRINT_MODEL}" "${ort_dll}")
    add_custom_command(TARGET IndustryAlgorithm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FINGERPRINT_DEEPPRINT_MODEL}"
            "$<TARGET_FILE_DIR:IndustryAlgorithm>/deepprint_texminu_512.onnx"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ort_dll}" "$<TARGET_FILE_DIR:IndustryAlgorithm>"
        VERBATIM
    )
    install(FILES "${FINGERPRINT_DEEPPRINT_MODEL}" DESTINATION . RENAME deepprint_texminu_512.onnx)
    install(FILES "${ort_dll}" DESTINATION .)
endif()

if(FINGERPRINT_BUILD_GUI)
    add_executable(fingerprint_compare WIN32
        app/main.cpp
        app/comparison_window.cpp
        app/fingerprint_image.cpp
    )
    target_link_libraries(fingerprint_compare PRIVATE IndustryAlgorithm ole32 windowscodecs uuid comdlg32 gdi32)
    if(MSVC)
        target_compile_options(fingerprint_compare PRIVATE /utf-8)
    elseif(MINGW)
        target_link_options(fingerprint_compare PRIVATE -municode -static -static-libstdc++)
    endif()
    install(TARGETS fingerprint_compare RUNTIME DESTINATION .)
endif()

install(TARGETS IndustryAlgorithm RUNTIME DESTINATION .)

add_library(fingerprint_tiff STATIC tools/fvc/fvc_dataset.cpp tools/fvc/tiff_image.cpp)
target_include_directories(fingerprint_tiff PUBLIC tools/fvc PRIVATE include)
target_link_libraries(fingerprint_tiff PUBLIC ole32 windowscodecs uuid)

add_library(gaussian_noise STATIC tools/noise/gaussian_noise.cpp)
target_include_directories(gaussian_noise PUBLIC tools/noise)

add_library(verification_metrics STATIC tools/evaluation/verification_metrics.cpp)
target_include_directories(verification_metrics PUBLIC tools/evaluation)

add_executable(augment_fvc tools/noise/main.cpp)
target_link_libraries(augment_fvc PRIVATE fingerprint_tiff gaussian_noise)
add_executable(evaluate_noise tools/evaluation/noise_evaluation.cpp)
target_include_directories(evaluate_noise PRIVATE include)
target_link_libraries(evaluate_noise PRIVATE fingerprint_tiff verification_metrics)
if(MINGW)
    target_link_options(augment_fvc PRIVATE -municode -static)
    target_link_options(evaluate_noise PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_noise tests/test_noise.cpp)
    target_link_libraries(test_noise PRIVATE gaussian_noise verification_metrics)
    if(MINGW)
        target_link_options(test_noise PRIVATE -static)
    endif()
    add_test(NAME noise_and_metrics COMMAND test_noise)

    if(NOT FINGERPRINT_DEEPPRINT)
        add_executable(test_pipeline tests/test_pipeline.cpp)
        target_link_libraries(test_pipeline PRIVATE IndustryAlgorithm)
        if(MINGW)
            target_link_options(test_pipeline PRIVATE -static -static-libstdc++)
        endif()
        add_test(NAME fingerprint_pipeline COMMAND test_pipeline)
    elseif(FINGERPRINT_DEEPPRINT_REFERENCE_DIR)
        add_executable(test_deepprint tests/test_deepprint.cpp
            src/feature/deepprint_preprocessing.cpp src/feature/template_manager.cpp)
        target_compile_definitions(test_deepprint PRIVATE FINGERPRINT_DEEPPRINT)
        target_include_directories(test_deepprint PRIVATE src)
        target_link_libraries(test_deepprint PRIVATE IndustryAlgorithm)
        if(MINGW)
            target_link_options(test_deepprint PRIVATE -municode -static)
        endif()
        add_test(NAME deepprint_reference COMMAND test_deepprint "${FINGERPRINT_DEEPPRINT_REFERENCE_DIR}")
    endif()
endif()
