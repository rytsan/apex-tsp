cmake_minimum_required(VERSION 3.28)
project(apex_tsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(APEX_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data" CACHE PATH "Directory with TSPLIB instances")

include(FetchContent)
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_GetProperties(doctest)
if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
endif()

# Warnings-as-errors everywhere (C0.3).
add_library(apex_warnings INTERFACE)
if(MSVC)
    target_compile_options(apex_warnings INTERFACE /W4 /WX /permissive-)
else()
    target_compile_options(apex_warnings INTERFACE -Wall -Wextra -Werror)
endif()

add_library(apex_core STATIC
    src/core/instance.cpp
    src/core/tour_io.cpp
    src/geo/hilbert.cpp
    src/geo/grid.cpp
    src/geo/delaunay.cpp
    src/graph/candidate_graph.cpp
    src/primal/two_opt.cpp
    src/bnc/checkpoint.cpp
    src/lp/csc.cpp
    src/lp/lu.cpp
    src/lp/eta.cpp
    src/lp/dual_simplex.cpp
    src/lp/mps.cpp)
target_include_directories(apex_core PUBLIC src)
target_link_libraries(apex_core PUBLIC apex_warnings)

add_executable(engine src/app/main.cpp)
set_target_properties(engine PROPERTIES OUTPUT_NAME apex_tsp)
target_link_libraries(engine PRIVATE apex_core)

enable_testing()
add_executable(engine_tests
    tests/test_main.cpp
    tests/test_instance.cpp
    tests/test_hilbert.cpp
    tests/test_grid.cpp
    tests/test_delaunay.cpp
    tests/test_candidate_graph.cpp
    tests/test_two_opt.cpp
    tests/test_tour_io.cpp
    tests/test_app.cpp
    tests/test_csc.cpp
    tests/test_lu.cpp
    tests/test_eta.cpp
    tests/test_dual_simplex.cpp
    tests/test_mps.cpp)
target_include_directories(engine_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
target_compile_definitions(engine_tests PRIVATE APEX_DATA_DIR="${APEX_DATA_DIR}")
target_link_libraries(engine_tests PRIVATE apex_core)
add_test(NAME engine_tests COMMAND engine_tests)
