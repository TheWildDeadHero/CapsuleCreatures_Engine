# M6a K2: the plots (capsule/plots/plots.h, src/plots/plots.cpp; B-5): the C++ program on the engine library's
# headless HAL (growth on the economy clock, its settable RTC) and the plots' compiler and save layout tests.
# Included from CMakeLists.txt's test section.
if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM STREQUAL "headless")
   add_executable(capsule_plots_tests ${CAPSULE_ROOT}/tests/plots_tests.cpp)
   target_link_libraries(capsule_plots_tests PRIVATE capsule_engine)
   add_test(NAME plots_tests COMMAND capsule_plots_tests)
   add_test(NAME plots_manifest_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_plots_manifest.py)
endif()
