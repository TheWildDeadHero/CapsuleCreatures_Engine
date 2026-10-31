# UM (D-33): prints a GBA program's memory use after its link (capsule_game() runs it with cmake -P as a post-build
# step): EWRAM (.ewram + .sbss) and IWRAM (code, .bss, .data) against the regions, and the stack left below __sp_usr.
# The link itself fails when a limit is passed (cmake/gba_memory.ld); this report shows the margins of every link.
#   cmake -DNM=<arm-none-eabi-nm> -DELF=<program> -DRESERVE=<bytes> -P gba_memory_report.cmake

set(ewram_start 0x02000000)    # gba_cart.ld's ewram region
set(ewram_bytes 262144)        # 256 KB
set(iwram_start 0x03000000)    # its iwram region
set(iwram_bytes 32768)         # 32 KB

execute_process(COMMAND ${NM} ${ELF} OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
   message(FATAL_ERROR "gba_memory_report: ${NM} failed on ${ELF}")
endif()

# The value of one of the linker script's symbols (nm prints "<hex address> <kind> <name>").
function(symbol out name)
   string(REGEX MATCH "([0-9a-fA-F]+) [A-Za-z] ${name}\n" hit "${symbols}")
   if(NOT hit)
      message(FATAL_ERROR "gba_memory_report: ${ELF} has no ${name}")
   endif()
   math(EXPR value "0x${CMAKE_MATCH_1}")
   set(${out} ${value} PARENT_SCOPE)
endfunction()

symbol(sbss_end __sbss_end__)
symbol(iheap_start __iheap_start)
symbol(sp_usr __sp_usr)
math(EXPR ewram_used "${sbss_end} - ${ewram_start}")
math(EXPR ewram_free "${ewram_bytes} - ${ewram_used}")
math(EXPR iwram_used "${iheap_start} - ${iwram_start}")
math(EXPR stack "${sp_usr} - ${iheap_start}")
get_filename_component(name ${ELF} NAME)
set(verdict "")
if(ewram_free LESS 0 OR stack LESS RESERVE)
   set(verdict " - DOES NOT FIT (CAPSULE_GBA_MEMORY_CHECKS is OFF)")   # with the checks on, the link failed already
endif()
message("GBA memory (${name}): EWRAM ${ewram_used} of ${ewram_bytes} B (${ewram_free} B free); "
        "IWRAM code + data ${iwram_used} of ${iwram_bytes} B, stack ${stack} B (reserve ${RESERVE} B)${verdict}")
