cmake_minimum_required(VERSION 3.20)

project(large_sdf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(large_sdf_core
  src/core/ObjExporter.cpp
  src/core/SdfHistory.cpp
  src/core/SdfVolume.cpp
  src/core/SparseSdfHistory.cpp
  src/core/SparseSdfVolume.cpp
  src/core/SurfaceMesh.cpp
)

target_include_directories(large_sdf_core PUBLIC src)

add_executable(large_sdf_demo
  src/tools/sdf_demo.cpp
)

target_link_libraries(large_sdf_demo PRIVATE large_sdf_core)

add_executable(large_sdf_tests
  src/tests/sdf_tests.cpp
)

target_link_libraries(large_sdf_tests PRIVATE large_sdf_core)
