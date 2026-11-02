# M5 VX (D-39): VM extension manifests (capsule.vm-ext/1, docs/VM_EXT.md). tools/vm_ext.py checks a game's
# manifests and writes its VMs' dispatch from them (ROM tables of alias steps and one small function per alias,
# include/capsule/core/vm_ext.h); this file gives games the two CMake functions and runs the tool's own tests.
# Included from CMakeLists.txt before anim.cmake and battle_vm.cmake (whose tests and sandbox use it).

# The tool, for games that call the functions below from their own folders (a global, like a function).
set(CAPSULE_VM_EXT_TOOL "${CMAKE_CURRENT_LIST_DIR}/../../tools/vm_ext.py" CACHE INTERNAL "tools/vm_ext.py")

# capsule_vm_dispatch(OUT <file> MANIFESTS <json>... [USAGE <json>...] [EXCLUSIONS <json>]): a VM's dispatch
# generated at build time from its manifests (with USAGE: the ops no bank uses left out, listed in EXCLUSIONS).
function(capsule_vm_dispatch)
   cmake_parse_arguments(arg "" "OUT;EXCLUSIONS" "MANIFESTS;USAGE" ${ARGN})
   set(extra)
   if(arg_USAGE)
      list(APPEND extra --usage ${arg_USAGE})
   endif()
   set(outputs ${arg_OUT})
   if(arg_EXCLUSIONS)
      list(APPEND extra --exclusions ${arg_EXCLUSIONS})
      list(APPEND outputs ${arg_EXCLUSIONS})
   endif()
   add_custom_command(
      OUTPUT  ${outputs}
      COMMAND Python3::Interpreter ${CAPSULE_VM_EXT_TOOL} dispatch ${arg_MANIFESTS} --out ${arg_OUT} ${extra}
      DEPENDS ${CAPSULE_VM_EXT_TOOL} ${arg_MANIFESTS} ${arg_USAGE}
      COMMENT "Generating VM dispatch ${arg_OUT}"
      VERBATIM)
endfunction()

# capsule_vm_ext_check(<test name> MANIFESTS <json>... [SOURCES <cpp>...] [USAGE <json>...]): a test running the
# manifests' checks (D-39 §3.3: reasons, implementations, declared usage; warnings fail it).
function(capsule_vm_ext_check name)
   cmake_parse_arguments(arg "" "" "MANIFESTS;SOURCES;USAGE" ${ARGN})
   set(extra)
   if(arg_SOURCES)
      list(APPEND extra --sources ${arg_SOURCES})
   endif()
   if(arg_USAGE)
      list(APPEND extra --usage ${arg_USAGE})
   endif()
   add_test(NAME ${name} COMMAND Python3::Interpreter ${CAPSULE_VM_EXT_TOOL} check ${arg_MANIFESTS} ${extra}
            --warnings-as-errors)
endfunction()

if(CAPSULE_ENGINE_TESTS AND CAPSULE_PLATFORM MATCHES "^(desktop|headless)$")
   add_test(NAME vm_ext_tests COMMAND Python3::Interpreter ${CAPSULE_ROOT}/tests/test_vm_ext.py)
endif()
