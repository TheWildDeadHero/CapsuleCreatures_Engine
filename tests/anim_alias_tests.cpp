/// @file
/// Tests for the animation player's aliases (capsule/anim/player.h run_alias(), capsule/core/vm_ext.h, D-39). The
/// manifest tests/data/vm_ext/anim_alias_vm_ext.json declares the game opcodes; tools/vm_ext.py writes their
/// command table (anim_alias_dispatch.inc, included below); tools/anim_bank.py assembles
/// tests/data/vm_ext/anim_alias_bank.canim with the manifest's signatures. Parity: the same scripts run on a table
/// of hand-written natives of the same layouts, and after every frame both players, both logs and both sets of
/// variables must be identical.
#include "capsule/anim/bank.h"
#include "capsule/anim/player.h"
#include "capsule/core/vm_ext.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern const uint8_t kAnimAliasBank[];        ///< The assembled bank (generated).
extern const uint32_t kAnimAliasBank_size;    ///< Its bytes.

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
}   // namespace capsule::hal::system

namespace
{

using namespace capsule;
namespace ext = capsule::vm_ext;

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

constexpr int kMarks = 32;          ///< Marks a log keeps.
constexpr int kFrames = 64;         ///< Frames a parity run takes at most.
constexpr int kFadeAt = 5;          ///< The frame the test sets the fade state.
constexpr uint8_t kFadedOut = 2;    ///< The fade state waitfade waits for.

/// The game state the scripts read and write (the manifest's refs: turn 0, fade 1, flag 2, pair 3).
struct World
{
   uint8_t turn;        ///< ref turn
   uint8_t fade[2];     ///< ref fade (2 bytes: copypair's source)
   uint8_t flag;        ///< ref flag
   uint8_t pair[2];     ///< ref pair
   int16_t marks[kMarks];   ///< mark's log.
   int mark_count;      ///< Its entries.
};

World g_alias_world;    ///< The aliases' world.
World g_native_world;   ///< The natives' world.
World* g_world = &g_alias_world;   ///< The world the running player works on.

/// @name The manifest's refs
///@{
constexpr uint32_t kRefTurn = 0;   ///< turn
constexpr uint32_t kRefFade = 1;   ///< fade
constexpr uint32_t kRefFlag = 2;   ///< flag
constexpr uint32_t kRefPair = 3;   ///< pair
///@}

/// The aliases' resolver.
/// @param ref    the manifest's ref number
/// @param bytes  bytes read or written
/// @return them, or nullptr
uint8_t* resolve_ref(anim::Player&, uint32_t ref, uint16_t bytes)
{
   World& w = *g_world;
   switch (ref)
   {
   case kRefTurn:
      return bytes <= sizeof(w.turn) ? &w.turn : nullptr;
   case kRefFade:
      return bytes <= sizeof(w.fade) ? w.fade : nullptr;
   case kRefFlag:
      return bytes <= sizeof(w.flag) ? &w.flag : nullptr;
   case kRefPair:
      return bytes <= sizeof(w.pair) ? w.pair : nullptr;
   default:
      return nullptr;
   }
}

/// mark: logs a value (the one native).
/// @param p  the player
void native_mark(anim::Player& p)
{
   ++p.pc;
   const int16_t v = anim::read_s16(p);
   if (p.active && g_world->mark_count < kMarks)
   {
      g_world->marks[g_world->mark_count++] = v;
   }
}

// The aliases' command table, generated from the manifest (kAliasCommands).
#include "anim_alias_dispatch.inc"

/// @name The same commands written as natives (the reference for parity, the way a game wrote them before D-39)
///@{
void ref_spin(anim::Player&)
{
}

void ref_ifturn(anim::Player& p)
{
   ++p.pc;
   const uint8_t turn = anim::read_u8(p);
   const uint32_t a = anim::read_address(p);
   if (p.active && turn == g_world->turn)
   {
      anim::jump(p, a);
   }
}

void ref_choose(anim::Player& p)
{
   ++p.pc;
   const uint32_t first = anim::read_address(p);
   const uint32_t second = anim::read_address(p);
   if (p.active)
   {
      anim::jump(p, (g_world->turn & 1u) != 0 ? second : first);
   }
}

void ref_waitfade(anim::Player& p)
{
   if (g_world->fade[0] == kFadedOut)
   {
      ++p.pc;
      return;
   }
   anim::yield(p);
}

void ref_ifflag(anim::Player& p)
{
   ++p.pc;
   const uint32_t a = anim::read_address(p);
   if (p.active && g_world->flag != 0)
   {
      anim::jump(p, a);
   }
}

void ref_setturn(anim::Player& p)
{
   ++p.pc;
   const uint8_t v = anim::read_u8(p);
   if (p.active)
   {
      g_world->turn = v;
   }
}

void ref_copypair(anim::Player& p)
{
   ++p.pc;
   std::memcpy(g_world->pair, g_world->fade, sizeof(g_world->pair));
}

void ref_ifpair(anim::Player& p)
{
   ++p.pc;
   const uint32_t a = anim::read_address(p);
   if (p.active && std::memcmp(g_world->pair, g_world->fade, sizeof(g_world->pair)) == 0)
   {
      anim::jump(p, a);
   }
}

void ref_goto2(anim::Player& p)
{
   ++p.pc;
   const uint32_t a = anim::read_address(p);
   anim::yield(p);
   if (p.active)
   {
      anim::jump(p, a);
   }
}
///@}

/// The natives' table.
constexpr anim::CommandTable make_native_table()
{
   anim::CommandTable t = anim::generic_table();
   constexpr anim::Command kGame[] = {native_mark,  ref_spin,    ref_ifturn,   ref_choose,  ref_waitfade,
                                      ref_ifflag,   ref_setturn, ref_copypair, ref_ifpair,  ref_goto2};
   for (uint8_t i = 0; i < sizeof(kGame) / sizeof(kGame[0]); ++i)
   {
      t = anim::with(t, static_cast<uint8_t>(anim::kFirstGameOpcode + i), kGame[i]);
   }
   return t;
}

constexpr anim::CommandTable kNativeCommands = make_native_table();   ///< The natives.
const anim::Tables kNoTables = {};                                    ///< No symbols.
const anim::Game kAliasGame = {kAliasCommands.at, &kNoTables, nullptr};    ///< The aliases' game.
const anim::Game kNativeGame = {kNativeCommands.at, &kNoTables, nullptr};  ///< The natives' game.

/// Two players in the same state.
/// @param a  one
/// @param b  the other
/// @return true when they agree
bool same_player(const anim::Player& a, const anim::Player& b)
{
   return a.pc == b.pc && a.command_pc == b.command_pc && a.frames_to_wait == b.frames_to_wait &&
          a.waiting == b.waiting && a.active == b.active && a.error == b.error && a.depth == b.depth &&
          a.frames == b.frames && a.commands == b.commands;
}

/// Runs a script on both tables frame by frame, comparing players and worlds after each frame.
/// @param bank    the bank
/// @param script  its name
/// @return frames run until both stopped (or kFrames)
int parity_run(const anim::Bank& bank, const char* script)
{
   g_alias_world = World{};
   g_native_world = World{};
   anim::Player pa{};
   anim::Player pn{};
   const uint16_t s = anim::find_script(bank, script);
   anim::start(pa, bank, kAliasGame, s);
   anim::start(pn, bank, kNativeGame, s);
   int frames = 0;
   while (frames < kFrames && (anim::running(pa) || anim::running(pn)))
   {
      if (frames == kFadeAt)
      {
         g_alias_world.fade[0] = kFadedOut;
         g_native_world.fade[0] = kFadedOut;
      }
      g_world = &g_alias_world;
      anim::frame(pa);
      g_world = &g_native_world;
      anim::frame(pn);
      ++frames;
      if (!same_player(pa, pn) || std::memcmp(&g_alias_world, &g_native_world, sizeof(World)) != 0)
      {
         std::printf("parity: %s differs after frame %d (pc %u vs %u)\n", script, frames, pa.pc, pn.pc);
         CHECK(false);
         break;
      }
   }
   g_world = &g_alias_world;
   return frames;
}

/// The flow script: same frames, same log, and the log the script means.
/// @param bank  the bank
void test_flow(const anim::Bank& bank)
{
   const int frames = parity_run(bank, "flow");
   CHECK(frames > kFadeAt && frames < kFrames);
   constexpr int16_t kExpected[] = {1, 3, 5, 6, 7, 8, 9};
   CHECK(g_alias_world.mark_count == static_cast<int>(sizeof(kExpected) / sizeof(kExpected[0])));
   for (int i = 0; i < g_alias_world.mark_count && i < kMarks; ++i)
   {
      CHECK(g_alias_world.marks[i] == kExpected[i]);
   }
}

/// A command that never advances: both stop with Runaway in the first frame; a bad target: both BadOperand.
/// @param bank  the bank
void test_stops(const anim::Bank& bank)
{
   parity_run(bank, "spin");
   anim::Player p{};
   anim::start(p, bank, kAliasGame, anim::find_script(bank, "spin"));
   anim::frame(p);
   CHECK(!anim::running(p) && p.error == anim::PlayerError::Runaway);
   parity_run(bank, "bad_target");
   anim::start(p, bank, kAliasGame, anim::find_script(bank, "bad_target"));
   anim::frame(p);
   CHECK(!anim::running(p) && p.error == anim::PlayerError::BadOperand);
}

}   // namespace

/// Runs the tests.
/// @return 0 when every check passed
int main()
{
   anim::Bank bank{};
   CHECK(anim::open(kAnimAliasBank, kAnimAliasBank_size, bank) == anim::BankError::None);
   test_flow(bank);
   test_stops(bank);
   std::printf("anim_alias_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
