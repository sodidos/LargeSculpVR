cmake_minimum_required(VERSION 3.22)

project(large_quest_sdf LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LARGE_USE_OPENXR "Link against the OpenXR loader for Quest builds." ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../" ABSOLUTE)

add_library(large_sdf_core STATIC
    "${REPO_ROOT}/src/core/ObjExporter.cpp"
    "${REPO_ROOT}/src/core/SdfHistory.cpp"
    "${REPO_ROOT}/src/core/SdfVolume.cpp"
    "${REPO_ROOT}/src/core/SurfaceMesh.cpp"
)

target_include_directories(large_sdf_core PUBLIC
    "${REPO_ROOT}/src"
)

add_library(native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
)

target_include_directories(native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue"
)

target_link_libraries(native_app_glue PUBLIC
    android
    log
)

add_library(large_quest_sdf SHARED
    QuestSdfApp.cpp
)

target_link_options(large_quest_sdf PRIVATE
    "-u" "ANativeActivity_onCreate"
)

target_link_libraries(large_quest_sdf PRIVATE
    large_sdf_core
    native_app_glue
    android
    log
    EGL
    GLESv3
    aaudio
)

if(LARGE_USE_OPENXR)
    target_compile_definitions(large_quest_sdf PRIVATE
        LARGE_USE_OPENXR=1
        XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_OPENGL_ES
    )

    find_package(OpenXR REQUIRED CONFIG)
    target_link_libraries(large_quest_sdf PRIVATE OpenXR::openxr_loader)
else()
    target_compile_definitions(large_quest_sdf PRIVATE LARGE_USE_OPENXR=0)
endif()
