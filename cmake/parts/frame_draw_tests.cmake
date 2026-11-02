# per-frame draw tests: the per-frame gameplay draw (rng::set_frame_advance(), tests/frame_draw_tests.cpp) with the default
# generator. Included from CMakeLists.txt's test section, after the map view part.

add_executable(capsule_frame_draw_tests ${CAPSULE_ROOT}/tests/frame_draw_tests.cpp ${CAPSULE_ROOT}/${CAPSULE_RNG_STREAM_SOURCES})
capsule_rng(capsule_frame_draw_tests default)
target_include_directories(capsule_frame_draw_tests PRIVATE ${CAPSULE_ROOT}/include)
target_link_libraries(capsule_frame_draw_tests PRIVATE capsule_options)
add_test(NAME frame_draw_tests COMMAND capsule_frame_draw_tests)
