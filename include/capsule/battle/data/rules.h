/// @file
/// Rules (C-08): the switches and caps of a battle that are not formulas (the arithmetic of damage, speed,
/// accuracy and critical hits is the ruleset's pipelines, capsule/battle/pipeline.h; the stage tables and the
/// critical-hit odds are the stages' own, data/creatures.h StageData), and the turn structure (D-22 A11, D-23
/// section 2 `turn`): action brackets, the speed tie rule, the end-of-turn phases, the hook dispatch order and
/// the battle start's send-outs; and progression (D-22 A8 / A12, E6c): the learn policy, experience sharing
/// and the generation list of new records.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/battle/data/creatures.h"

namespace capsule::battle
{

/// Where a battler's effect owners come from (the hook dispatch's sources). TurnRules::effect_order and an
/// end-of-turn battlers phase list them in the order their handlers run.
enum class EffectSource : uint8_t
{
   Status,      ///< Its primary status.
   Volatile,    ///< Its volatiles (one owner per bit that is on, in manifest order).
   SideCond,    ///< Its side's conditions (manifest order).
   FieldCond,   ///< The field's conditions (manifest order).
   Weather,     ///< The weather (its battler hooks; its residual is the weather phase).
   Passive,     ///< Its passive.
   Item,        ///< Its held item.
   NUM_SOURCES
};

/// Sources a dispatch order lists at most (every EffectSource once).
inline constexpr int kEffectSources = static_cast<int>(EffectSource::NUM_SOURCES);
/// Most phases of the end of turn (TurnRules::end).
inline constexpr int kMaxEndPhases = 8;

/// What one end-of-turn phase runs.
enum class EndPhase : uint8_t
{
   Weather,    ///< The weather counts down (or ends), then its field_residual for each active battler.
   Battlers,   ///< Each active battler's residual hooks (EndPhaseData::sources in that order).
   Sides,      ///< Each side condition that is on (sides in order): its side_residual, then its countdown.
   Field,      ///< Each field condition that is on: its field_residual, then its countdown.
   Regen,      ///< Each active battler's resources regenerate (ResourceData::regen / regen_formula; D-25).
   Delayed,    ///< Each position's pending effect (effect `delay`) counts down; at 0 its move's land hook runs (E13b).
   NUM_END_PHASES
};

/// For whom a side or field condition's residual phase runs.
enum class PhaseEach : uint8_t
{
   Battler,   ///< Once for each active battler it holds for (its side's, or every one), fastest first.
   Once,      ///< Once per condition (holder: the first of those battlers, fastest first).
};

/// How battlers whose actions share a bracket and a speed are ordered.
enum class TieRule : uint8_t
{
   RandomByte,   ///< A byte drawn for every battler at the start of each turn (Battler::tiebreak).
   Position,     ///< Position order (no draw).
};

/// The battle start's send-out order.
enum class SendOrder : uint8_t
{
   FoesFirst,   ///< The other sides' positions first, then the human player's side.
   Position,    ///< Position order.
};

/// The order of the battle start's switch-in hooks (they run once everyone is out, one battler per step).
enum class StartHooks : uint8_t
{
   BySpeed,       ///< Fastest first.
   BySendOrder,   ///< In the order the battlers were sent out (SendOrder).
};

/// One end-of-turn phase.
struct EndPhaseData
{
   uint8_t phase;                         ///< EndPhase.
   uint8_t each;                          ///< Sides / Field: PhaseEach.
   uint8_t source_count;                  ///< Battlers: sources that run (<= kEffectSources).
   uint8_t sources[kEffectSources];       ///< Battlers: EffectSource values in the order their residual hooks
                                          ///< run (a source not listed has none in this phase).
};
CAPSULE_POD(EndPhaseData, 10);

/// The turn structure (manifest `turn`).
struct TurnRules
{
   int8_t run_bracket;                    ///< Order bracket of running (bigger acts first).
   int8_t switch_bracket;                 ///< Order bracket of switching.
   int8_t item_bracket;                   ///< Order bracket of using an item (kept for the Bag's item action).
   int8_t move_bracket;                   ///< Order bracket of every move when move_priority is 0.
   uint8_t move_priority;                 ///< 1: a move's bracket is its priority (MoveData::priority).
   uint8_t tie;                           ///< TieRule.
   uint8_t send_order;                    ///< SendOrder.
   uint8_t start_hooks;                   ///< StartHooks.
   uint8_t effect_order[kEffectSources];  ///< Hook dispatch order: every EffectSource once.
   uint8_t end_count;                     ///< Phases in end (<= kMaxEndPhases).
   EndPhaseData end[kMaxEndPhases];       ///< The end of turn's phases in order (then the volatiles that last
                                          ///< one turn end).
};
CAPSULE_POD(TurnRules, 96);

/// What a level-up's new move does when every move slot is taken (manifest `learn.on_full`, D-22 A8).
enum class LearnFull : uint8_t
{
   Skip,            ///< It is not learned (Msg::NotLearned).
   Request,         ///< A human party is asked which move to forget (RequestKind::LearnMove); AI parties skip.
   ReplaceOldest,   ///< The first slot's move is forgotten, the others move up, the new one goes last.
   NUM_LEARN_FULL
};

/// Which learnset moves a creature built without moves knows (manifest `learn.default_moves`).
enum class DefaultMoves : uint8_t
{
   LastByLevel,   ///< The last move_slots() distinct moves learned up to its level.
   First,         ///< The first move_slots() distinct moves learned up to its level.
   NUM_DEFAULT_MOVES
};

/// Who shares a fainted foe's experience (manifest `exp_policy.share`, D-22 A12).
enum class ExpShare : uint8_t
{
   Faced,    ///< The receiving parties' battlers that were on the field with it (Battler::faced).
   Active,   ///< The receiving parties' battlers on the field when it fainted.
   Party,    ///< Every usable battler of the receiving parties.
   NUM_EXP_SHARES
};

/// Which parties earn experience (manifest `exp_policy.receivers`).
enum class ExpReceivers : uint8_t
{
   HumanSide,       ///< Human parties on another side than the fainted battler's.
   OpposingSides,   ///< Every party on another side (AI parties too).
   NUM_EXP_RECEIVERS
};

/// How rules::exp_gain()'s outsider input is set (manifest `exp_policy.outsider`).
enum class ExpOutsider : uint8_t
{
   Never,     ///< Always 0.
   OwnerId,   ///< 1 for a battler built from a record whose OwnerId role differs from Setup::owner_id.
   NUM_EXP_OUTSIDERS
};

/// How a generation entry makes its role's value (manifest `generation`, D-22 A12, D-23 section 2).
enum class GenKind : uint8_t
{
   Random,        ///< GenData::value + one draw below GenData::span per element (a span of 1 draws nothing).
   Wide,          ///< GenData::value + (two draws, high half first) & GenData::span (a power of two less one).
   Constant,      ///< GenData::value (no draw).
   PassiveSlot,   ///< A passive slot the species rolls: one draw below its rolled slots, none with one.
   Derived,       ///< rules::derive(GenData::derived, element, record) (no draw; none for a role the layout does
                  ///< not store: record_value() computes it on read).
   WideLowFirst,  ///< As Wide, the low half drawn first (manifest "order": "low_first", E13a).
   NUM_GEN_KINDS
};

/// Most entries of the generation list.
inline constexpr int kMaxGeneration = 32;

/// One entry of the generation list: make_monster() runs them in order (their order is the draw order).
struct GenData
{
   uint32_t value;    ///< Random / Wide: lowest value; Constant: the value.
   uint32_t span;     ///< Random: values (1 to 65536); Wide / WideLowFirst: the mask of the two draws.
   uint8_t role;      ///< monster::RoleId (a role the layout leaves out still draws; nothing is stored).
   uint8_t kind;      ///< GenKind.
   uint8_t count;     ///< Elements it fills (the role's elements: stats, move slots, a custom role's count).
   uint8_t derived;   ///< Derived: the index rules::derive() takes.
};
CAPSULE_POD(GenData, 12);

/// Progression (D-22 A8, A12; manifest `learn`, `exp_policy`, `generation`).
struct ProgressRules
{
   const GenData* generation;   ///< The generation list (generation_count entries).
   uint8_t generation_count;    ///< Entries (<= kMaxGeneration).
   uint8_t learn_full;          ///< LearnFull.
   uint8_t default_moves;       ///< DefaultMoves.
   uint8_t exp_share;           ///< ExpShare.
   uint8_t exp_receivers;       ///< ExpReceivers.
   uint8_t exp_outsider;        ///< ExpOutsider.
};

/// Rule switches and constants of a ruleset that are not formulas.
struct Rules
{
   uint16_t stat_cap;             ///< Highest battle stat (P-18, REF-6a; pipelines read it as STAT_CAP).
   uint16_t fallback_move;        ///< Move used, paying nothing, when no move's cost can be paid (kNone16: the
                                  ///< turn is lost; D-25).
   uint8_t level_max;             ///< Highest level (LEVEL_MAX).
   uint8_t rank_max;              ///< Highest party rank (foe_rank is clamped to it).
   uint8_t self_hit_power;        ///< Power of self_hit_preview() (the self_hit pipeline's move_power; effect
                                  ///< `self_hit POWER` names its own).
   uint8_t self_hit_category;     ///< Category whose stats a self-hit uses (a damaging one).
   uint8_t move_type_point;       ///< The ruleset's `move_type` point, run on a hit's move type (kNone8: none;
                                  ///< empty data has no points: its 0 names none either; E13b).
   TurnRules turn;                ///< The turn structure.
   ProgressRules progress;        ///< Learning, experience sharing and the generation of new records.
};

} // namespace capsule::battle
