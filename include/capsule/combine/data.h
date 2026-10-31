/// @file
/// Combination tables (D-28 section 1, package B1): the structure's combination methods, compiled from the battle
/// manifests by tools/battlec/combine.py into capsule::combine::kCombine with rules::combine_value() (in the
/// generated battle.cpp; src/combine/combine_empty.cpp is the empty default of a program without battle data).
///
/// The engine has no idea of breeding, fusion or synthesis: a method takes N records, its ordered result rules
/// (recipes, tables with a pick, expressions) choose a species, the `on combine` effect logic builds the result
/// record and the result is delivered (capsule/combine/combine.h). Every rule, table, number and name is manifest
/// data; the engine only evaluates the generated expressions.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::combine
{

#if defined(CAPSULE_PLATFORM_GBA)
/// Most records one combination takes (tools/build_settings.py max_combine_inputs).
inline constexpr int kMaxInputs = 4;
#else
/// Most records one combination takes (tools/build_settings.py max_combine_inputs).
inline constexpr int kMaxInputs = 8;
#endif
#if defined(CAPSULE_PLATFORM_GBA)
/// Most nursery slots (B2; tools/build_settings.py max_nursery_slots).
inline constexpr int kMaxNurserySlots = 4;
#else
/// Most nursery slots (B2; tools/build_settings.py max_nursery_slots).
inline constexpr int kMaxNurserySlots = 8;
#endif
/// Most methods a manifest declares (tools/battlec/combine.py MAX_METHODS).
inline constexpr int kMaxMethods = 16;
/// Most move sources of one method (combine.py MAX_SOURCES).
inline constexpr int kMaxMoveSources = 4;
/// Most moves the sources gather for the chooser (combine.py MAX_CANDIDATES).
inline constexpr int kMaxCandidates = 16;
/// No expression (a rule without `when`, a method without odds or fee, a pick without where).
inline constexpr uint16_t kNoExpr = 0xFFFF;
/// No growth rule (a slot without a requirement).
inline constexpr uint16_t kNoRequirement = 0xFFFF;
/// No species (a rule's result besides a recipe's).
inline constexpr uint16_t kNoSpecies = 0xFFFF;
/// No field (a table keyed by the type, a pick ordered by the species index).
inline constexpr uint8_t kNoField = 0xFF;
/// No message for a moment.
inline constexpr uint8_t kNoMessage = 0xFF;
/// No step effect (a build without eggs or a nursery).
inline constexpr uint8_t kNoStepEffect = 0xFF;
/// C9: a move source that reads every input (MoveSourceData::from).
inline constexpr uint8_t kAnyInput = 0xFF;

/// @name MethodData::flags
///@{
inline constexpr uint8_t kFillMoves = 1;   ///< The result's own moves fill the slots the sources leave.
inline constexpr uint8_t kAllowEggs = 2;   ///< Eggs may be inputs (B2: the structure's egg role).
/// C9: the result keeps its own moves first and learns the chosen ones after them in order, each pushing the first
/// move out when the slots are full (an already known move is skipped); the ordered chooser takes every candidate.
inline constexpr uint8_t kPushMoves = 4;
///@}

/// @name EggData::flags (the refusal flags of the systems that use records; default refused)
///@{
inline constexpr uint8_t kEggsBattle = 1;    ///< Eggs may battle (structure rules.allow_eggs).
inline constexpr uint8_t kEggsRecruit = 2;   ///< An egg may be recruited (structure recruit.allow_eggs).
inline constexpr uint8_t kEggsLead = 4;      ///< C9: an egg may be the encounter lead (structure lead.allow_eggs).
inline constexpr uint8_t kEggsTransform = 8;   ///< C9: an egg may transform (structure transformation.allow_eggs).
/// C9: a transferred egg is received hatched by a build without eggs (structure `transfer_eggs: "hatch"`; default
/// refused: capsule::battle::import_record()). Set without an egg section too.
inline constexpr uint8_t kEggsTransferHatch = 16;
///@}

/// @name NurseryData::flags
///@{
inline constexpr uint8_t kNurseryGain = 1;   ///< Held records gain experience per step (pipeline nursery_gain).
inline constexpr uint8_t kNurseryFee = 2;    ///< Taking a record back costs (pipeline nursery_fee).
///@}

/// @name MoveSourceData::flags
///@{
inline constexpr uint8_t kSourceOn = 1;    ///< The source is on.
inline constexpr uint8_t kSourceAll = 2;   ///< combine_moves: the whole list (not only the moves an input knows).
///@}

/// Where a method runs (combine.py KINDS).
enum class MethodKind : uint8_t
{
   Nursery,     ///< Records are left in a nursery; the result appears over steps (B2: capsule/combine/nursery.h).
   Immediate,   ///< A script or menu combines records on the spot.
};

/// How the result arrives (combine.py DELIVERIES).
enum class Delivery : uint8_t
{
   Immediate,   ///< The party, else the storage boxes.
   Egg,         ///< An egg that hatches by steps (B2: capsule/combine/eggs.h).
};

/// What happens to the inputs (combine.py AFTERWARDS).
enum class Afterwards : uint8_t
{
   Kept,       ///< They stay (breeding).
   Consumed,   ///< They leave the party or boxes once the result is placed (fusion, synthesis).
};

/// Who picks the result's moves among the candidates (combine.py CHOOSERS).
enum class Chooser : uint8_t
{
   Ordered,   ///< The first ones, in source order.
   Random,    ///< At random.
   Player,    ///< A menu through the script port (random without one).
};

/// Where candidate moves come from (combine.py MOVE_SOURCES).
enum class MoveSource : uint8_t
{
   Known,           ///< The inputs' moves.
   CombineMoves,    ///< The result species' combine_moves list known by an input (kSourceAll: the whole list).
   SharedLevelUp,   ///< Moves of the result's learnset every input knows.
   TeachableKnown,  ///< The result's teachable moves an input knows.
};

/// A result rule's kind (combine.py RULE_KINDS).
enum class RuleKind : uint8_t
{
   Recipe,       ///< An exact (multi)set of species or species-field values.
   Table,        ///< A table's cell is a group; a pick chooses a species in it.
   Expression,   ///< An expression gives the species (index + 1; 0: no match).
};

/// A pick's choice among the candidates its where holds for (combine.py PICK_CHOICES).
enum class PickChoose : uint8_t
{
   First,    ///< The lowest by the pick's order.
   Last,     ///< The highest.
   Random,   ///< A random one.
};

/// What a pick does when where holds for no member (combine.py PICK_ELSE).
enum class PickElse : uint8_t
{
   None,    ///< The rule does not match.
   First,   ///< The lowest member of the group.
   Last,    ///< The highest.
};

/// What a recipe matcher compares (combine.py MATCHER_KINDS).
enum class MatcherKind : uint8_t
{
   Species,   ///< The species index.
   Field,     ///< A species field's value.
   Type,      ///< The first type.
};

/// What a table is keyed by (combine.py KEY_KINDS).
enum class KeyKind : uint8_t
{
   Field,   ///< A species field.
   Type,    ///< The first type.
};

/// The moments a method's messages name (combine.py MOMENTS).
enum class Moment : uint8_t
{
   Unfit,          ///< An input fails its slot's requirement.
   Incompatible,   ///< The odds are 0.
   Refused,        ///< No result rule matched (or the logic refused).
   NoRoom,         ///< Neither the party nor the boxes have room.
   Combined,       ///< The result was placed.
   NUM_MOMENTS     ///< Number of moments.
};

/// One input slot.
struct SlotData
{
   uint16_t requirement;   ///< Its requirement (rules::growth_when() id over the input), or kNoRequirement.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(SlotData, 4);

/// One recipe matcher.
struct MatcherData
{
   MatcherKind kind;   ///< What it compares.
   uint8_t field;      ///< Field: the species field.
   uint16_t value;     ///< The species index, field value or type.
};
CAPSULE_POD(MatcherData, 4);

/// One table: cells (a, b) -> group.
struct TableData
{
   uint16_t first_cell;    ///< First of its cells.
   uint16_t cell_count;    ///< Its cells.
   KeyKind key;            ///< What it is keyed by.
   uint8_t field;          ///< Field: the species field.
   uint8_t first_input;    ///< The input slot of a.
   uint8_t second_input;   ///< The input slot of b.
   uint8_t symmetric;      ///< 1: (a, b) also matches (b, a).
   uint8_t reserved[kPadAfterOneByte];    ///< 0.
};
CAPSULE_POD(TableData, 12);

/// One table cell.
struct CellData
{
   uint16_t a;          ///< The first input's key value.
   uint16_t b;          ///< The second input's.
   uint16_t value;      ///< The group.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(CellData, 8);

/// One result rule.
struct RuleData
{
   uint16_t when;        ///< Its condition (rules::combine_value() id), or kNoExpr.
   uint16_t expr;        ///< Expression: the species expression; Table: the pick's where (or kNoExpr).
   uint16_t first;       ///< Recipe: its first matcher; Table: its table (CombineData::tables index).
   uint16_t result;      ///< Recipe: the species it gives.
   RuleKind kind;        ///< What it is.
   uint8_t count;        ///< Recipe: its matchers (one per input).
   uint8_t order;        ///< Table: the species field the pick orders by, or kNoField (the species index).
   PickChoose choose;    ///< Table: the pick's choice.
   PickElse otherwise;   ///< Table: the pick's fallback.
   uint8_t reserved[kPadAfterOneByte];  ///< 0.
};
CAPSULE_POD(RuleData, 16);

/// One move source.
struct MoveSourceData
{
   MoveSource kind;    ///< Where the moves come from.
   uint8_t limit;      ///< Most moves it gives (0: no limit).
   uint8_t flags;      ///< kSourceOn, kSourceAll.
   uint8_t from;       ///< C9: the one input slot it reads (`from: ROLE`; its moves in slot order), or kAnyInput.
};
CAPSULE_POD(MoveSourceData, 4);

/// One combination method.
struct MethodData
{
   const text::Code* label;                        ///< What menus show.
   uint16_t first_slot;                            ///< First of its slots (CombineData::slots).
   uint16_t first_rule;                            ///< First of its result rules.
   uint16_t odds;                                  ///< Its odds expression (through rules::combine_odds()), or kNoExpr.
   uint16_t fee;                                   ///< Its fee expression (through rules::combine_fee()), or kNoExpr.
   uint16_t level;                                 ///< The result's level expression.
   uint8_t rule_count;                             ///< Its rules (first that matches wins).
   MethodKind kind;                                ///< Where it runs.
   Delivery delivery;                              ///< How the result arrives.
   Afterwards afterwards;                          ///< What happens to the inputs.
   Chooser chooser;                                ///< Who picks the moves.
   uint8_t min_inputs;                             ///< Fewest inputs.
   uint8_t max_inputs;                             ///< Most inputs (its slots).
   uint8_t move_limit;                             ///< Most moves the chooser gives.
   uint8_t source_count;                           ///< Its move sources.
   uint8_t flags;                                  ///< kFillMoves, kAllowEggs.
   MoveSourceData sources[kMaxMoveSources];        ///< The move sources, in order.
   uint8_t messages[static_cast<int>(Moment::NUM_MOMENTS)];   ///< Message id per moment, or kNoMessage.
   uint16_t cycle_steps;                           ///< B2, egg delivery: steps of one egg cycle (else 0).
};

/// The egg rules (B2, D-28 section 3; tools/battlec/nursery.py: structure `eggs` and the refusal flags).
struct EggData
{
   const text::Code* name;     ///< What an egg is called on screen, or nullptr (a build without eggs).
   uint32_t sprite;            ///< The default egg sprite's AssetKey value, or 0.
   uint32_t field_actions;     ///< Bit per field action an egg may do (field_actions[].allow_eggs).
   uint32_t step_effects;      ///< C9: bit per step effect whose handlers run on eggs (step_effects[].allow_eggs).
   monster::RoleId role;       ///< The egg role (non-zero: an egg), or monster::kNoRole (no eggs).
   monster::RoleId counter;    ///< The hatch counter role (steps left).
   uint8_t cycles_field;       ///< The species field of the hatch cycles.
   uint8_t sprite_field;       ///< The species field of the egg look, or kNoField.
   uint8_t step_effect;        ///< The egg cycle's step effect, or kNoStepEffect.
   uint8_t flags;              ///< kEggs* bits.
};

/// The nursery (B2, D-28 section 2; tools/battlec/nursery.py: structure `nursery`).
struct NurseryData
{
   uint16_t odds_scale;   ///< A check's chance is the method's odds in this.
   uint8_t slots;         ///< Records it holds (0: no nursery).
   uint8_t method;        ///< The combination method it runs (kind Nursery).
   uint8_t check;         ///< The checks' step effect, or kNoStepEffect.
   uint8_t flags;         ///< kNursery* bits.
   uint8_t reserved[kPadAfterTwoBytes];   ///< 0.
};
CAPSULE_POD(NurseryData, 8);

/// The game's combination tables.
struct CombineData
{
   const MethodData* methods;     ///< method_count methods.
   const SlotData* slots;         ///< The methods' slots.
   const RuleData* rules;         ///< Their result rules.
   const MatcherData* matchers;   ///< The recipes' matchers.
   const TableData* tables;       ///< Their tables.
   const CellData* cells;         ///< The tables' cells.
   const uint16_t* move_first;    ///< Per species (species_count + 1): the first of its combine moves in moves.
   const uint16_t* moves;         ///< The species' combine moves.
   const uint16_t* base;          ///< Per species: its base stage (the transformation rules walked backwards).
   uint16_t species_count;        ///< Entries of base (the battle's species, or 0 without methods).
   uint8_t method_count;          ///< Methods (<= kMaxMethods).
   EggData eggs;                  ///< B2: the egg rules.
   NurseryData nursery;           ///< B2: the nursery.
};

/// The build's tables (generated with the battle data; the default has no methods).
extern const CombineData kCombine;

/// A part of an input or a candidate an expression reads (combine.py INPUT_PARTS, then a species field).
enum class Part : uint8_t
{
   Species,   ///< The species index + 1 (0: no input in the slot).
   Base,      ///< Its base stage + 1.
   Type,      ///< Its first type.
   Field,     ///< A species field (the argument).
};

/// An aggregate of the inputs (combine.py AGGREGATES).
enum class Aggregate : uint8_t
{
   LevelSum,      ///< The sum of their levels.
   LevelAvg,      ///< Their average level (rounded down).
   LevelMin,      ///< Their lowest level.
   LevelMax,      ///< Their highest level.
   SameSpecies,   ///< 1 when every input is one species.
};

/// What the generated rules::combine_value() reads.
struct ValueInputs
{
   const monster::Record* recs[kMaxInputs];   ///< The inputs, slot order (count of them).
   uint16_t species[kMaxInputs];              ///< Their species indices.
   const int32_t* contexts;                   ///< The context values (growth::Context::values).
   uint16_t candidate;                        ///< A pick's candidate species, or kNoSpecies.
   uint16_t result_species;                   ///< The result species + 1 (the level and fee; 0 before one is known).
   uint8_t result_level;                      ///< The result's level (a fee).
   uint8_t count;                             ///< Inputs given.
};

/// @name What the generated expressions read (src/combine/combine.cpp)
///@{

/// An input's record role (battle::record_value()).
/// @param in       the inputs
/// @param slot     input slot
/// @param role     the role
/// @param element  its element
/// @return the value (0 for a slot without an input)
uint32_t input_role(const ValueInputs& in, int slot, monster::RoleId role, int element);

/// An input's species, base stage or type.
/// @param in    the inputs
/// @param slot  input slot
/// @param part  Species, Base or Type
/// @return the value (0 for a slot without an input)
int32_t input_part(const ValueInputs& in, int slot, Part part);

/// An input's species field.
/// @param in     the inputs
/// @param slot   input slot
/// @param field  species field
/// @return its value (0 for a slot without an input)
int32_t input_field(const ValueInputs& in, int slot, int field);

/// An aggregate of the inputs.
/// @param in  the inputs
/// @param a   which
/// @return its value
int32_t aggregate(const ValueInputs& in, Aggregate a);

/// A pick's candidate's species, base stage, type or species field.
/// @param in     the inputs (candidate set)
/// @param part   which part
/// @param field  Field: the species field
/// @return its value
int32_t candidate_part(const ValueInputs& in, Part part, int field);

/// The result's species, base stage, type or species field (the level and fee expressions).
/// @param in     the inputs (result_species set; 0 before the result is known: every part reads 0)
/// @param part   which part
/// @param field  Field: the species field
/// @return its value
int32_t result_part(const ValueInputs& in, Part part, int field);
///@}

} // namespace capsule::combine

namespace capsule::rules
{

/// A combination expression's value (generated from the manifest's combination methods; B1).
/// @param expr  expression id (MethodData::odds / fee / level, RuleData::when / expr)
/// @param in    the inputs (and a pick's candidate, a fee's result)
/// @return its value (0 for an unknown id)
int32_t combine_value(int32_t expr, const combine::ValueInputs& in);

} // namespace capsule::rules
