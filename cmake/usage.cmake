# D-36 usage reports (tools/usage.py, docs/USAGE.md): each compiler a game's build runs writes a usage record
# (capsule.usage/1) into usage/<game>/ in the build folder, the VM bank tools theirs (capsule.vm-usage/1), and the
# target capsule_<game>_usage (part of ALL) merges them into usage/<game>.json (capsule.usage-report/1) with the
# engine libraries the project links, the platform and the HAL sources of the engine library. capsule_game() sets it
# up; a caller that compiles more for a game (its banks, its audio table) adds the records with capsule_usage_add().
# After the report, the exclusion step (package 3) writes usage/<game>.exclude.json and checks the game's manifest.
# Included from CMakeLists.txt before capsule_game().

# The folder a game's usage records go to.
# @param out   the variable to set
# @param game  the game's name (project.json "name")
function(capsule_usage_dir out game)
   set(${out} ${CMAKE_BINARY_DIR}/usage/${game} PARENT_SCOPE)
endfunction()

# Adds usage records to a game's report (files some custom command of the game's directory writes).
# @param target  the game's target (capsule_<name>)
# @param ARGN    the records
function(capsule_usage_add target)
   set_property(TARGET ${target} APPEND PROPERTY CAPSULE_USAGE_RECORDS ${ARGN})
endfunction()

# The engine library's HAL sources (src/hal/...), relative to the engine, for a usage report.
# @param out  the variable to set
function(capsule_usage_hal out)
   get_target_property(engine_sources capsule_engine SOURCES)
   set(hal)
   foreach(src ${engine_sources} ${CAPSULE_RUNTIME_SOURCES})
      if(NOT IS_ABSOLUTE ${src})
         set(src ${CAPSULE_ROOT}/${src})
      endif()
      file(RELATIVE_PATH rel ${CAPSULE_ROOT} ${src})
      if(rel MATCHES "^src/hal/" AND NOT rel IN_LIST hal)
         list(APPEND hal ${rel})
      endif()
   endforeach()
   set(${out} ${hal} PARENT_SCOPE)
endfunction()

# The merge step of a game's usage report (capsule_game() defers it to the end of its caller's directory, so the
# records added after capsule_game() are in it).
# @param target  the game's target
# @param game    the game's name
function(capsule_usage_report target game)
   get_target_property(records ${target} CAPSULE_USAGE_RECORDS)
   if(NOT records)
      set(records)
   endif()
   get_target_property(libraries ${target} CAPSULE_USAGE_LIBRARIES)
   if(NOT libraries)
      set(libraries)
   endif()
   capsule_usage_hal(hal)
   set(out ${CMAKE_BINARY_DIR}/usage/${game}.json)
   add_custom_command(
      OUTPUT  ${out}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/usage.py merge ${records} --game ${game}
            --platform ${CAPSULE_PLATFORM} --out ${out} --libraries ${libraries} --hal ${hal}
      DEPENDS ${CAPSULE_ROOT}/tools/usage.py ${records}
      COMMENT "Merging the usage report of ${game}"
      VERBATIM)

   # D-36 package 3 (tools/usage_exclude.py): the exclusion manifest of what the game can leave out, from the report
   # and the game's own objects (their references to engine modules, which no compiler records), and, when the
   # game's project names one ("exclude"), its check: an excluded item the game uses fails the build
   get_target_property(objects ${target} CAPSULE_USAGE_OBJECTS)
   get_target_property(exclude ${target} CAPSULE_EXCLUDE)
   capsule_usage_nm(nm)
   set(suggested ${CMAKE_BINARY_DIR}/usage/${game}.exclude.json)
   set(outputs ${suggested})
   set(check_command)
   if(exclude)
      set(findings ${CMAKE_BINARY_DIR}/usage/${game}.exclude-check.txt)
      list(APPEND outputs ${findings})
      set(check_command COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/usage.py exclude-check ${exclude} ${out}
                              --objects ${objects} --nm ${nm} --out ${findings})
   else()
      set(exclude)
   endif()
   add_custom_command(
      OUTPUT  ${outputs}
      COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tools/usage.py exclude ${out} --objects ${objects} --nm ${nm}
            --out ${suggested}
      ${check_command}
      DEPENDS ${CAPSULE_ROOT}/tools/usage.py ${CAPSULE_ROOT}/tools/usage_exclude.py
            ${CAPSULE_ROOT}/src/script/script_vm.json ${out} ${exclude} ${target}
      COMMENT "Checking what ${game} can exclude"
      COMMAND_EXPAND_LISTS
      VERBATIM)
   add_custom_target(${target}_usage ALL DEPENDS ${out} ${outputs})
   add_dependencies(${target}_usage ${target})   # the records are written while the game's sources are generated
endfunction()

# Adds object files or archives the exclusion step reads for a game's references to engine modules (besides the
# game target's own objects, which capsule_game() adds): a library of the game's own code that it links.
# @param target  the game's target (capsule_<name>)
# @param ARGN    the files (generator expressions allowed, such as $<TARGET_FILE:lib>)
function(capsule_usage_objects target)
   set_property(TARGET ${target} APPEND PROPERTY CAPSULE_USAGE_OBJECTS ${ARGN})
endfunction()

# The nm the exclusion step runs: the compiler's gcc-nm beside its gcc-ar when there is one (it reads link-time
# optimized objects), else CMAKE_NM.
# @param out  the variable to set
function(capsule_usage_nm out)
   set(nm ${CMAKE_NM})
   if(CMAKE_CXX_COMPILER_AR MATCHES "gcc-ar[^/]*$")
      string(REGEX REPLACE "gcc-ar([^/]*)$" "gcc-nm\\1" gcc_nm "${CMAKE_CXX_COMPILER_AR}")
      if(EXISTS ${gcc_nm})
         set(nm ${gcc_nm})
      endif()
   endif()
   set(${out} ${nm} PARENT_SCOPE)
endfunction()
