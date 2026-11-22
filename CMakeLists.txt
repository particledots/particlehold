cmake_minimum_required(VERSION 3.22)

# macOS build settings. These must be set before project().
# Universal binary (Apple Silicon + Intel) so the same build runs on the M4 Mac mini
# and on the Intel Mac. Deployment target 11.0 = macOS Big Sur or newer.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build for Apple Silicon and Intel")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# The version is the plugin's version in Logic. Raise it with every milestone: macOS and Logic cache a plugin's
# details and only refresh them when the installed version goes up.
project(particlehold VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PARTICLEHOLD_BUILD_PLUGIN "Build the AU/VST3 plugin (needs JUCE)" ON)
option(PARTICLEHOLD_BUILD_TESTS  "Build the engine self-tests and the mapper test" ON)
option(PARTICLEHOLD_BUILD_CHECK  "Build particleholdCheck, the host-less test of the plugin layer (needs JUCE)" OFF)

if(PARTICLEHOLD_BUILD_TESTS)
    enable_testing()
endif()

# 1) The engine: plain C++, no plugin framework. The clock, noise, sample and hold, slew, note tracking, the CC stream and the Clock output.
#    Adapted from Bryn 0.1.0.
add_subdirectory(engine)

# 2) The plugin wrapper, controls and panel (JUCE).
if(PARTICLEHOLD_BUILD_PLUGIN OR PARTICLEHOLD_BUILD_CHECK)
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.17-security
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
    if(PARTICLEHOLD_BUILD_PLUGIN)
        add_subdirectory(plugin)
    endif()
    if(PARTICLEHOLD_BUILD_CHECK)
        add_subdirectory(check)
    endif()
endif()
