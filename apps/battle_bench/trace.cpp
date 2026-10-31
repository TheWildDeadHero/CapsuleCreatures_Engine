/// @file
/// The bench's RNG and effect VM traces (see trace.h).
#include "trace.h"

#include "bench_data.h"
#include "output.h"

#include <cstdio>
#include <cstring>

/// @name The renamed originals (cmake/parts/bench.cmake builds them under these names)
///@{
namespace capsule::rng
{
/// The linked generator's next16() (default or Method 1).
/// @param s  the stream
/// @return 0-65535
uint16_t bench_generator_next16(Stream& s);
} // namespace capsule::rng

namespace capsule::battle::effect
{
/// src/battle/effect_host.cpp host_get().
/// @param host  the battle
/// @param r     the run
/// @param ref   what
/// @param at    of what
/// @param arg   argument
/// @return the value
int32_t traced_host_get(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg);
/// src/battle/effect_host.cpp host_set().
/// @param host   the battle
/// @param r      the run
/// @param ref    what
/// @param at     of what
/// @param arg    argument
/// @param value  the value
void traced_host_set(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg, int32_t value);
/// src/battle/effect_host.cpp host_has().
/// @param host  the battle
/// @param r     the run
/// @param kind  id kind
/// @param at    of what
/// @param id    the id
/// @return true when present
bool traced_host_has(HostState& host, const EffectRun& r, HasKind kind, Place at, uint8_t id);
/// src/battle/effect_host.cpp host_role_get().
/// @param host     the battle
/// @param r        the run
/// @param battler  battler
/// @param role     role ID
/// @param elem     element
/// @return the value
int32_t traced_host_role_get(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem);
/// src/battle/effect_host.cpp host_role_set().
/// @param host     the battle
/// @param r        the run
/// @param battler  battler
/// @param role     role ID
/// @param elem     element
/// @param value    the value
void traced_host_role_set(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem,
                          int32_t value);
/// src/battle/effect_host.cpp host_battlers().
/// @param host  the battle
/// @param r     the run
/// @param set   which set
/// @param at    relative to
/// @return the mask
uint32_t traced_host_battlers(HostState& host, const EffectRun& r, BattlerSet set, Place at);
/// src/battle/effect_host.cpp host_below().
/// @param host  the battle
/// @param n     outcomes
/// @return the draw
uint32_t traced_host_below(HostState& host, uint32_t n);
/// src/battle/effect_host.cpp host_between().
/// @param host  the battle
/// @param lo    lowest
/// @param hi    highest
/// @return the draw
int32_t traced_host_between(HostState& host, int32_t lo, int32_t hi);
/// src/battle/effect_host.cpp host_chance().
/// @param host  the battle
/// @param n     successes
/// @param m     outcomes
/// @return the roll
bool traced_host_chance(HostState& host, uint32_t n, uint32_t m);
/// src/battle/effect_host.cpp host_odds() (E9).
/// @param host  the battle
/// @param r     the run
/// @param n     successes
/// @param m     outcomes
/// @param by    the causing battler
/// @param on    the battler it lands on
/// @return the roll
bool traced_host_odds(HostState& host, const EffectRun& r, uint32_t n, uint32_t m, uint8_t by, uint8_t on);
/// src/battle/effect_host.cpp host_action().
/// @param host  the battle
/// @param r     the run
/// @param a     the primitive
/// @return its outcome
Outcome traced_host_action(HostState& host, EffectRun& r, const Action& a);
} // namespace capsule::battle::effect
///@}

namespace capsule::bench
{

namespace
{

namespace effect = battle::effect;

/// Digits of a 32-bit state in hexadecimal.
constexpr int kStateDigits = sizeof("FFFFFFFF");
/// Suffix of a condition template's source name (tools/battlec/logic.py TEMPLATE_SUFFIX): code the battle
/// compiler expanded from a condition record's fields, not a file.
constexpr char kTemplateSuffix[] = " (template)";
/// Characters of kTemplateSuffix.
constexpr size_t kTemplateSuffixChars = sizeof(kTemplateSuffix) - 1;
/// Primitives in progress at once (nested runs start inside primitives).
constexpr int kMaxActions = effect::kMaxEffectDepth;

/// A primitive in progress: its run and the offset of its instruction (the run's pc is already past it).
struct ActionAt
{
   const effect::EffectRun* run;   ///< The run.
   uint16_t pc;                    ///< The instruction.
};

const battle::State* g_state = nullptr;   ///< The traced battle.
TraceOptions g_options;                   ///< What is traced.
uint32_t g_draws = 0;                     ///< Battle stream draws so far.
ActionAt g_actions[kMaxActions];          ///< Primitives in progress.
int g_action_depth = 0;                   ///< Entries of g_actions in use.

/// A run's position for a trace record.
struct RunAt
{
   const effect::EffectRun* run;   ///< The run, or nullptr.
   uint16_t pc;                    ///< The instruction.
};

/// The pc a host call at the run's pc stands for (inside a primitive: the primitive's instruction).
/// @param r  the run
/// @return the instruction offset
uint16_t pc_of(const effect::EffectRun& r)
{
   for (int k = g_action_depth - 1; k >= 0; --k)
   {
      if (g_actions[k].run == &r)
      {
         return g_actions[k].pc;
      }
   }
   return r.pc;
}

/// The innermost run executing right now: the newest hook frame that is running, else the move's phase program.
/// @return the run (nullptr outside the VM)
RunAt running()
{
   const effect::Machine& m = g_state->vm;
   if (m.depth == 0)
   {
      return RunAt{nullptr, 0};
   }
   const effect::EffectRun& top = m.frames[m.depth - 1];
   const effect::EffectRun* r = nullptr;
   if (top.state == effect::RunState::Running)
   {
      r = &top;
   }
   else if (g_state->mv.run.state == effect::RunState::Running)
   {
      r = &g_state->mv.run;
   }
   return RunAt{r, r != nullptr ? pc_of(*r) : static_cast<uint16_t>(0)};
}

/// The opcode at an instruction.
/// @param program  program
/// @param pc       offset
/// @return its name ("?" outside the code)
const char* op_at(uint16_t program, uint16_t pc)
{
   const effect::Bank& bank = battle::kData.effects;
   if (program >= bank.program_count || pc >= bank.programs[program].length)
   {
      return "?";
   }
   const uint8_t op = bank.code[bank.programs[program].offset + pc];
   return op < kBenchData.op_count ? kBenchData.op_names[op] : "?";
}

/// Adds a run's owner, hook, program, pc and source position to a record.
/// @param rec  record
/// @param at   the run and instruction
void put_run(Record& rec, const RunAt& at)
{
   const effect::EffectRun& r = *at.run;
   const uint16_t top = r.call_depth > 0 ? r.calls[0].program : r.program;
   rec.str("owner", r.ctx.owner < kBenchData.owner_count ? kBenchData.owner_names[r.ctx.owner] : "?");
   rec.str("hook", top < kBenchData.program_count ? kBenchData.program_hooks[top] : "?");
   if (r.call_depth > 0 && r.program < kBenchData.program_count)
   {
      rec.str("sub", kBenchData.program_labels[r.program]);
   }
   rec.num("program", r.program).num("pc", at.pc).num("depth", g_state->vm.depth);
   const effect::SourceLine* src = source_of(r.program, at.pc);
   if (src != nullptr)
   {
      // A file without a name is code the battle compiler generated; a condition template's source ("status
      // Poison (template)") is given as "template" (the owner it expands); otherwise "file" is a source file. The
      // lines are those of that text.
      const char* file = kBenchData.lines->files[src->file];
      const size_t chars = std::strlen(file);
      if (chars == 0)
      {
         rec.flag("generated", true);
      }
      else if (chars > kTemplateSuffixChars && std::strcmp(file + chars - kTemplateSuffixChars, kTemplateSuffix) == 0)
      {
         rec.str("template", std::string(file, chars - kTemplateSuffixChars));
      }
      else
      {
         rec.str("file", file);
      }
      rec.num("line", src->line).num("column", src->column);
   }
}

/// Writes a "vm" record of a host call.
/// @param r       the run
/// @param pc      its instruction
/// @param value   the call's result
/// @param result  whether @p value is reported
void vm_record(const effect::EffectRun& r, uint16_t pc, int64_t value, bool result)
{
   if (!g_options.vm || g_state == nullptr)
   {
      return;
   }
   Record rec("vm");
   rec.str("op", op_at(r.program, pc));
   put_run(rec, RunAt{&r, pc});
   if (result)
   {
      rec.num("value", value);
   }
   rec.write();
}

/// Writes a "vm" record of a draw primitive (Rand, Between, Chance, Odds), which gets no run from the VM.
/// @param value  its result
void vm_draw_record(int64_t value)
{
   if (!g_options.vm || g_state == nullptr)
   {
      return;
   }
   const RunAt at = running();
   if (at.run != nullptr)
   {
      vm_record(*at.run, at.pc, value, true);
   }
}

/// Phase names (battle::Phase order).
constexpr const char* kPhaseNames[] = {"Intro", "Start", "StartHooks", "Choose", "Execute", "EndTurn", "Replace",
                                       "Ended"};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == static_cast<size_t>(battle::Phase::NUM_PHASES));

/// Event kind names (battle::EventKind order).
constexpr const char* kEventNames[] = {"Message", "StatusText", "WeatherText", "UseMove", "Hp", "Status",
                                       "Stage", "Faint", "SwitchIn", "SwitchOut", "Weather", "Exp", "Level",
                                       "Turn", "End", "Presenter", "PresenterText", "Text", "Volatile", "Side",
                                       "Field", "Cue", "Resource", "UseItem", "Recruit", "Transform"};
static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) ==
              static_cast<size_t>(battle::EventKind::NUM_EVENT_KINDS));

} // namespace

void trace_start(const battle::State& s, const TraceOptions& opts)
{
   g_state = &s;
   g_options = opts;
   g_draws = 0;
   g_action_depth = 0;
}

uint32_t draw_count()
{
   return g_draws;
}

const battle::effect::SourceLine* source_of(uint16_t program, uint16_t pc)
{
   const effect::LineTable* t = kBenchData.lines;
   if (t == nullptr || t->line_count == 0)
   {
      return nullptr;
   }
   // Binary search for the last entry at or before (program, pc).
   int lo = 0;
   int hi = t->line_count - 1;
   int found = -1;
   while (lo <= hi)
   {
      const int mid = lo + ((hi - lo) >> 1);   // the midpoint
      const effect::SourceLine& e = t->lines[mid];
      if (e.program < program || (e.program == program && e.pc <= pc))
      {
         found = mid;
         lo = mid + 1;
      }
      else
      {
         hi = mid - 1;
      }
   }
   return found >= 0 && t->lines[found].program == program ? &t->lines[found] : nullptr;
}

void trace_event(const battle::State& s, const battle::Event& e, void* user)
{
   (void)user;
   if (!g_options.events)
   {
      return;
   }
   const int kind = static_cast<int>(e.kind);
   Record("event")
      .str("kind", kind < static_cast<int>(battle::EventKind::NUM_EVENT_KINDS) ? kEventNames[kind] : "?")
      .num("battler", e.battler)
      .num("target", e.target)
      .num("extra", e.extra)
      .num("a", e.a)
      .num("b", e.b)
      .num("turn", s.turn)
      .write();
}

const char* phase_name(battle::Phase p)
{
   const int k = static_cast<int>(p);
   return k < static_cast<int>(battle::Phase::NUM_PHASES) ? kPhaseNames[k] : "?";
}

} // namespace capsule::bench

namespace capsule::rng
{

uint16_t next16(Stream& s)
{
   const uint16_t v = bench_generator_next16(s);
   using namespace capsule::bench;
   if (g_state == nullptr || &s != &g_state->rng)
   {
      return v;   // a record's stream, or before the battle
   }
   ++g_draws;
   if (g_options.rng)
   {
      char state[kStateDigits];
      std::snprintf(state, sizeof(state), "%08X", static_cast<unsigned>(s.state));
      Record rec("rng");
      rec.num("n", g_draws).num("value", v).str("state", state).num("turn", g_state->turn);
      rec.str("phase", phase_name(g_state->phase));
      const RunAt at = running();
      if (at.run != nullptr)
      {
         put_run(rec, at);
      }
      rec.write();
   }
   return v;
}

} // namespace capsule::rng

namespace capsule::battle::effect
{

using bench::vm_record;

int32_t host_get(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg)
{
   const int32_t v = traced_host_get(host, r, ref, at, arg);
   vm_record(r, bench::pc_of(r), v, true);
   return v;
}

void host_set(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg, int32_t value)
{
   vm_record(r, bench::pc_of(r), value, true);
   traced_host_set(host, r, ref, at, arg, value);
}

bool host_has(HostState& host, const EffectRun& r, HasKind kind, Place at, uint8_t id)
{
   const bool v = traced_host_has(host, r, kind, at, id);
   vm_record(r, bench::pc_of(r), v ? 1 : 0, true);
   return v;
}

int32_t host_role_get(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem)
{
   const int32_t v = traced_host_role_get(host, r, battler, role, elem);
   vm_record(r, bench::pc_of(r), v, true);
   return v;
}

void host_role_set(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem, int32_t value)
{
   vm_record(r, bench::pc_of(r), value, true);
   traced_host_role_set(host, r, battler, role, elem, value);
}

uint32_t host_battlers(HostState& host, const EffectRun& r, BattlerSet set, Place at)
{
   const uint32_t v = traced_host_battlers(host, r, set, at);
   vm_record(r, bench::pc_of(r), v, true);
   return v;
}

uint32_t host_below(HostState& host, uint32_t n)
{
   const uint32_t v = traced_host_below(host, n);
   bench::vm_draw_record(v);
   return v;
}

int32_t host_between(HostState& host, int32_t lo, int32_t hi)
{
   const int32_t v = traced_host_between(host, lo, hi);
   bench::vm_draw_record(v);
   return v;
}

bool host_chance(HostState& host, uint32_t n, uint32_t m)
{
   const bool v = traced_host_chance(host, n, m);
   bench::vm_draw_record(v ? 1 : 0);
   return v;
}

bool host_odds(HostState& host, const EffectRun& r, uint32_t n, uint32_t m, uint8_t by, uint8_t on)
{
   const bool v = traced_host_odds(host, r, n, m, by, on);
   bench::vm_draw_record(v ? 1 : 0);
   return v;
}

Outcome host_action(HostState& host, EffectRun& r, const Action& a)
{
   // The VM has moved the run past the primitive; its instruction starts op size + 1 bytes earlier.
   const uint16_t pc = static_cast<uint16_t>(r.pc - 1 - op_info(a.op).size);
   vm_record(r, pc, 0, false);
   const bool tracked = bench::g_action_depth < bench::kMaxActions;
   if (tracked)
   {
      bench::g_actions[bench::g_action_depth++] = bench::ActionAt{&r, pc};
   }
   const Outcome o = traced_host_action(host, r, a);
   if (tracked)
   {
      --bench::g_action_depth;
   }
   return o;
}

} // namespace capsule::battle::effect
