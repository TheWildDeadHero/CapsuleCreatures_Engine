# Inventory, item use, save image and the script items port (D-26 section 2, package G2): their sources, shared
# by the engine library (CMakeLists.txt) and the tests (cmake/parts/inventory_tests.cmake). The save medium's
# backends are per platform (CMakeLists.txt: src/hal/gba/save_gba.cpp, src/hal/common/save_memory.cpp, the
# desktop's per-game src/hal/sdl2/save_file.cpp).

# Item use (src/inventory; G3: the bag itself is a battle module, cmake/parts/battle.cmake), the empty storage
# rules, the save image (src/save), the script bag statements' default port (C6: its paged menus, script_menu.cpp)
# and the game's time of day default.
set(CAPSULE_INVENTORY_SOURCES
   src/inventory/use.cpp
   src/inventory/inventory_empty.cpp
   src/storage/storage_empty.cpp
   src/save/save.cpp
   src/script/script_items.cpp
   src/script/script_boxes.cpp
   src/script/script_menu.cpp
   src/core/clock_default.cpp
   # F1: the minute clock's default (timed field effects counted in minutes)
   src/core/clock_minutes_default.cpp)

# What the script VM's bag and box statements link besides the VM (the items and boxes ports' defaults, the bag, the
# storage, the empty tables and the time of day): programs that build src/script/script.cpp themselves add these.
set(CAPSULE_SCRIPT_ITEMS_SOURCES
   src/script/script_items.cpp
   src/script/script_boxes.cpp
   src/script/script_menu.cpp
   src/inventory/bag.cpp
   src/inventory/inventory_empty.cpp
   src/storage/storage.cpp
   src/storage/storage_empty.cpp
   src/battle/battle_empty.cpp
   src/core/clock_default.cpp
   # G4: the transform statement's port of a program without battle data (nothing transforms)
   src/script/script_growth_none.cpp
   # B1: the combine statements' port of a program without battle data (nothing combines)
   src/script/script_combine_none.cpp
   # B2: the nursery statements' port of a program without battle data (no nursery)
   src/script/script_nursery_none.cpp
   # F1: the timed field effects (the FieldEffect statement) and the minute clock's default
   src/script/field_effects.cpp
   src/core/clock_minutes_default.cpp
   # F3: the step effects' counters (the overworld runner counts the walker's steps into them)
   src/script/step_effects.cpp)
