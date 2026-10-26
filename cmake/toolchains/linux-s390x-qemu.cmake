# Big-endian check (P-19): cross-compiles the headless platform for s390x Linux
# and runs the tests under QEMU user-mode emulation. s390x is used because it
# is a big-endian target with stock cross compilers and QEMU in Debian/Ubuntu:
#   sudo apt-get install g++-s390x-linux-gnu qemu-user
# Used by the big-endian-check preset and CI; not a shipping target.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)
set(CAPSULE_PLATFORM headless)

set(CMAKE_C_COMPILER   s390x-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER s390x-linux-gnu-g++)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-s390x -L /usr/s390x-linux-gnu)

set(CMAKE_FIND_ROOT_PATH /usr/s390x-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
