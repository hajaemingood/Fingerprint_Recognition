cmake_minimum_required(VERSION 3.16)
project(IndustryAlgorithm LANGUAGES CXX)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT fingerprint_compare)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8 /DNOMINMAX)
endif()

option(FINGERPRINT_DEEPPRINT "Build the DeepPrint KISA DLL alongside the evaluation tools" OFF)
option(FINGERPRINT_FVC2004_ROI "Crop foreground in DeepPrint KISA iType 3 inputs" ON)
option(FINGERPRINT_BUILD_GUI "Build the Windows KISA evaluation application" ON)
set(FINGERPRINT_ONNXRUNTIME_ROOT "${CMAKE_SOURCE_DIR}/build/dependencies/onnxruntime" CACHE PATH "Extracted Windows x64 ONNX Runtime NuGet package")
set(FINGERPRINT_DEEPPRINT_MODEL "${CMAKE_SOURCE_DIR}/build/models/deepprint_texminu_512.onnx" CACHE FILEPATH "Exported DeepPrint ONNX model")
set(FINGERPRINT_DEEPPRINT_REFERENCE_DIR "" CACHE PATH "Reference vectors from the DeepPrint exporter")

if(FINGERPRINT_DEEPPRINT)
    add_library(IndustryAlgorithm SHARED
        src/api/kisa_api.cpp
        src/feature/deepprint_extractor.cpp
        src/feature/deepprint_preprocessing.cpp
        src/matching/fingerprint_matcher.cpp
        src/feature/template_manager.cpp
    )
    target_include_directories(IndustryAlgorithm PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" PRIVATE src)
    target_compile_definitions(IndustryAlgorithm PRIVATE
        FINGERPRINT_FVC2004_ROI=$<BOOL:${FINGERPRINT_FVC2004_ROI}>)
    set_target_properties(IndustryAlgorithm PROPERTIES PREFIX "")
    if(MINGW)
        target_link_options(IndustryAlgorithm PRIVATE -static -static-libstdc++)
    endif()

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
    install(TARGETS IndustryAlgorithm RUNTIME DESTINATION .)
endif()

add_library(fingerprint_tiff STATIC tools/fvc/fvc_dataset.cpp tools/fvc/tiff_image.cpp)
target_include_directories(fingerprint_tiff PUBLIC tools/fvc PRIVATE include)
target_link_libraries(fingerprint_tiff PUBLIC ole32 windowscodecs uuid)

add_library(gaussian_noise STATIC tools/noise/gaussian_noise.cpp)
target_include_directories(gaussian_noise PUBLIC tools/noise)

add_library(fingerprint_augmentation STATIC tools/noise/image_transform.cpp)
target_include_directories(fingerprint_augmentation PUBLIC tools/noise)
target_link_libraries(fingerprint_augmentation PUBLIC fingerprint_tiff PRIVATE gaussian_noise)

add_library(verification_metrics STATIC tools/evaluation/verification_metrics.cpp)
target_include_directories(verification_metrics PUBLIC tools/evaluation)

if(FINGERPRINT_BUILD_GUI)
    add_executable(fingerprint_compare WIN32
        app/main.cpp app/comparison_window.cpp app/fingerprint_image.cpp)
    target_include_directories(fingerprint_compare PRIVATE include)
    target_compile_definitions(fingerprint_compare PRIVATE FINGERPRINT_DYNAMIC_BACKEND)
    target_link_libraries(fingerprint_compare PRIVATE ole32 windowscodecs uuid comdlg32 gdi32)
    if(FINGERPRINT_DEEPPRINT)
        add_dependencies(fingerprint_compare IndustryAlgorithm)
    endif()
    if(MSVC)
        target_compile_options(fingerprint_compare PRIVATE /utf-8)
    elseif(MINGW)
        target_link_options(fingerprint_compare PRIVATE -municode -static -static-libstdc++)
    endif()
    install(TARGETS fingerprint_compare RUNTIME DESTINATION .)
endif()

add_executable(augment_fvc tools/noise/main.cpp)
target_link_libraries(augment_fvc PRIVATE fingerprint_augmentation gaussian_noise)
add_executable(evaluate_noise tools/evaluation/noise_evaluation.cpp)
target_include_directories(evaluate_noise PRIVATE include)
target_link_libraries(evaluate_noise PRIVATE fingerprint_tiff verification_metrics)

add_executable(tune_weights tools/evaluation/weight_tuning.cpp
    src/matching/fingerprint_matcher.cpp src/feature/template_manager.cpp)
target_include_directories(tune_weights PRIVATE src)
target_link_libraries(tune_weights PRIVATE fingerprint_tiff verification_metrics)
if(MINGW)
    target_link_options(augment_fvc PRIVATE -municode -static)
    target_link_options(evaluate_noise PRIVATE -municode -static)
    target_link_options(tune_weights PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
    # Upstream's evaluation_workflow tests its alternative app/evaluation modules.
    # This branch retains the original five-file UI rather than those modules.
    add_executable(test_noise tests/test_noise.cpp)
    target_link_libraries(test_noise PRIVATE gaussian_noise fingerprint_augmentation verification_metrics)
    if(MINGW)
        target_link_options(test_noise PRIVATE -static)
    endif()
    add_test(NAME noise_and_metrics COMMAND test_noise)

    if(FINGERPRINT_DEEPPRINT AND FINGERPRINT_DEEPPRINT_REFERENCE_DIR)
        add_executable(test_deepprint tests/test_deepprint.cpp
            src/feature/deepprint_preprocessing.cpp src/feature/template_manager.cpp)
        target_include_directories(test_deepprint PRIVATE src)
        target_link_libraries(test_deepprint PRIVATE IndustryAlgorithm)
        target_compile_definitions(test_deepprint PRIVATE
            FINGERPRINT_FVC2004_ROI=$<BOOL:${FINGERPRINT_FVC2004_ROI}>)
        if(MINGW)
            target_link_options(test_deepprint PRIVATE -municode -static)
        endif()
        add_test(NAME deepprint_reference COMMAND test_deepprint "${FINGERPRINT_DEEPPRINT_REFERENCE_DIR}")
    endif()
endif()
