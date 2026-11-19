/// @file
/// D-36 package 3 step 3B: a program on the engine library's default script dispatch (every module op the manifest
/// does not exclude), as a test that links the library without a game's own table does (tests/exclude_build). The
/// table's handlers reach the modules of their ops, so the link fails if an excluded module's op is still in it.

#include "script_dispatch.h"

/// Checks the table is there (the link is the test).
/// @return 0 (the first op is not a module op)
int main()
{
   return capsule::script::detail::kModuleOps[0] == nullptr ? 0 : 1;
}
