# D-36 package 3 step 3B: the engine built with every excludable module left out (ctest exclude_build, run by
# ctest --build-and-test). The game is the script test app with a source that reaches the code with cut points (the
# save codec's boxes and nursery, the overworld's preview, the party role port), so its link shows the cut points
# hold; a second program links the library's default script dispatch with the cut points, as a test program does.
cmake_minimum_required(VERSION 3.21)
project(capsule_exclude_build CXX)

set(CAPSULE_ENGINE_DIR "" CACHE PATH "The engine's checkout")
set(CAPSULE_EXCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/game/reference/exclude.json CACHE FILEPATH "" FORCE)
add_subdirectory(${CAPSULE_ENGINE_DIR} engine)
capsule_game(${CMAKE_CURRENT_SOURCE_DIR}/game)

add_executable(capsule_exclude_default_dispatch game/src/default_dispatch.cpp game/src/cut_points.cpp)
target_include_directories(capsule_exclude_default_dispatch PRIVATE ${CAPSULE_ENGINE_DIR}/src/script)
target_link_libraries(capsule_exclude_default_dispatch PRIVATE capsule_engine)
