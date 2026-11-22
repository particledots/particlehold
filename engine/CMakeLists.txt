# The particlehold engine. Plain C++17, no JUCE, no plugin code: the gate from notes (Keys), the internal clock
# (Clock), the noise (Noise), the slew (Glide, on Curve), the CC stream (CcStream), the Clock output (Pulses) and the
# whole per-sample engine (Engine). Keys and Curve are Bryn 0.1.0's, unchanged except the namespace; CcStream is
# Bryn's with a rest value per lane; Pulses and Engine are adapted from Bryn.

add_library(particlehold_engine STATIC
    src/Keys.cpp
    src/CcStream.cpp
    src/Pulses.cpp
    src/Engine.cpp
)

target_include_directories(particlehold_engine PUBLIC include)
target_compile_features(particlehold_engine PUBLIC cxx_std_17)

if(PARTICLEHOLD_BUILD_TESTS)
    add_executable(particlehold_engine_test tests/engine_test.cpp)
    target_link_libraries(particlehold_engine_test PRIVATE particlehold_engine)
    add_test(NAME engine COMMAND particlehold_engine_test)

    # Writes the engine's real MIDI stream to stdout; the mapper test (Node.js) feeds it through the Scripter mapper.
    add_executable(particlehold_ccdump tests/ccdump.cpp)
    target_link_libraries(particlehold_ccdump PRIVATE particlehold_engine)

    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME mapper
                 COMMAND ${CMAKE_COMMAND} -E env
                         PARTICLEHOLD_CCDUMP=$<TARGET_FILE:particlehold_ccdump>
                         ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mapper_test.js
                         ${CMAKE_SOURCE_DIR}/tools/mapper/particlehold-Mapper.js
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Bryn-Mapper.js
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Hillside-Mapper.js
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Ogdoad-Mapper.js
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/StrangeDrift-Mapper.js)
    endif()
endif()
