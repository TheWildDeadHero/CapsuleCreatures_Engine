/// @file
/// Battle engine internals shared by the modules of src/battle (C-08): the constants, helpers,
/// hook context and damage types more than one module uses, and the functions one module defines for
/// another. Not part of the public API (capsule/battle/battle.h).
#pragma once

#include "capsule/battle/battle.h"

#include "capsule/battle/pipeline.h"
#include "capsule/battle/ruleset.h"
#include "capsule/core/assert.h"
#include "capsule/core/saturate.h"

namespace capsule::battle
{
struct EncounterPlan;   // capsule/battle/encounter_lead.h (F1)
} // namespace capsule::battle

/// Battle engine internals: what the src/battle modules share (see src/battle/internal.h).
namespace capsule::battle::detail
{

/// @name Value ranges
///@{
constexpr uint8_t kCountMax8 = 0xFF;          ///< Saturating 8-bit counters stop here.
constexpr int kMaxU16 = 0xFFFF;               ///< Highest 16-bit value (stats, damage, rewards).
constexpr int kPercent = 100;                 ///< 100 percent (accuracy, damage rolls, traits).
constexpr uint8_t kNoSide = 0xFF;             ///< State::winning_side: none.
///@}

/// Shorthand for the build's battle data.
/// @return kData
inline const BattleData& data()
{
   return kData;
}

/// Bit for a battler or position index.
/// @param i  index
/// @return 1 << i
constexpr uint32_t bit(unsigned i)
{
   return uint32_t{1} << i;
}

/// Whether a flag bit is set.
/// @param flags  flag byte
/// @param f      bit position
/// @return true if set
constexpr bool has_flag(uint32_t flags, unsigned f)
{
   return (flags & bit(f)) != 0;
}

/// The held item whose hooks, points and tags react (C9: a field context's read-only input keeps its item for
/// `input.X.item` reads, flagged BATTLER_QUIET_ITEM: it reacts to nothing).
/// @param b  the battler
/// @return its item index, or 0 (none, or quiet)
constexpr uint16_t live_item(const Battler& b)
{
   return has_flag(b.flags, BATTLER_QUIET_ITEM) ? uint16_t{0} : b.item;
}

/// @name Hook dispatch (effects.cpp)
///@{
constexpr uint8_t kVerdictNone = 0;      ///< Dispatch::verdict: the event goes on.
constexpr uint8_t kVerdictImmune = 1;    ///< An owner said immune.
constexpr uint8_t kVerdictBlocked = 2;   ///< An owner blocked it.
///@}

/// Where a battler's owners come from (data/rules.h EffectSource). Hooks run them in the manifest's dispatch
/// order (`turn.effect_order`, TurnRules::effect_order; the sample: status, volatiles, side, field, weather,
/// passive, item); an end-of-turn battlers phase may list its own.
using Source = EffectSource;

/// A dispatch order: the sources in the order their owners run.
struct SourceOrder
{
   const uint8_t* sources;   ///< EffectSource values.
   int count;                ///< Entries in sources (<= kEffectSources).
};

/// The hook dispatch order of the build's data (TurnRules::effect_order: every source).
/// @return the order
inline SourceOrder dispatch_order()
{
   return SourceOrder{kData.rules.turn.effect_order, kEffectSources};
}

/// Which of an owner's handlers a hook dispatch runs (C5: the per_foe mark, effect::kHandlerPerFoe).
enum class HandlerPart : uint8_t
{
   Every,      ///< Every handler of the hook (a switch-in's own part runs its per_foe handlers too).
   FoesOnly,   ///< Only the per_foe ones (a switch-in's part per foe; a foe's try_switch part).
   OwnOnly,    ///< Only the others (the switching battler's own try_switch part).
};

/// @name HookCtx::stopped
///@{
constexpr uint8_t kRunStopped = 1;   ///< A handler stopped (before_move: the turn is lost).
constexpr uint8_t kRunFailed = 2;    ///< A handler failed (try_start: the move that started it fails).
///@}

/// What one hook dispatch passes its handlers and gets back.
struct HookCtx
{
   uint8_t holder;     ///< The battler whose owners run.
   uint8_t other;      ///< The other battler of the event, or kBench.
   uint16_t move;      ///< Move involved, or kNone16.
   int32_t value;      ///< The hook's value in and out (damage, stage delta, order key; switch_in: the part).
   uint8_t verdict;    ///< Out: kVerdict*.
   uint8_t stopped;    ///< Out: kRunStopped / kRunFailed when a handler ended the dispatch, else 0.
   uint16_t heal_q8;   ///< Out: an immune verdict's heal.
};

/// The context registers of a run for a hook of @p c (user / target: the move in progress, else holder / other).
/// @param s   battle
/// @param c   the hook
/// @param owner  owner index
/// @return the registers
effect::Context hook_context(const State& s, const HookCtx& c, uint16_t owner);

/// A move type's chart result against a battler (damage.cpp type_effect()).
struct TypeEffect
{
   int32_t q8;         ///< Combined factor of the scaling tiers (Q8, BattleData::combine); kQ8One for a decisive
                       ///< tier.
   int16_t exp;        ///< Exponent sum of the scaling tiers (combine Exponent; else 0).
   uint8_t decisive;   ///< The decisive tier that applied (BattleData::tiers index), or kNone8.
   uint8_t kind;       ///< That tier's TierKind (Scale when none).
};

/// Result of a damage calculation.
struct Hit
{
   int32_t damage;      ///< HP to take.
   TypeEffect type;     ///< The chart result.
   bool zero;           ///< A decisive Zero tier: no effect.
   bool ko;             ///< A decisive Ko tier.
   bool crit;           ///< Critical hit.
};

/// How calc_damage() decides the critical hit.
enum class CritMode : uint8_t
{
   Never,    ///< No critical hit, no draw (AI estimates, previews).
   Roll,     ///< Roll the crit odds now (real hits).
   Always,   ///< A critical hit, no draw (previews).
};

/// No program (move.cpp phase_program()).
constexpr uint16_t kNoProgram = 0xFFFF;
/// MoveCursor::hit_type of a hit without a type (G1: a delayed hit whose record is not typed): no type is it, so
/// the chart is neutral and no battler shares it.
constexpr uint8_t kTypeless = 0xFE;
/// Action::slot of a Replace choice not sent in yet (step() sends it in).
constexpr uint8_t kPendingSend = 1;

/// The taken record of a battler that took no damage from a move since it entered.
constexpr Taken kNoTaken{0, kBench, kNone8, kNone16, 0};

// -- state.cpp -----------------------------------------------------------------------------------

void emit(State& s, EventKind kind, uint8_t battler, uint8_t target = kBench, uint8_t extra = 0, uint16_t a = 0,
          uint16_t b = 0);
void say(State& s, Msg m, uint8_t user, uint8_t target = kBench, uint16_t b = 0, uint8_t extra = 0);
bool alive_active(const State& s, uint8_t i);
bool usable(const Battler& b);
uint8_t side_of(const State& s, uint8_t i);
bool has_type(const Battler& b, uint8_t t);
void compute_stats(const SpeciesData& sp, uint8_t level, const uint8_t* genes, const uint8_t* training,
                   const uint8_t* awaken, uint8_t trait, uint16_t* out);
void set_hp(State& s, uint8_t i, int32_t hp);
bool party_alive(const State& s, int t);

// -- resources.cpp (D-25) ------------------------------------------------------------------------

const ResourceData* resource_at(uint8_t r);
bool is_move_resource(const ResourceData& d);
bool is_faint_resource(const ResourceData& d);
int slot_of(const Battler& b, uint16_t move);
int32_t value_of(const Battler& b, const ResourceData& d, int slot);
int32_t max_of(const Battler& b, const ResourceData& d, int slot);
void set_resource(State& s, uint8_t i, uint8_t r, int slot, int32_t v);
void load_resources(Battler& b, const monster::Record* rec);
void store_resources(const Battler& b, monster::Record& rec);
void reset_move_slot(Battler& b, int slot);
void shift_move_slots(Battler& b);
void pool_maxima(const Battler& b, int32_t out[kMaxPools]);
void grow_pools(State& s, uint8_t i, const int32_t old_max[kMaxPools]);
bool can_pay(const Battler& b, uint16_t move, int slot);
int32_t cost_share(const Battler& b, uint16_t move, int slot, int32_t scale);
void pay(State& s, uint8_t i, uint16_t move, int slot);
void regen_battler(State& s, uint8_t i);
int32_t restore_resource(State& s, uint8_t i, const effect::Action& a, uint16_t move);
int32_t raise_resource(State& s, uint8_t i, const effect::Action& a, uint16_t move);

// -- effects.cpp ---------------------------------------------------------------------------------

const effect::Owner* owner_at(uint16_t owner);
int source_slots(Source src);
uint32_t source_mask(const State& s, uint8_t i, Source src);
const effect::Owner* source_owner(const State& s, uint8_t i, Source src, int k);
void run_hook(State& s, effect::Hook h, HookCtx& c, HandlerPart part = HandlerPart::Every);
void run_hook_in(State& s, effect::Hook h, HookCtx& c, SourceOrder order);
HookCtx run_own(State& s, uint16_t owner, effect::Hook h, uint8_t holder, uint8_t other, int32_t value = 0,
                uint16_t move = kNone16);
void switch_in_hooks(State& s, uint8_t i);
void hook_simple(State& s, effect::Hook h, uint8_t holder, uint8_t other, uint16_t move = kNone16);
void count_fault(State& s, effect::Fault f);

/// Calls @p f (Source, const effect::Owner&) for every owner a battler holds, in dispatch order, each read from
/// the battle as it is at that moment (an owner an earlier call removed is skipped). Stops early when @p f
/// returns false.
/// @tparam F     callable returning bool
/// @param s      battle
/// @param i      battler
/// @param f      the visitor
/// @param order  the sources in order (default: the data's dispatch order)
/// @return false if @p f stopped it
template <typename F>
bool for_each_owner(const State& s, uint8_t i, F&& f, SourceOrder order = dispatch_order())
{
   for (int n = 0; n < order.count; ++n)
   {
      const Source so = static_cast<Source>(order.sources[n]);
      const int slots = source_slots(so);
      for (int k = 0; k < slots; ++k)
      {
         // re-read: an earlier owner may have changed what the battler holds
         const uint32_t held = source_mask(s, i, so);
         if ((held >> k) == 0)
         {
            break;
         }
         const effect::Owner* o = has_flag(held, static_cast<unsigned>(k)) ? source_owner(s, i, so, k) : nullptr;
         if (o != nullptr && !f(so, *o))
         {
            return false;
         }
      }
   }
   return true;
}

// -- conditions.cpp ------------------------------------------------------------------------------

/// How starting a condition went (conditions.cpp).
enum class Started : uint8_t
{
   Started,   ///< It is on now.
   Already,   ///< It was on already (its `already` hook ran; a layer may have been added).
   Refused,   ///< Its try_start hook (immune / block) or the engine refused it.
   Failed,    ///< Its try_start hook failed: the run that started it fails too.
};

uint32_t status_id_mask();
int status_id_bits();
Started inflict(State& s, uint8_t target, uint8_t status, uint8_t source, bool loud);
void cure(State& s, uint8_t i, bool quiet = false);
Started volatile_on(State& s, uint8_t i, uint8_t v, uint8_t source, int32_t turns);
void volatile_off(State& s, uint8_t i, uint8_t v, uint8_t source);
void clear_volatiles(Battler& b, VolatileFlag when);
void pass_volatiles(const Battler& from, Battler& to);
void end_bound(State& s, uint8_t source);
void rebind_bound(State& s, uint8_t from, uint8_t to);
Started side_on(State& s, uint8_t side, uint8_t c, uint8_t holder, int32_t turns, bool add_layer);
void side_off(State& s, uint8_t side, uint8_t c, uint8_t holder);
Started field_on(State& s, uint8_t c, uint8_t holder, int32_t turns);
void field_off(State& s, uint8_t c, uint8_t holder);
Started weather_on(State& s, uint8_t w, uint8_t holder, int32_t turns);
void weather_off(State& s, uint8_t holder);
/// The weather that acts (C6): State::weather, or kNone8 while a holder of BattleData::weather_suppress_tags is on
/// the field (the weather's owner is no source then and field.weather reads none).
/// @param s  battle
/// @return weather index or kNone8
uint8_t acting_weather(const State& s);
/// Runs the weather_changed hook for every active battler (position order).
/// @param s       battle
/// @param source  who changed the weather (other), or kBench
/// @param now     the weather acting now, or kNone8
void weather_changed(State& s, uint8_t source, uint8_t now);
/// Scope: the acting weather when it opens; when it closes and the acting weather differs, weather_changed() runs
/// (a weather started or ended, a suppressing battler came or went).
struct WeatherWatch
{
   State& s;              ///< The battle.
   const uint8_t source;  ///< Who changes it.
   const uint8_t before;  ///< The acting weather when the scope opened.

   /// Opens the scope.
   /// @param st   battle
   /// @param who  who may change it, or kBench
   WeatherWatch(State& st, uint8_t who)
      : s(st), source(who), before(acting_weather(st))
   {
   }
   WeatherWatch(const WeatherWatch&) = delete;
   WeatherWatch& operator=(const WeatherWatch&) = delete;
   /// Runs weather_changed() when the acting weather changed.
   ~WeatherWatch()
   {
      const uint8_t now = acting_weather(s);
      if (now != before)
      {
         weather_changed(s, source, now);
      }
   }
};
uint8_t condition_holder(const State& s, bool on_side, uint8_t side);
void record_taken(State& s, uint8_t target, uint8_t from, uint16_t move, int32_t amount);
bool weather_hurts(const State& s);
void end_turn_weather(State& s, uint8_t i);
void end_turn_battler(State& s, uint8_t i, const EndPhaseData& phase);
bool end_turn_conditions(State& s, bool field, uint8_t each);
void end_turn_clear(State& s);

// -- damage.cpp ----------------------------------------------------------------------------------

bool damaging(const MoveData& m);
PipelineCtx pipeline_ctx(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move, int roll);
int32_t speed_of(State& s, uint8_t i);
TypeEffect type_effect(uint8_t type, const Battler& b);
uint8_t move_type_of(const State& s, uint8_t user, uint8_t target, uint16_t move);
uint8_t category_of(uint16_t move, uint8_t type);
void aim_hit(State& s, uint8_t target);
uint8_t hit_type_of(const State& s, uint8_t user, uint8_t target, uint16_t move);
void say_tier(State& s, uint8_t message, uint8_t user, uint8_t target);
void say_effectiveness(State& s, const Hit& hit, uint8_t user, uint8_t target);
bool change_stage(State& s, uint8_t i, uint8_t stage, int delta, bool quiet_fail, bool silent = false);
Hit calc_damage(State& s, uint8_t user, uint8_t target, uint16_t move, bool spread, int roll, CritMode crit,
                int32_t draw = kNoDraw);
int32_t self_hit_damage(State& s, uint8_t i, int roll, int32_t power = -1);
ChanceResult accuracy_of(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move);
ChanceResult accuracy_at(const State& s, rng::Stream* rng, uint8_t user, uint8_t target, uint16_t move,
                         int32_t accuracy);
ChanceResult crit_of(const State& s, rng::Stream* rng, uint8_t user, uint16_t move);
ChanceResult effect_odds_of(const State& s, rng::Stream* rng, uint8_t by, uint8_t on, uint16_t move, int32_t n,
                            int32_t m);

// -- move.cpp ------------------------------------------------------------------------------------

uint8_t reach(const State& s, uint8_t pos);
uint8_t occupied(const State& s, uint8_t user, bool foes);
void begin_move(State& s, uint8_t i, const Action& a);
void advance_move(State& s);
bool move_asking(const State& s);
uint16_t phase_program(uint16_t move, effect::Hook phase);

// -- progress.cpp --------------------------------------------------------------------------------

uint32_t exp_for(const SpeciesData& sp, int level);
void default_moves(const SpeciesData& sp, uint8_t level, uint16_t* moves);
bool pay_exp(State& s);
bool learn_asking(const State& s);
void check_faints(State& s, uint8_t attacker);

// -- parties.cpp ---------------------------------------------------------------------------------

void party_intro(State& s, int t);
void send_in(State& s, uint8_t i, uint8_t pos, bool hooks = true);
void withdraw(State& s, uint8_t i);
bool pending_send(const State& s, uint8_t i);
uint8_t next_bench(const State& s, int t);
int owner_of(const State& s, uint8_t pos);
bool check_end(State& s);
void end_battle(State& s, Outcome o);
void finish_ended(State& s);
bool switch_allowed(const State& s, uint8_t active, uint8_t bench);
void write_back(State& s);
bool needs_replacement(const State& s, uint8_t pos);

// -- item_action.cpp, recruit.cpp (G3) ---------------------------------------------------------

void item_action(State& s, uint8_t i, const Action& a);
effect::Outcome recruit_battler(State& s, uint8_t target, int32_t bonus);

// -- ai.cpp --------------------------------------------------------------------------------------

Action ai_choose(State& s, uint8_t i);

// -- effect_host.cpp -----------------------------------------------------------------------------

int32_t battler_read(const State& s, uint8_t who, effect::Ref ref, uint8_t arg, uint16_t move = kNone16);
bool battler_has(const State& s, uint8_t who, effect::HasKind kind, uint8_t id, uint16_t move);
bool place_has(const State& s, const effect::Place& at, effect::HasKind kind, uint8_t id, uint16_t move);
int32_t place_read(const State& s, const effect::Place& at, effect::Ref ref, uint8_t arg, bool& found);
effect::Status run_phase(State& s, bool answer);

// -- teach.cpp, field_context.cpp (G2) ---------------------------------------------------------------------------

bool from_record(Battler& b, const monster::Record& rec);

effect::Outcome teach_move(State& s, uint8_t i, int32_t move);
void learn_into(State& s, uint8_t i, int slot, uint16_t m);
void answer_offer(State& s);

// -- growth_battle.cpp (G4), with what it uses of state.cpp and progress.cpp ----------------------------------------

uint8_t passive_for(const SpeciesData& sp, uint8_t slot);
bool learn_level_moves(State& s, uint8_t i, int from);
const SpeciesData& battler_species(const Battler& b);
void restat(State& s, uint8_t i);
void gain_battler_training(State& s, uint8_t i, uint16_t foe);
void store_training(const Battler& b, monster::Record& rec);
void write_growth(const Battler& b, monster::Record& rec);
effect::Outcome form_primitive(State& s, uint8_t i, uint8_t name);
effect::Outcome transform_primitive(State& s, uint8_t i, uint16_t owner);
effect::Outcome train_primitive(State& s, uint8_t i, uint8_t stat, int32_t amount);

// -- combine_battle.cpp (B1): the Inherit op, with the record lookup of effect_host.cpp ---------------------------

monster::Record* record_of(State& s, uint8_t i);
effect::Outcome inherit_primitive(State& s, const effect::Action& a, uint8_t i);
void set_combine_inputs(uint8_t count);

// -- encounter_plan.cpp (F1): the plan the Encounter op changes while an encounter hook runs (encounter_lead.cpp) --

void set_encounter_plan(EncounterPlan* plan, effect::Hook hook);

}   // namespace capsule::battle::detail

namespace capsule::battle::effect
{

/// The battle engine as the effect VM's embedder: the host functions work on the battle (effect_host.cpp).
/// Every engine entry into the VM wraps the State in one; everything it keeps lives in the State.
struct HostState
{
   State& s;   ///< The battle.
};

} // namespace capsule::battle::effect
