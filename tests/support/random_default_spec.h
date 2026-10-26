/// @file
/// The engine default generator's description for tests/random_tests.cpp
/// (included inside its anonymous namespace, after Known and kKnownDraws): its
/// name, a reference of its formula written out independently, and its known
/// first draws. A ruleset repository with its own generator (the reference
/// repo's compat module) passes its own description as CAPSULE_RNG_TEST_SPEC.

/// Name printed in the summary.
constexpr const char* kName = "default";

/// Reference of the default generator (src/core/random_default.cpp), written
/// out independently: PCG RXS-M-XS 32/32 on the new state, top 16 bits.
/// @param state  generator state (advanced)
/// @return the draw
uint16_t reference(uint32_t& state)
{
   state = state * 747796405u + 2891336453u;
   uint32_t word = ((state >> ((state >> 28) + 4)) ^ state) * 277803737u;
   word ^= word >> 22;
   return static_cast<uint16_t>(word >> 16);
}

/// Known first draws of the default generator (computed from its definition).
constexpr Known kKnown[] = {
   {0x00000000u, {0x07BB, 0x22B6, 0x3BF6, 0x572F, 0x86FC, 0x3465, 0x6879, 0xDDE7}, 0x21A37198u},
   {0x12345678u, {0x9953, 0x3A39, 0x83FD, 0xB767, 0x1F23, 0xB00B, 0x9A48, 0x7B8F}, 0x7647AD10u},
};
