# N1 (D-32) tests: the per-frame gameplay draw (rng::set_frame_advance(), tests/n1_tests.cpp) with the default
# generator. Included from CMakeLists.txt's test section, after the N2 part.

add_executable(capsule_n1_tests ${CAPSULE_ROOT}/tests/n1_tests.cpp ${CAPSULE_ROOT}/${CAPSULE_RNG_STREAM_SOURCES})
capsule_rng(capsule_n1_tests default)
target_include_directories(capsule_n1_tests PRIVATE ${CAPSULE_ROOT}/include)
target_link_libraries(capsule_n1_tests PRIVATE capsule_options)
add_test(NAME n1_tests COMMAND capsule_n1_tests)
