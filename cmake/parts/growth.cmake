# Growth (D-26 section 4, package G4): transformation rules, forms and training values. The battle side
# (src/battle/growth_battle.cpp) and the rule lookup / Change scope (src/growth/growth.cpp) are battle modules
# (cmake/parts/battle.cmake); this part lists the defaults a game replaces at link time.

# The engine library's growth defaults: the empty tables of a program without battle data, the context values'
# port (the manifest's defaults) and the script statement's port (a Change over the attached State).
set(CAPSULE_GROWTH_SOURCES
   src/growth/growth_empty.cpp
   src/growth/growth_port_default.cpp
   # B1: the context values a game sets (a moon phase: growth::set_context_value()) and the empty combination tables
   src/growth/growth_context_store.cpp
   src/combine/combine_empty.cpp
   src/script/script_growth.cpp
   src/script/script_growth_ask.cpp
   # B1: the combine statements' port (a combine::Combination over the attached State, the text box's menus)
   src/script/script_combine.cpp
   # B2: the nursery statements' port (combine::nursery over the attached party, the party menu, the nickname prompt)
   src/script/script_nursery.cpp)

# What a program with the battle modules but without the engine library links (capsule_battle_library(): the tests,
# the bench, the reference repository's programs): the context values' port and the clock's default, in a static
# library so a program's own definitions win. F1: the minute clock's default and the timed field effects of a
# program without a script bank (none: effect logic's `field_effect start` refuses; a program that builds the script
# VM lists src/script/field_effects.cpp itself, which wins). F2: the game state field action requirements read
# (src/script/field_actions_none.cpp: none; the script VM's programs list src/script/field_actions.cpp).
set(CAPSULE_GROWTH_DEFAULT_SOURCES
   src/growth/growth_port_default.cpp
   src/growth/growth_context_store.cpp
   src/core/clock_default.cpp
   src/core/clock_minutes_default.cpp
   src/script/field_effects_none.cpp
   src/script/field_actions_none.cpp)

# The static library of CAPSULE_GROWTH_DEFAULT_SOURCES (made once).
function(capsule_growth_defaults)
   if(TARGET capsule_growth_defaults)
      return()
   endif()
   set(sources ${CAPSULE_GROWTH_DEFAULT_SOURCES})
   list(TRANSFORM sources PREPEND ${CAPSULE_ROOT}/)
   add_library(capsule_growth_defaults STATIC ${sources})
   target_include_directories(capsule_growth_defaults PUBLIC ${CAPSULE_ROOT}/include)
   target_link_libraries(capsule_growth_defaults PUBLIC capsule_options)
endfunction()
