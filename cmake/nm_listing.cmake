# D-36 (tools/usage.py linkmap): a program's sized symbols, mangled (`nm -S --defined-only`), for the usage report's
# link comparison; exact under link-time optimization, where the linker map keeps few symbols.
# Run with: cmake -DNM=<nm> -DELF=<program> -DOUT=<listing> -P nm_listing.cmake
execute_process(COMMAND ${NM} -S --defined-only ${ELF} OUTPUT_FILE ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
   message(WARNING "nm_listing.cmake: ${NM} failed on ${ELF} (${rc}); no symbol listing")
endif()
