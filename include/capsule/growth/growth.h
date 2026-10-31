/// @file
/// Growth (D-26 section 4, package G4): transformations, forms and training values. The rules are the
/// manifest's (capsule/growth/data.h, tools/battlec/growth.py); the engine evaluates them at its trigger points:
///
/// - level_up and after_battle: once a battle is over, the game looks at each party member (levelled_members()
///   says whose battlers levelled up: their trigger is level_up, the others' after_battle);
/// - item_use: an item's use_field logic runs `transform holder;` (capsule/inventory/use.h): the rules of the
///   item_use trigger with Context::item = the item decide; none holds: the logic fails (no effect, the item stays);
/// - trade: after a transfer import (battle::import_record()), with Context::item = the held item;
/// - script: the script statement `transform MEMBER -> var(v);` (capsule/script/port.h namespace growth).
///
/// A Change is a scope over a field context (capsule/battle/field_context.h) in a State the game lends: it finds the
/// rule, waits for the game's go() (the player may stop() it where the manifest's transformation.cancel allows),
/// applies it to the member (species, form 0 of the new species, stats and resource maxima re-derived, the stored
/// derived roles that read the species re-derived, the new species' moves of its level offered as a level-up
/// offers them) and writes the member back on destruction. Its presentation is events in the lent State
/// (EventKind::Transform, the learn messages) and the script port's callbacks.
///
/// Training values (structure `training`) are gained in battle by the battlers exp_policy shares a defeated foe's
/// experience with (its species' yields through the ruleset's gain formula, per-stat and total caps); stats follow
/// them at the next level-up in battle, and records always compute stats from them. Items change them with the
/// effect primitive `train WHO STAT AMOUNT` (the caps apply). Forms change in battle with `form WHO NAME` (battle
/// only: the record keeps its form).
#pragma once

#include "capsule/battle/battle.h"
#include "capsule/growth/data.h"
#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::growth
{

/// A form name that the species does not have (form_of_name()).
inline constexpr uint8_t kNoForm = 0xFF;

/// @name Event::extra of EventKind::Transform (battler: the creature)
///@{
inline constexpr uint8_t kTransformStart = 0;     ///< A rule holds: a = species now, b = the species it becomes.
inline constexpr uint8_t kTransformDone = 1;      ///< It became b (a = what it was).
inline constexpr uint8_t kTransformStopped = 2;   ///< The player stopped it (a = its species, b = the other).
inline constexpr uint8_t kTransformForm = 3;      ///< Its form is now a (b = the form before).
inline constexpr uint8_t kTransformAlso = 4;      ///< A second creature of species a joined the party (party slot b).
///@}

/// The context of a trigger: the item, and every context value (the clock's time of day, the game's port for the
/// others; src/growth/growth_context.cpp).
/// @param trigger  the trigger
/// @param item     the item (item_use: the item used; trade: the held item), or 0
/// @return the context
Context make_context(Trigger trigger, uint16_t item = 0);

/// The game's value of a context value whose source is the game (link-time port, like clock::time_of_day(): the
/// engine's default, src/growth/growth_port_default.cpp, answers the manifest's default).
/// @param index  context value (GrowthData::contexts index; battle_ids.h CONTEXT_<NAME>)
/// @return its value now
int32_t context_value(uint8_t index);

/// Sets a context value the game supplies (B1, D-28: a named value such as a moon phase; the default port
/// context_value() answers what was set last, else the manifest's default; src/growth/growth_context_store.cpp). A
/// game with its own context_value() port may ignore it. Values outside the declared range are clamped where read.
/// @param index  context value (battle_ids.h CONTEXT_<NAME>)
/// @param value  its value now
void set_context_value(uint8_t index, int32_t value);

/// What set_context_value() set (the default port's store).
/// @param index  context value
/// @param out    receives the value when one was set
/// @return false when none was set (or the index is out of range)
bool stored_context_value(uint8_t index, int32_t& out);

/// A species as one of its forms gives it (base stats, types, passives, sprites).
/// @param species  species index
/// @param form     form index (0, or one past the species' forms: the species itself)
/// @return the species' data or its form's view
const battle::SpeciesData& species_view(uint16_t species, uint8_t form);

/// How many forms a species has besides itself.
/// @param species  species index
/// @return 0 to kMaxForms
uint8_t form_count(uint16_t species);

/// The form of a species that has a form name.
/// @param species  species index
/// @param name     form name id (FormData::name; kBaseForm: the species itself)
/// @return form index, or kNoForm
uint8_t form_of_name(uint16_t species, uint8_t name);

/// A record's form: its form role through battle::record_value() (a derived, unstored form included).
/// @param rec  the record
/// @return form index (0 without a form role)
uint8_t record_form(const monster::Record& rec);

/// What a lookup found.
enum class ChangeKind : uint8_t
{
   None,      ///< No rule holds.
   Species,   ///< A transformation rule holds.
   Form,      ///< A form's rule holds (a form other than the record's).
   Blocked,   ///< The blocked expression holds: nothing may change.
   NUM_KINDS  ///< Number of kinds.
};

/// A rule a lookup found.
struct Found
{
   ChangeKind kind;   ///< What it is.
   uint8_t form;      ///< Form: the new form.
   uint8_t flags;     ///< Species: TransformRule::flags.
   uint8_t reserved;  ///< 0.
   uint16_t into;     ///< Species: the species it becomes.
   uint16_t also;     ///< Species: a second creature's species, or kNoSpecies.
   uint16_t rule;     ///< The rule (kNoRule for None).
};

/// Which change fires for a record in a context (nothing is changed): the blocked expression, then the species'
/// transformation rules in order (the first whose triggers include the context's and whose `when` holds), then
/// its forms with rules.
/// @param rec  the record
/// @param ctx  the context
/// @return what holds
Found find(const monster::Record& rec, const Context& ctx);

/// The party slots of a party whose battlers levelled up in a battle (State::levelled; look once the battle is
/// over: their trigger is level_up).
/// @param s      the battle
/// @param party  the party (its records)
/// @return bit per party slot
uint32_t levelled_members(const battle::State& s, const monster::Party& party);

/// Adds training to one stat within the caps (the effect primitive `train`, tools).
/// @param training  kMaxStats training values (in / out)
/// @param stat      stat index
/// @param delta     the change (negative lowers, never below 0)
/// @return the change made
int32_t add_training(uint8_t* training, uint8_t stat, int32_t delta);

/// Changes one stat's gain (the battle's TrainingRules::gain_point, C6): the gain and the stat in, the gain out.
using GainPoint = int32_t (*)(int32_t gain, int stat, void* ctx);

/// What a defeated foe gives a receiver's training values: each stat in order, the foe species' yield through
/// rules::train_gain() (and, in battle, the training point: @p point), capped by the total and then the stat cap (a
/// stat whose yield is 0 gains nothing).
/// @param training  kMaxStats training values (in / out)
/// @param foe       the defeated foe's species
/// @param level     the receiver's level
/// @param point     changes each gain (the battle's training point), or nullptr
/// @param ctx       its context
/// @return true when a value changed
bool gain_training(uint8_t* training, uint16_t foe, uint8_t level, GainPoint point = nullptr, void* ctx = nullptr);

/// Where a Change stands.
enum class Stage : uint8_t
{
   None,       ///< No rule holds (or the member is missing, or the blocked expression holds; C9: or it is an
               ///< egg and the structure's transformation.allow_eggs is not set).
   Asking,     ///< A rule holds: go() applies it, stop() stops it when cancellable().
   Learning,   ///< Applied; the new species offers a move with every move slot taken: answer_learn().
   Done,       ///< Applied (and every move offer answered).
   Stopped,    ///< The player stopped it.
   NUM_STAGES  ///< Number of stages.
};

/// One member's transformation or form change, as a scope over a field context in a lent State.
struct Change
{
   battle::State& state;      ///< The lent State (the field context; its events are the presentation).
   monster::Party& party;     ///< The party.
   Context context;           ///< The trigger context.
   Found found;               ///< The rule found.
   uint16_t from;             ///< The member's species before.
   uint8_t member;            ///< Its party slot.
   Stage stage;               ///< Where it stands.
   bool open;                 ///< The field context opened.

   /// Opens the field context on a member and looks for a rule (Stage::Asking when one holds; a
   /// kTransformStart event for a transformation).
   /// @param s       a State the game lends (not a battle in progress)
   /// @param p       the party
   /// @param slot    party slot of the member
   /// @param ctx     the trigger context (make_context())
   Change(battle::State& s, monster::Party& p, uint8_t slot, const Context& ctx);
   Change(const Change&) = delete;
   Change& operator=(const Change&) = delete;
   /// Writes the member back (and makes the second creature of an `also` rule when the party has room).
   ~Change();

   /// Whether the player may stop it (transformation.cancel names its trigger; a form change never asks).
   /// @return true while Asking and cancellable
   bool cancellable() const;

   /// Applies the rule.
   /// @return Learning (a move offer waits), Done, or the stage it was in when it was not Asking
   Stage go();

   /// Stops a cancellable transformation (a kTransformStopped event).
   /// @return true when it stopped
   bool stop();

   /// Answers the waiting move offer: the move slot to forget, or battle::kKeepMoves.
   /// @param slot  move slot or kKeepMoves
   /// @return Learning (the next offer of the level waits) or Done
   Stage answer_learn(uint8_t slot);

   /// The move a Learning stage offers.
   /// @return move id, or battle::kNone16
   uint16_t offered_move() const;
};

} // namespace capsule::growth

/// The battle side of growth (src/battle/growth_battle.cpp): changes to a battler, used by growth::Change on the field
/// context's member and by the effect primitives.
namespace capsule::battle
{

/// Turns a battler into another species: its base form, the species' types and passive (its passive slot), its
/// name when it carried the old species' name, stats and resource maxima re-derived (the hit points and pools gain
/// what their maxima gained); a Transform event (growth::kTransformDone).
/// @param s     the State (a field context)
/// @param i     battler
/// @param into  species index
/// @return false for an unknown battler or species
bool transform_battler(State& s, uint8_t i, uint16_t into);

/// Changes a battler's form: the form's types (added types end), its passive when the form gives other passives,
/// stats and resource maxima re-derived; a Transform event (growth::kTransformForm). In battle the record keeps its
/// form (battle only); in a field context the member's record takes it.
/// @param s     the State
/// @param i     battler
/// @param form  form index (0: the species itself)
/// @return true when the form changed
bool set_form(State& s, uint8_t i, uint8_t form);

/// Offers a battler the moves its species learns at its level, as a level-up does (learn.on_full; a human party's
/// offer waits in State::learn).
/// @param s  the State
/// @param i  battler
/// @return true when an offer waits for its answer
bool learn_level(State& s, uint8_t i);

/// Queues a Transform event (growth::kTransformStart, kTransformStopped, kTransformAlso).
/// @param s      the State
/// @param i      battler
/// @param extra  growth::kTransform...
/// @param a      first value
/// @param b      second value
void growth_event(State& s, uint8_t i, uint8_t extra, uint16_t a, uint16_t b);

} // namespace capsule::battle
