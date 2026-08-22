cmake_minimum_required(VERSION 3.15)
project(schwung-6w6 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)          # the vendored 606 voices are C++14
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which would remove
# the safety of the clamps on the realtime path. The hat's 47-partial sum is
# the only hot spot and it is measured, not guessed.
set(CMAKE_C_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

set(SD606_SOURCES
    src/dsp/sd606_plugin.cpp
    src/dsp/sd606_engine.cpp
)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(sd606 SHARED ${SD606_SOURCES})
target_include_directories(sd606 PRIVATE src src/dsp src/host src/vendor/606)
target_link_libraries(sd606 PRIVATE m)
set_target_properties(sd606 PROPERTIES OUTPUT_NAME "dsp" PREFIX "" SUFFIX ".so")
# libstdc++ static: the Move ships 6.0.29 and we will not gamble on it.
target_link_options(sd606 PRIVATE -static-libstdc++ -static-libgcc)

# End-to-end loader test: dlopens the real dsp.so like the chain host does.
# Cross-compiled and run ON the Move.
add_executable(sd606_loadtest src/tools/loadtest.c)
target_include_directories(sd606_loadtest PRIVATE src src/dsp src/host)
target_link_libraries(sd606_loadtest PRIVATE dl m)

# CPU gate: renders each voice in isolation and reports realtime factor.
# Phase 1 exists because 3 metal voices sum 141 sines per sample.
add_executable(sd606_bench src/tools/bench.cpp src/dsp/sd606_engine.cpp)
target_include_directories(sd606_bench PRIVATE src src/dsp src/host src/vendor/606)
target_link_libraries(sd606_bench PRIVATE m)
target_link_options(sd606_bench PRIVATE -static-libstdc++ -static-libgcc)

# Offline renderer: same engine, writes WAVs so the kit can be heard and A/B'd
# against hardware without a device in the loop.
add_executable(sd606_render src/tools/render.cpp src/dsp/sd606_engine.cpp)
target_include_directories(sd606_render PRIVATE src src/dsp src/host src/vendor/606)
target_link_libraries(sd606_render PRIVATE m)
target_link_options(sd606_render PRIVATE -static-libstdc++ -static-libgcc)
