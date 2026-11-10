# M6a K1: the clocks (capsule/core/clock.h, src/core/clock.cpp; C-05) and the real-time clock HAL
# (capsule/hal/rtc.h): the C++ program on the engine library's headless HAL (its settable RTC) and the world
# clock's compiler and save layout tests. Included from CMakeLists.txt's test section.
if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM STREQUAL "headless")
   add_executable(capsule_clock_tests ${CAPSULE_ROOT}/tests/clock_tests.cpp)
   target_link_libraries(capsule_clock_tests PRIVATE capsule_engine)
   add_test(NAME clock_tests COMMAND capsule_clock_tests)
   add_test(NAME clock_manifest_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_clock_manifest.py)
endif()
