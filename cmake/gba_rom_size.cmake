# D-5 (D-40 RS): checks a GBA ROM against its game's cartridge target after the build (capsule_game() runs it with
# cmake -P as a post-build step, after gbafix). A game fits the smallest cartridge its content allows; the target is
# the manifest's "rom": {"cart_mb": N}, one of the D-5 sizes (8, 16 or 32 MB), 32 MB (the GBA's whole ROM window)
# when the manifest names none. The build fails when the ROM is larger, and every build prints the margin.
#   cmake -DROM=<game>.gba -DCART_MB=<8|16|32> -P gba_rom_size.cmake

cmake_minimum_required(VERSION 3.24)   # script mode: the presets' minimum (if(IN_LIST) needs policy CMP0057)

set(bytes_per_mb 1048576)     # 1 MB = 2^20 bytes
set(cart_sizes_mb 8 16 32)    # D-5: 32 MB at most, 16 or 8 MB when the content allows

if(NOT CART_MB IN_LIST cart_sizes_mb)
   message(FATAL_ERROR "gba_rom_size: cart_mb ${CART_MB} is not a D-5 cartridge size (${cart_sizes_mb})")
endif()
if(NOT EXISTS ${ROM})
   message(FATAL_ERROR "gba_rom_size: ${ROM} does not exist")
endif()

file(SIZE ${ROM} rom_bytes)
math(EXPR cart_bytes "${CART_MB} * ${bytes_per_mb}")
math(EXPR free_bytes "${cart_bytes} - ${rom_bytes}")
get_filename_component(name ${ROM} NAME)
if(free_bytes LESS 0)
   math(EXPR over_bytes "0 - ${free_bytes}")
   message(FATAL_ERROR "GBA ROM (${name}): ${rom_bytes} B is ${over_bytes} B over its ${CART_MB} MB cartridge "
                       "target (project.json rom.cart_mb, D-5): shrink the content or raise the target")
endif()
message("GBA ROM (${name}): ${rom_bytes} of ${cart_bytes} B (${CART_MB} MB cartridge, ${free_bytes} B free)")
