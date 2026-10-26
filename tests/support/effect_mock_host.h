/// @file
/// A mock effect VM host for tests (the engine's tests/effect_vm_tests.cpp and a ruleset repository's effect tests,
/// e.g. the reference repo's Gen-3 examples): the host functions the VM calls (include/capsule/battle/effect.h)
/// over a plain HostState (counters, words, status counter / scratch, role 128, volatile bits, HP), a logged list
/// of primitives, its own draw generator, and the helpers reset(), find() and context(). Include it in exactly one
/// source file of a test program, after `using namespace capsule::battle::effect;`, below the bank declarations.
/// Also defines the test checks (CHECK, CHECK_EQ; g_checks / g_failures) and the test build's assert handler.
#pragma once

#include "capsule/battle/effect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

namespace capsule::battle::effect
{

/// One logged primitive.
struct Logged
{
   Action action;   ///< What the VM passed.
   int depth;       ///< Machine::depth when it ran.
};

/// The mock embedder state.
struct HostState
{
   Machine* machine;          ///< The machine (for nested runs).
   const Bank* bank;          ///< The bank (for nested runs).
   uint32_t seed;             ///< Draw generator state.
   int draws;                 ///< Draws so far.
   Logged log[64];            ///< Primitives performed.
   int logged;                ///< Entries in log.
   int32_t counters[kMaxBattlers][8];   ///< Volatile counters.
   int32_t words[kMaxBattlers][4];      ///< Volatile words.
   int32_t status_counter[kMaxBattlers];   ///< Status counters.
   int32_t status_aux[kMaxBattlers];       ///< Status scratch.
   int32_t roles[kMaxBattlers][4];         ///< Role 128, elements 0-3.
   uint32_t volatiles[kMaxBattlers];       ///< Volatile bits (has(); 0: the formula answers).
   int32_t hp[kMaxBattlers];               ///< HP (0: the formula answers).
   int32_t max_hp[kMaxBattlers];           ///< Max HP (0: the formula answers).
   bool ask_suspends;         ///< Ask: no answer yet.
   int32_t ask_answer;        ///< Ask: the answer when it does not suspend.
   int32_t damage;            ///< What Damage deals.
   bool recurse;              ///< Inflict runs `recurse_program` again (depth limit test).
   uint16_t recurse_program;  ///< See recurse.
   int nested_runs;           ///< Runs started by Inflict.
   int marked_start;          ///< Program of status Marked's start hook (-1: none).
};

/// The pipeline context of the generated point handlers.
struct PointCtx
{
   bool has;      ///< What point_has answers.
   int32_t base;  ///< Added to point_get's answers.
};

} // namespace capsule::battle::effect

namespace
{

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Checks a condition.
/// @param ok    condition
/// @param what  description
/// @param line  source line
void check(bool ok, const char* what, int line)
{
   ++g_checks;
   if (!ok)
   {
      std::printf("FAIL line %d: %s\n", line, what);
      ++g_failures;
   }
}

#define CHECK(c) check((c), #c, __LINE__)
#define CHECK_EQ(a, b)                                                                                                 \
   do                                                                                                                  \
   {                                                                                                                   \
      const long long va_ = static_cast<long long>(a);                                                               \
      const long long vb_ = static_cast<long long>(b);                                                               \
      ++g_checks;                                                                                                      \
      if (va_ != vb_)                                                                                                  \
      {                                                                                                                \
         std::printf("FAIL line %d: %s == %s (%lld != %lld)\n", __LINE__, #a, #b, va_, vb_);                         \
         ++g_failures;                                                                                                 \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

/// The formula host_get answers non-stored reads with.
/// @param ref   what
/// @param at    of what
/// @param arg   argument
/// @return the value
int32_t formula(Ref ref, Place at, uint8_t arg)
{
   return static_cast<int32_t>(ref) * 1000 + at.battler * 10 + static_cast<int32_t>(at.kind) + arg * 3;
}

/// The formula host_has answers with.
/// @param kind  kind
/// @param at    of what
/// @param id    id
/// @return the answer
bool has_formula(HasKind kind, Place at, uint8_t id)
{
   return (static_cast<int>(kind) + id + at.battler + static_cast<int>(at.kind)) % 2 == 1;
}

/// Multiplier of the mock's draw generator (a plain 32-bit LCG step; the draws only need to be repeatable).
constexpr uint32_t kMockDrawMul = 747796405u;
/// Increment of the mock's draw generator.
constexpr uint32_t kMockDrawAdd = 2891336453u;

/// The mock's draw generator.
/// @param h  the host
/// @return 15 random bits
uint32_t next_draw(HostState& h)
{
   h.seed = h.seed * kMockDrawMul + kMockDrawAdd;
   ++h.draws;
   return (h.seed >> 16) & 0x7FFFu;
}

} // namespace

namespace capsule::battle::effect
{

int32_t host_get(HostState& h, const EffectRun& /*r*/, Ref ref, Place at, uint8_t arg)
{
   switch (ref)
   {
   case Ref::Counter:
      return h.counters[at.battler][arg];
   case Ref::Word:
      return h.words[at.battler][arg];
   case Ref::StatusCounter:
      return h.status_counter[at.battler];
   case Ref::StatusAux:
      return h.status_aux[at.battler];
   case Ref::Hp:
      return h.hp[at.battler] != 0 ? h.hp[at.battler] : formula(ref, at, arg);
   case Ref::MaxHp:
      return h.max_hp[at.battler] != 0 ? h.max_hp[at.battler] : formula(ref, at, arg);
   default:
      return formula(ref, at, arg);
   }
}

void host_set(HostState& h, const EffectRun& /*r*/, Ref ref, Place at, uint8_t arg, int32_t value)
{
   switch (ref)
   {
   case Ref::Counter:
      h.counters[at.battler][arg] = value;
      break;
   case Ref::Word:
      h.words[at.battler][arg] = value;
      break;
   case Ref::StatusCounter:
      h.status_counter[at.battler] = value;
      break;
   default:
      h.status_aux[at.battler] = value;
      break;
   }
}

bool host_has(HostState& h, const EffectRun& /*r*/, HasKind kind, Place at, uint8_t id)
{
   if (kind == HasKind::Volatile && h.volatiles[at.battler] != 0)
   {
      return (h.volatiles[at.battler] >> id & 1u) != 0;
   }
   return has_formula(kind, at, id);
}

int32_t host_role_get(HostState& h, const EffectRun& /*r*/, uint8_t battler, uint8_t role, uint8_t elem)
{
   return role == 128 ? h.roles[battler][elem] : -1;
}

void host_role_set(HostState& h, const EffectRun& /*r*/, uint8_t battler, uint8_t role, uint8_t elem, int32_t value)
{
   if (role == 128)
   {
      h.roles[battler][elem] = value;
   }
}

uint32_t host_battlers(HostState& /*h*/, const EffectRun& /*r*/, BattlerSet set, Place /*at*/)
{
   return set == BattlerSet::Foes ? 0b1010u : 0b1111u;
}

uint32_t host_below(HostState& h, uint32_t n)
{
   return next_draw(h) % n;
}

int32_t host_between(HostState& h, int32_t lo, int32_t hi)
{
   return lo + static_cast<int32_t>(next_draw(h) % static_cast<uint32_t>(hi - lo + 1));
}

bool host_chance(HostState& h, uint32_t n, uint32_t m)
{
   return next_draw(h) % m < n;
}

bool host_odds(HostState& h, const EffectRun&, uint32_t n, uint32_t m, uint8_t, uint8_t)
{
   return next_draw(h) % m < n;   // the mock has no effect odds pipeline: plain odds
}

Outcome host_action(HostState& h, EffectRun& r, const Action& a)
{
   if (h.logged < 64)
   {
      h.log[h.logged++] = Logged{a, h.machine->depth};
   }
   switch (a.op)
   {
   case Op::Damage:
      return Outcome{h.damage, false, false, false};
   case Op::Inflict:
   {
      // A nested run: the status's own start hook (or the depth-limit chain).
      Context ctx = r.ctx;
      ++h.nested_runs;
      if (h.recurse)
      {
         invoke(*h.machine, *h.bank, h, h.recurse_program, ctx);
      }
      else if (h.marked_start >= 0)
      {
         invoke(*h.machine, *h.bank, h, static_cast<uint16_t>(h.marked_start), ctx);
      }
      return Outcome{1, false, false, false};
   }
   case Op::Ask:
      return Outcome{h.ask_answer, h.ask_suspends, false, false};
   case Op::Stage:
      return Outcome{a.args[0], false, false, false};
   default:
      return Outcome{0, false, false, false};
   }
}

int32_t point_get(PointCtx& c, Ref ref, uint8_t subj, uint8_t /*arg*/)
{
   return c.base + static_cast<int32_t>(ref) * 10 + subj;
}

bool point_has(PointCtx& c, HasKind /*kind*/, uint8_t /*subj*/, uint8_t /*id*/)
{
   return c.has;
}

} // namespace capsule::battle::effect

namespace
{

Machine g_machine{};   ///< Shared bookkeeping (as a battle would keep it).
HostState g_host{};    ///< The mock host.

/// Resets the mock host for a bank.
/// @param bank  the bank the nested runs use
void reset(const Bank& bank)
{
   g_host = HostState{};
   g_host.machine = &g_machine;
   g_host.bank = &bank;
   g_host.seed = 7;
   g_host.damage = 10;
   g_host.marked_start = -1;
   g_machine = Machine{};
}

/// Finds a handler's program by owner name ("kind Name") and hook.
/// @param bank   the bank
/// @param names  its owner names
/// @param owner  "kind Name"
/// @param hook   hook or phase
/// @return the program index (aborts when missing)
uint16_t find(const Bank& bank, const char* const* names, const char* owner, Hook hook)
{
   for (int o = 0; o < bank.owner_count; ++o)
   {
      if (std::strcmp(names[o], owner) != 0)
      {
         continue;
      }
      const Owner& ow = bank.owners[o];
      for (int i = 0; i < ow.handler_count; ++i)
      {
         const Handler& hd = bank.handlers[ow.first_handler + i];
         if ((hd.flags & kHandlerPoint) == 0 && hd.hook == static_cast<uint8_t>(hook))
         {
            return hd.program;
         }
      }
   }
   std::printf("no handler %s / %d\n", owner, static_cast<int>(hook));
   std::abort();
}

/// The standard context: holder 0, other 1, user 2, target 3, move 6, owner 9.
/// @param value  ctx.value
/// @return the context
Context context(int32_t value)
{
   return Context{value, 0, 6, 9, 0, 1, 2, 3};
}

} // namespace
