/// @file
/// The timed field effects in the save image (F1, D-27 section 2) on tests/data/field: their implicit variables are
/// saved in the Script section like any other (the save shape follows them), and a read stops the effects that do
/// not persist (glow) while the others come back (ward, lure). The script VM is not linked: the test writes the
/// variables in the bank's storage as the VM would.
#include "capsule/save/save.h"
#include "capsule/script/data.h"

#include "battle_ids.h"
#include "field_script_ids.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

/// A u16 variable's bytes in the bank's storage.
/// @param v  the variable
/// @return its first byte
uint8_t* bytes_of(script::VarId v)
{
   const script::VarInfo& vi = script::kBank.vars[static_cast<uint16_t>(v)];
   return script::kBank.var_storage[static_cast<int>(vi.lifetime)] + vi.offset;
}

/// Writes a u16 variable (little-endian, as the VM stores it).
/// @param v      the variable
/// @param value  its value
void put(script::VarId v, uint16_t value)
{
   uint8_t* p = bytes_of(v);
   p[0] = static_cast<uint8_t>(value & 0xFF);
   p[1] = static_cast<uint8_t>(value >> 8);
}

/// Reads a u16 variable.
/// @param v  the variable
/// @return its value
uint16_t get(script::VarId v)
{
   const uint8_t* p = bytes_of(v);
   return static_cast<uint16_t>(p[0] | p[1] << 8);
}

/// The effects' records and their save round trip.
void test_save()
{
   using script_ids::VAR_FIELD_EFFECT_GLOW;
   using script_ids::VAR_FIELD_EFFECT_LURE;
   using script_ids::VAR_FIELD_EFFECT_WARD;
   const script::Bank& b = script::kBank;
   CHECK(b.field_effect_count == battle_ids::NUM_FIELD_EFFECTS && b.field_effect_count == 3);
   CHECK(b.field_effects[battle_ids::FIELD_EFFECT_WARD].var == static_cast<uint16_t>(VAR_FIELD_EFFECT_WARD));
   CHECK(b.field_effects[battle_ids::FIELD_EFFECT_GLOW].expiry_script == script::kNoExpiryScript);
   CHECK(b.field_effects[battle_ids::FIELD_EFFECT_LURE].expiry_script ==
         static_cast<uint16_t>(script_ids::SCRIPT_FIELD_EFFECT_END_LURE));
   CHECK(b.vars[static_cast<uint16_t>(VAR_FIELD_EFFECT_WARD)].lifetime == script::Lifetime::Permanent);
   put(VAR_FIELD_EFFECT_WARD, 7);
   put(VAR_FIELD_EFFECT_GLOW, 9);
   put(VAR_FIELD_EFFECT_LURE, 400);
   const save::Sources none{nullptr, nullptr, nullptr, nullptr};
   CHECK(save::write(none) == save::Status::Ok);
   put(VAR_FIELD_EFFECT_WARD, 0);
   put(VAR_FIELD_EFFECT_LURE, 0);
   CHECK(save::read(none) == save::Status::Ok);
   CHECK(get(VAR_FIELD_EFFECT_WARD) == 7 && get(VAR_FIELD_EFFECT_LURE) == 400);
   CHECK(get(VAR_FIELD_EFFECT_GLOW) == 0);   // glow does not persist: a loaded save has it stopped
}

} // namespace

/// Runs the tests.
/// @return 0 when all pass
int main()
{
   test_save();
   std::printf("field_save_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
