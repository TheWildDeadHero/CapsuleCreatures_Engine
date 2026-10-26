/// @file
/// The battle engine (C-08): state, setup, turn loop, choices and events.
///
/// A battle is one State (plain data, no heap). The caller sets it up with
/// start(), then drives it:
///
///     while (true) {
///         Event e;
///         while (pop_event(s, e)) { present(e); }   // text, HP bars, sprites
///         const Progress p = step(s);
///         if (p == Progress::Ended) break;
///         if (p == Progress::Input) { Request r; request(s, r); choose(s, r.battler, ask_player(r)); }
///     }
///
/// step() runs one small unit of work (see kEventsPerStep: the send-outs, one
/// hit or effect of a move, one level-up, one battler's end of turn, one
/// replacement...) and records what happened as presentation events, so the
/// presentation can play them at its own pace; the engine never waits for
/// it. Controllers are input sources only (D-20): human choices come through
/// choose(); AI choices are made at the start of each turn from that moment's
/// state, before any human choice exists (D-18: no insight into the player's
/// choices). All randomness comes from the battle's own stream (A-4), seeded
/// in the Setup: the same seed, parties and choices replay the same battle.
///
/// Parties are generic (P-13): a party given as data (PartyData) brings its
/// members, presenters, AI flags, rank and flags; a party of records (the
/// player's, or creatures met without an owner, PARTY_UNOWNED) brings
/// monster::Party records. A battle opens with each party's presenters and
/// intro (Phase::Intro), then the send-outs.
///
/// Positions and sides come from the format (data); the engine runs any
/// format the data defines up to kMaxPositions (1 vs 1, 2 vs 2 and 3 vs 3 in the
/// test data). Moves, passives, items, statuses, volatiles, weathers and side
/// and field conditions run effect code (capsule/battle/effect.h; the engine
/// dispatches its hooks, src/battle/effects.cpp, implements the VM's host
/// functions, src/battle/effect_host.cpp, and keeps the conditions' state:
/// each battler's volatile bank, the sides' and the field's conditions,
/// src/battle/conditions.cpp).
///
/// G3 (D-26 section 3): the Item action (ActionKind::Item) uses an item from a party's bag (Participant::bag; an AI
/// party's PartyData::items, chosen by its profile's item rules) and runs its use_battle logic; that logic's
/// `recruit` (src/battle/recruit.cpp, capsule/battle/data/recruit.h) may end the battle with Outcome::Recruited,
/// the record joining the player's party or the storage boxes (Setup::storage). Not in this version: battle
/// animations, link and recorded battles, SOS calls, joins, Battle Royal scoring and bosses.
#pragma once

#include "capsule/battle/data.h"
#include "capsule/combine/eggs.h"
#include "capsule/core/pod.h"
#include "capsule/core/random.h"
#include "capsule/inventory/bag.h"
#include "capsule/monster/monster.h"
#include "capsule/monster/transfer.h"
#include "capsule/storage/storage.h"

#include <cstdint>

namespace capsule::battle
{

/// Who makes a party's choices.
enum class Controller : uint8_t
{
   Human,   ///< choose() calls.
   Ai,      ///< The AI (D-18), by the party's AI profile (data/ai.h, G1).
   NUM_CONTROLLERS
};

/// Battle outcome from the human player's side (C-08e).
enum class Outcome : uint8_t
{
   None,
   Victory,
   Defeat,
   Stalemate,
   Escaped,
   Recruited,   ///< The player recruited a battler (effect `recruit`, G3): State::recruit says where it went.
   NUM_OUTCOMES
};

/// What step() did.
enum class Progress : uint8_t
{
   Busy,    ///< Did some work (events may be waiting); call step() again.
   Input,   ///< Waiting for a human choice (request()).
   Ended,   ///< The battle is over (State::outcome).
   NUM_PROGRESS
};

/// Participant::ai_profile: the party data's profile, or the default one (G1).
inline constexpr uint8_t kAiProfileOfData = 0;

/// One party in the setup. It may give data (presenters, AI profile, rank,
/// flags; its members unless records are given) and records (the members).
struct Participant
{
   const PartyData* data;      ///< Party data, or nullptr.
   monster::Party* records;    ///< Member records, or nullptr (the data's members).
   const text::Code* name;     ///< Name ({party}) instead of the data's, or nullptr.
   Controller controller;      ///< Human or AI.
   uint8_t side;               ///< Side in the format.
   uint8_t flags;              ///< PartyFlag bits (added to the data's).
   uint8_t rank;               ///< Rank without data (foe_rank).
   uint8_t ai_profile = kAiProfileOfData;   ///< AI profile + 1 (BattleData::ai_profiles), or kAiProfileOfData
                                            ///< (the data's, else BattleData::ai_default; G1).
   inventory::Bag* bag = nullptr;   ///< The bag its Item actions use (G3; a human party's), or nullptr: no bag
                                    ///< (an AI party with data uses PartyData::items).
};

/// Setup flag bits (Setup::flags; P-9 bit positions).
enum SetupFlag : uint8_t
{
   SETUP_NO_ESCAPE,   ///< Nobody can run from this battle.
   SETUP_NO_RECRUIT,  ///< Nobody can be recruited in this battle (effect `recruit` refuses; G3).
   NUM_SETUP_FLAGS    ///< Number of flags.
};

/// Battle setup.
struct Setup
{
   Participant participants[kMaxParties];   ///< Participants...
   uint8_t count;                        ///< ...used.
   uint8_t format;                       ///< Format index.
   uint8_t flags;                        ///< SetupFlag bits.
   uint8_t weather;                      ///< Starting weather, or kNone8.
   uint32_t seed;                         ///< Battle RNG seed.
   uint32_t owner_id;                     ///< The player's owner id (exp_policy outsider "owner_id": records
                                          ///< with another OwnerId earn exp_gain() with outsider = 1).
   storage::Storage* storage = nullptr;   ///< The storage boxes a recruited creature goes to when the player's party
                                          ///< is full (G3), or nullptr: none.
};

/// Codes in a battler's display name (a nickname's length), without its end code.
inline constexpr int kNameCodes = 12;
static_assert(kNameCodes == monster::kMaxNicknameCodes, "the Nickname role's longest count is a battler's name");

/// Move slots of the build's layout (monster::Layout::move_slots): battlers and member data hold
/// kMaxMoveSlots, the engine uses the first move_slots() of them.
/// @return 1 to kMaxMoveSlots (0 without battle data)
inline int move_slots()
{
   return monster::kLayout.move_slots;
}

/// "Not on the field" position.
inline constexpr uint8_t kBench = 0xFF;
/// Move slot meaning "the fallback move" (no move's cost can be paid; it pays nothing).
inline constexpr uint8_t kFallbackSlot = 0xFF;
/// Move slot meaning "the move the battler is locked into" (effect `lock`; Battler::lock_move).
inline constexpr uint8_t kLockedSlot = 0xFE;
/// Move slot of a chained move (effect `use_move`; no checks, no costs).
inline constexpr uint8_t kChainSlot = 0xFD;
/// Battler::lock_flags: the lock is set.
inline constexpr uint8_t kLockSet = 0x80;
/// The answer to a level-up's RequestKind::LearnMove that keeps every move (the new one is not learned).
inline constexpr uint8_t kKeepMoves = 0xFF;

/// Battler flag bits.
enum BattlerFlag : uint8_t
{
   BATTLER_FAINTED,     ///< HP reached 0.
   BATTLER_ACTED,       ///< Acted this turn.
   BATTLER_FROM_RECORD, ///< Built from a party record (written back at the end).
   BATTLER_ANNOUNCED,   ///< Sent out at least once.
   BATTLER_EGG,         ///< B2: an egg the battle refuses (never sent in; structure rules.allow_eggs).
   BATTLER_QUIET_ITEM,  ///< C9: its held item is read only (a field context's input: no hooks, points or tags).
   NUM_BATTLER_FLAGS
};

/// The last damage a battler took from a move (D-23 section 1.5; effect `holder.taken.*`).
struct Taken
{
   uint16_t amount;    ///< HP it took (0: none yet since it entered).
   uint8_t from;       ///< Who dealt it (battler), or kBench.
   uint8_t category;   ///< The move's category, or kNone8.
   uint16_t move;      ///< The move, or kNone16.
   uint16_t turn;      ///< Turn it happened (State::turn).
};
CAPSULE_POD(Taken, 8);

/// One creature in a battle (RAM). Holds what level-ups need, so a battle
/// never reads ROM tables back into RAM or the party records
/// until it ends.
struct Battler
{
   text::Code name[kNameCodes + 1];          ///< Display name (kCodeEnd-terminated).
   uint8_t party;             ///< Owning party (State::parties).
   uint16_t species;           ///< Species index.
   uint16_t hp;                ///< Current HP.
   uint16_t stats[kMaxStats];  ///< Permanent stats (BattleData::stat_count; max_hp() is the pool stat).
   uint16_t moves[kMaxMoveSlots];  ///< Move indices (kNone16 = empty).
   uint16_t item;              ///< Held item index (0 = none).
   uint16_t last_hit_by;       ///< Last damaging move that hit it (C-08j).
   uint16_t last_move;         ///< Last move it used (kNone16 = none).
   uint16_t targeted_by;       ///< Last move that targeted it since it entered, any move (kNone16 = none; E13b).
   uint16_t pool[kMaxPools];   ///< Battler resources besides the hit points (ResourceData::slot; D-25).
   uint8_t move_pool[kMaxMoveResources][kMaxMoveSlots];   ///< Move resources per slot (e.g. PP).
   uint8_t raises[kMaxMoveResources][kMaxMoveSlots];      ///< Their raises per slot (ResourceData::raises; a
                                                          ///< newly learned move has 0).
   uint8_t position;          ///< Field position, or kBench.
   uint8_t party_slot;        ///< Member index in its party.
   uint8_t level;             ///< Level.
   uint8_t status;            ///< Primary status (0 = none).
   uint8_t status_counter;    ///< Primary-status counter (effect `status_counter`: sleep turns, ...; the
                              ///< PrimaryStatus role keeps it above the status id).
   uint8_t status_aux;        ///< Primary-status scratch (effect `status_aux`: escalation, ...).
   uint8_t types[kMaxTypesPer]; ///< The species' types, then added ones (kNone8 = none).
   uint8_t passive;           ///< Passive index (kNone8 = none).
   uint8_t trait;             ///< Trait index.
   uint8_t form;              ///< Form (growth::species_view(): its base stats, types, passives; G4).
   uint8_t flags;             ///< BattlerFlag bits.
   uint8_t genes[kMaxStats];  ///< Genes (for level-ups).
   uint8_t training[kMaxStats]; ///< Training values.
   uint8_t awaken[kMaxStats]; ///< Awakening values.
   int8_t stages[kMaxStages]; ///< Stage values (BattleData::stages order).
   uint8_t tiebreak;          ///< This turn's tie byte (TurnRules::tie RandomByte: drawn per battler at the start
                              ///< of each turn, it orders battlers whose actions share a bracket and a speed;
                              ///< unused under Position ties).
   uint8_t lock_target;       ///< Locked action's target position (kBench: default).
   uint8_t lock_flags;        ///< kLockSet | effect::kLockFree, or 0 without a lock.
   uint16_t lock_move;        ///< Move the next action is locked into (effect `lock`).
   uint32_t vol;               ///< Volatile bits (BattleData::volatiles order; D-23 section 1.5 volatile bank).
   uint8_t vcounter[kMaxVolatileCounters];   ///< Volatile counters (VolatileData::counter slots).
   uint16_t vword[kMaxVolatileWords];        ///< Volatile words (VolatileData::word slots).
   uint8_t vsource[kMaxBoundVolatiles];      ///< Who started each bound volatile (VolatileData::source slots;
                                             ///< kBench: nobody; G1).
   Taken taken;                ///< The last damage it took from a move.
   uint32_t faced;             ///< Opposing battlers it was on the field with (EXP).
   uint32_t exp;               ///< Experience.
};

/// The conditions of one side (RAM; D-23 section 1.5).
struct SideState
{
   uint16_t cond;                   ///< Condition bits (BattleData::side_conds order).
   uint8_t turns[kMaxSideConds];    ///< Turns left per condition (0 = until it is ended).
   uint8_t layers[kMaxSideConds];   ///< Layers per condition (1 when it is on and not layered).
};

/// The field's conditions (RAM; the weather stays State::weather, the exclusive field condition).
struct FieldState
{
   uint16_t cond;                   ///< Condition bits (BattleData::field_conds order).
   uint8_t turns[kMaxFieldConds];   ///< Turns left per condition (0 = until it is ended).
};

/// What a delayed hit keeps of its user when the move is used (effect `delay_hit`, G1; Delayed::snap): the
/// delayed-hit record (BattleData::delayed_hits) says which of them its landing hit reads instead of the user's
/// values at that time.
struct DelayedSnapshot
{
   uint16_t stats[kMaxStats];      ///< The user's stats.
   uint16_t defense;               ///< The target's stat that resists the move's category (DELAYED_TARGET_DEFENSE).
   int8_t stages[kMaxStages];      ///< The user's stages.
   int8_t defense_stage;           ///< The target's stage of that stat.
   uint8_t types[kMaxTypesPer];    ///< The user's types.
   uint8_t level;                  ///< Its level.
   uint8_t rule;                   ///< The delayed-hit record (BattleData::delayed_hits), or kNone8: a plain delay.
};

/// A move's pending effect at a field position (effect `delay`, E13b; `delay_hit`, G1; State::delayed): its land
/// phase runs on whoever stands there when its turns run out (end-of-turn phase `delayed`).
struct Delayed
{
   uint16_t move;     ///< The move whose land phase runs.
   uint16_t amount;   ///< The value it gets (e.g. damage worked out when the move was used; 0 for a hit).
   uint8_t user;      ///< Who used it.
   uint8_t turns;     ///< Ends of turn until it lands (0: no pending effect).
   DelayedSnapshot snap;   ///< A delayed hit's snapshot of its user (snap.rule kNone8: a plain delay).
};

/// One party in a battle (RAM).
struct PartyState
{
   const PartyData* data;      ///< Party data, or nullptr.
   monster::Party* records;    ///< Records, or nullptr.
   const text::Code* name;     ///< Its name ({party}), or nullptr.
   Controller controller;      ///< Human or AI.
   uint8_t side;               ///< Side.
   uint8_t first_battler;      ///< First battler in State::battlers.
   uint8_t count;              ///< Battlers.
   uint8_t positions;          ///< Positions it owns (bit mask).
   uint8_t ai_profile;         ///< Its AI profile (BattleData::ai_profiles; G1).
   uint8_t flags;              ///< PartyFlag bits (the participant's and the data's).
   uint8_t rank;               ///< foe_rank, clamped to Rules::rank_max.
   uint8_t defeated;           ///< 1 once all its battlers fainted.
   uint8_t shown;              ///< Presenter slots on screen (bit per slot in PartyData::presenters).
   uint16_t knockouts;         ///< Foes it knocked out.
   inventory::Bag* bag;        ///< The bag its Item actions use (Participant::bag; G3), or nullptr.
   uint16_t items[kMaxPartyItems];   ///< An AI party's items left (PartyData::items; 0: used or none; G3).
};

/// What a battler does this turn.
enum class ActionKind : uint8_t
{
   None,
   Move,
   Switch,
   Run,
   Answer,   ///< The answer to an effect's request (SwitchTarget, LearnMove, Choice): Action::slot.
   /// Use Action::item (G3): from the party's bag (a human party) or PartyData::items (an AI party), on its
   /// inventory::Target: a member (Action::battler, a battler of the user's party), a move slot of one
   /// (Action::battler, Action::slot) or a battler in battle (Action::target, a position).
   Item,
   NUM_ACTION_KINDS
};

/// A choice for one battler.
struct Action
{
   ActionKind kind;     ///< What.
   uint8_t slot;     ///< Move slot (0 to move_slots() - 1, kFallbackSlot, kLockedSlot), or an Answer's value.
   uint8_t target;   ///< Target position for single-target moves (kBench = default).
   uint8_t battler;  ///< Switch: battler to send in.
   uint16_t item = 0;   ///< Item: the item id (G3).
};
CAPSULE_POD(Action, 6);

/// What request() asks for.
enum class RequestKind : uint8_t
{
   Action,    ///< A turn's choice (move, switch, run).
   Replace,   ///< A battler to send into an empty position (Action::kind = Switch).
   SwitchTarget,   ///< An effect asks for a replacement (Action::kind = Answer, slot = the battler).
   LearnMove,      ///< Which move to forget (Answer, slot = the move slot, or kKeepMoves): an effect asks, or
                   ///< a level-up's new move finds every slot taken under LearnFull::Request (State::learn).
   Choice,         ///< An effect asks for one of its options (Answer, slot = the option; reserved).
   NUM_REQUEST_KINDS
};

/// A pending human choice.
struct Request
{
   uint8_t battler;    ///< Battler that acts (Action), or the fainted one (Replace).
   uint8_t position;   ///< Its position.
   RequestKind kind;       ///< What is asked.
   uint8_t reserved;   ///< Padding; 0.
};
CAPSULE_POD(Request, 4);

/// Presentation event kinds.
enum class EventKind : uint8_t
{
   Message,     ///< a = Msg; battler = user, target; b = value / move / passive / item / party; extra = stat.
   StatusText,  ///< a = status; extra = 0 inflicted, 1 residual, 2 skip, 3 cured, 4 already; battler.
   WeatherText, ///< a = weather; extra = 0 started, 1 continues, 2 ended, 3 hurts; battler.
   UseMove,     ///< battler uses move a on position target (animation cue).
   Hp,          ///< battler's HP is now a (was b).
   Status,      ///< battler's status is now a.
   Stage,       ///< battler's stage extra changed by (int16) b.
   Faint,       ///< battler fainted.
   SwitchIn,    ///< battler entered position target.
   SwitchOut,   ///< battler left position target.
   Weather,     ///< weather is now a (kNone8 = clear).
   Exp,         ///< battler gained a | b << 16 experience.
   Level,       ///< battler reached level a.
   Turn,        ///< turn a starts.
   End,         ///< battle over, a = Outcome.
   Presenter,   ///< presenter cue: target = party, a = presenter, b = slot in the party, extra = kCue...
   PresenterText, ///< a presenter speaks: a = presenter, b = party, extra = kText...
   Text,        ///< an owner text (effect `text`): a = text id (BattleData::texts); battler = {user}, target.
   Volatile,    ///< battler's volatile a went on (extra 1) or off (0) (HUD; volatiles with VOLATILE_HUD only).
   Side,        ///< side condition a of side target changed: extra = turns, b = layers (0: it ended).
   Field,       ///< field condition a changed: extra = turns, b = 1 while on (0: it ended).
   Cue,         ///< animation cue a (effect `cue`): battler, target.
   Resource,    ///< battler's resource extra (a battler resource but the hit points: Hp) is now a (was b).
   UseItem,     ///< battler uses item a (G3: the Item action) on target (a battler, or kBench).
   Recruit,     ///< a recruit moment (G3): extra = RecruitMoment, battler = the battler to recruit, target = the
                ///< item's user, a = the moment's value (checks passed, the box from 1, the party slot), b = item.
   Transform,   ///< a transformation or form change (G4, capsule/growth/growth.h): extra = growth::kTransform...,
                ///< battler = the creature, a / b = species (or forms) as that extra says.
   NUM_EVENT_KINDS
};

/// @name Event::extra of Presenter
///@{
inline constexpr uint8_t kCueEnter = 0;    ///< The presenter enters (PresenterData::enter).
inline constexpr uint8_t kCueExit = 1;     ///< It leaves (PresenterData::exit).
inline constexpr uint8_t kCueReturn = 2;   ///< It comes back at the end (PRESENTER_RETURNS).
///@}

/// @name Event::extra of PresenterText
///@{
inline constexpr uint8_t kTextIntro = 0;     ///< PresenterData::intro.
inline constexpr uint8_t kTextDefeat = 1;    ///< PresenterData::defeat.
inline constexpr uint8_t kTextVictory = 2;   ///< PresenterData::victory.
inline constexpr int kPresenterTexts = 3;    ///< PresenterText extras.
///@}

/// @name Event::extra of StatusText
///@{
inline constexpr uint8_t kStatusInflicted = 0;   ///< The status was inflicted.
inline constexpr uint8_t kStatusResidual = 1;    ///< End-of-turn damage.
inline constexpr uint8_t kStatusSkip = 2;        ///< The battler cannot act.
inline constexpr uint8_t kStatusCured = 3;       ///< The status ended.
inline constexpr uint8_t kStatusAlready = 4;     ///< The battler already has a status.
inline constexpr int kStatusTexts = 5;           ///< StatusText extras.
///@}

/// @name Event::extra of WeatherText
///@{
inline constexpr uint8_t kWeatherStarted = 0;     ///< The weather started.
inline constexpr uint8_t kWeatherContinues = 1;   ///< It goes on this turn.
inline constexpr uint8_t kWeatherEnded = 2;       ///< It ended.
inline constexpr uint8_t kWeatherHurts = 3;       ///< It hurts the battler.
inline constexpr int kWeatherTexts = 4;           ///< WeatherText extras.
///@}

/// One presentation event.
struct Event
{
   EventKind kind;      ///< Kind.
   uint8_t battler;   ///< Subject battler.
   uint8_t target;    ///< Target battler or position (see kind).
   uint8_t extra;     ///< Kind-specific.
   uint16_t a;         ///< Kind-specific.
   uint16_t b;         ///< Kind-specific.
};
CAPSULE_POD(Event, 8);

/// Events the ring buffer holds.
inline constexpr int kEventCapacity = 256;
/// Room step() needs before it starts a unit of work. Every unit is small:
/// the opening send-outs, one battler's switch-in effects, one target's
/// accuracy check, one effect component or one hit of a move (a phase program's
/// segment between yields), one
/// experience award or one level-up, one battler's end-of-turn effects, one
/// replacement. The compilers cap what one unit can do (effect programs'
/// event budgets, moves learned per level) so a unit fits.
inline constexpr int kEventsPerStep = 128;

/// Internal phase (State::phase).
enum class Phase : uint8_t
{
   Intro,
   Start,
   StartHooks,
   Choose,
   Execute,
   EndTurn,
   Replace,
   Ended,
   NUM_PHASES
};

/// MoveCursor::flags bits.
enum CursorFlag : uint8_t
{
   CURSOR_CHECKED,   ///< The current target's protect / accuracy / immunity checks are done.
   CURSOR_SHOWN,     ///< Effectiveness was shown for the current target.
   CURSOR_SPREAD,    ///< The move hits more than one target.
   CURSOR_BEGUN,     ///< The current phase's program was begun (MoveCursor::run).
   CURSOR_LOCKED,    ///< The move came from a lock (effect `locked`).
   CURSOR_CRIT,      ///< The latest hit was critical (effect `crit`).
   CURSOR_ABSORBED,  ///< A before_damage handler took the latest hit (effect `absorb`: a decoy; ref `absorbed`, G1).
   NUM_CURSOR_FLAGS
};

/// Where a move in progress stands (MoveCursor::stage).
enum class MoveStage : uint8_t
{
   Use,        ///< The move's use phase (once).
   Targets,    ///< Choosing the targets (target phase or the target kind).
   Check,      ///< The current target's checks.
   Hit,        ///< The move's hit phase on the current target.
   AfterMove,  ///< The move's after_move phase, then the user's after_move hooks.
   NUM_STAGES
};

/// A move in progress: step() runs it one unit at a time. A phase program that yields or asks is
/// suspended in `run` (effect::EffectRun) and resumed by the next unit.
struct MoveCursor
{
   effect::EffectRun run;             ///< The phase program in progress (ctx.dealt: damage to this target).
   uint16_t move;                      ///< Move, or kNone16 when no move is running.
   uint16_t chain_move;                ///< A move to use right after this one (effect `use_move`), or kNone16.
   uint8_t user;                      ///< Battler using it.
   uint8_t chain_user;                ///< Who uses the chained move.
   uint8_t count;                     ///< Targets (1 with kBench for a field move).
   uint8_t targets[kMaxPositions];    ///< Target battlers.
   uint8_t index;                     ///< Current target.
   MoveStage stage;                   ///< Where it stands.
   uint8_t flags;                     ///< CursorFlag bits.
   uint8_t chosen;                    ///< The action's target position (locks keep it).
   uint8_t hits;                      ///< Hits landed on the current target (effect `hits`).
   uint8_t ask;                       ///< RequestKind of a run waiting for an answer, or kNone8.
   uint8_t ask_battler;               ///< Who answers it.
   uint8_t answer;                    ///< The answer choose() took.
   uint8_t answered;                  ///< 1 once it is there.
   /// The type the move hits hit_target with (C5: the move_type point's result, worked out once per target; hooks,
   /// points and the damage read it), or kNone8 before its targets (the move's own type is read).
   uint8_t hit_type;
   uint8_t hit_category;              ///< The category it hits with (BattleData::hit_categories when the hit type
                                      ///< is not the move's own, else the move's).
   uint8_t hit_target;                ///< The battler hit_type is for (kBench: a field move).
};

/// One hook dispatch in progress (State::dispatch; src/battle/effects.cpp).
struct Dispatch
{
   uint8_t battler;     ///< Holder.
   uint8_t hook;        ///< effect::Hook.
   uint8_t source;      ///< Source of the owner running now (detail::Source).
   uint8_t announced;   ///< Sources announced so far (bit per source).
   uint8_t verdict;     ///< Verdict so far (detail::kVerdict*).
   uint8_t reserved;    ///< Padding; 0.
   uint16_t heal_q8;    ///< Immune: the share of max HP it heals.
   uint16_t owner;      ///< A condition-own hook's owner (run_own()), or kNone16 for a dispatch over every owner
                        ///< the battler holds: the re-entry guard is keyed on battler, hook and owner.
};
CAPSULE_POD(Dispatch, 10);

/// A level-up's move offer waiting for an answer (State::learn; LearnFull::Request).
struct LearnOffer
{
   uint16_t move;      ///< The move it would learn.
   uint8_t battler;    ///< Who learns it, or kBench when nothing is offered.
   uint8_t next;       ///< Learnset entry the level's learning goes on from once it is answered.
   uint8_t answered;   ///< 1 once choose() took the answer.
   uint8_t answer;     ///< The move slot to forget, or kKeepMoves.
};
CAPSULE_POD(LearnOffer, 6);

/// Where a recruited record went (State::recruit::placed; G3).
enum class Placed : uint8_t
{
   Nowhere,   ///< Not recruited (yet).
   Party,     ///< The player's party (RecruitState::slot: its member index).
   Storage,   ///< A storage box (RecruitState::slot: its storage slot).
   NUM_PLACES ///< Number of places.
};

/// The Item action in progress and what a recruit did (State::recruit; G3).
struct RecruitState
{
   uint16_t item;      ///< The item the Item action in progress uses (effect `recruit` runs only then), or kNone16.
   uint8_t user;       ///< Its user (battler), or kBench.
   uint8_t battler;    ///< The recruited battler, or kBench.
   uint16_t slot;      ///< Where its record went (party member or storage slot).
   Placed placed;      ///< Where that is.
   uint8_t passed;     ///< Checks the last recruit passed.
};
CAPSULE_POD(RecruitState, 8);

/// A battle (RAM).
///
/// Where State lives: it is large (several KB: the event ring, every
/// battler, the effect VM frames) and step() touches only a few of its fields
/// per unit of work, so on the GBA it belongs in EWRAM, not in the 32 KB
/// IWRAM: declare it (and the monster::Party records start() copies from and
/// write_back() copies to) as a static with CAPSULE_EWRAM_BSS
/// (capsule/core/memory.h), as sandbox_battle and test_battle do. Never put
/// it on the stack. EWRAM costs 2 wait states per 16-bit access (a 32-bit
/// access is two); a step() reads and writes far less than a frame's worth
/// of State, so the cost is small next to the IWRAM it frees.
struct State
{
   rng::Stream rng;                             ///< Battle stream (A-4).
   PartyState parties[kMaxParties];         ///< Parties.
   Battler battlers[kMaxBattlers];          ///< Every creature of every party.
   Action actions[kMaxPositions];          ///< This turn's choices per position.
   Event events[kEventCapacity];          ///< Presentation ring buffer.
   MoveCursor mv;                              ///< Move in progress.
   effect::Machine vm;                         ///< Effect VM frames of hook runs and its fault count.
   Dispatch dispatch[effect::kMaxEffectDepth];  ///< Hook dispatches in progress (no battler re-enters a hook
                                                ///< of the same owner).
   uint32_t exp_pending[kMaxBattlers];       ///< Experience earned, not yet shown.
   uint32_t level_pending;                   ///< Battlers with level-ups to show (bit per battler).
   uint32_t owner_id;                        ///< Setup::owner_id.
   LearnOffer learn;                         ///< A level-up's move offer waiting for an answer.
   uint8_t dispatch_depth;                   ///< Entries of dispatch in use.
   uint8_t at[kMaxPositions];               ///< Battler on each position (kBench = empty).
   uint8_t order[kMaxPositions];   ///< Execution order (positions; battlers at the start and end of a turn).
   uint8_t order_count;   ///< Entries in order.
   uint8_t order_next;   ///< Next entry to run.
   uint8_t party_count;   ///< Parties used.
   uint8_t battler_count;   ///< Battlers used.
   uint8_t format;   ///< Format index.
   uint8_t flags;   ///< SetupFlag bits.
   Phase phase;   ///< Where the turn loop is.
   Outcome outcome;   ///< Result once Ended.
   uint8_t weather;   ///< Active weather, or kNone8.
   uint8_t weather_turns;   ///< Turns left (0 = until replaced).
   uint8_t cond_next;   ///< End of turn: the next side or field condition (side * kMaxSideConds + condition).
   uint8_t escape_attempts;   ///< Failed escape attempts.
   uint8_t sub;   ///< Stage within the phase (end of turn; the party whose intro is next).
   uint8_t events_lost;   ///< Events dropped on a full ring (0 unless a bound was wrong).
   uint16_t ev_head;   ///< First queued event.
   uint16_t ev_count;   ///< Queued events.
   uint8_t winning_side;   ///< Winner (0xFF = none).
   uint16_t turn;   ///< Turn number (1 = first).
   uint32_t reward;   ///< Reward won (the ruleset's reward() per defeated party).
   SideState sides[kMaxSides];   ///< Side conditions per side.
   FieldState field;             ///< Field conditions.
   Delayed delayed[kMaxPositions];   ///< Pending effects per position (effect `delay`, E13b; `delay_hit`, G1).
   uint8_t landing;   ///< The position whose delayed hit lands now (its snapshot stays in delayed[] until it is
                      ///< done; G1), or kBench.
   uint32_t trap_told;   ///< Battlers whose try_switch texts this turn's refused choices showed (bit per battler;
                         ///< choose() says why once per turn, C5).
   storage::Storage* storage;   ///< Setup::storage (G3).
   RecruitState recruit;        ///< The Item action in progress and the recruit's result (G3).
   uint32_t levelled;           ///< Battlers that levelled up in this battle (bit per battler; growth's level_up
                                ///< trigger once it is over, growth::levelled_members(); G4).
};

/// Starts a battle: copies the parties in and queues the opening events.
/// @param s      state to fill
/// @param setup  participants, format, flags, seed
/// @return false if the setup is invalid: bad format or side, a party with
///         nothing to send out or no position, an unowned party larger than
///         its positions, more creatures than kMaxBattlers
bool start(State& s, const Setup& setup);

/// Runs one unit of work.
/// @param s  the battle
/// @return Busy, Input or Ended
Progress step(State& s);

/// The pending human choice, if any.
/// @param s    the battle
/// @param out  filled with the request
/// @return true if a choice is needed
bool request(const State& s, Request& out);

/// Gives a human choice for the pending request.
/// @param s        the battle
/// @param battler  the request's battler
/// @param a        the choice
/// @return false if the choice is not allowed (a cost it cannot pay, unreachable target, ...)
bool choose(State& s, uint8_t battler, const Action& a);

/// Takes the oldest presentation event.
/// @param s    the battle
/// @param out  the event
/// @return false if none is queued
bool pop_event(State& s, Event& out);

/// @name Queries for presentation and controllers
///@{

/// The battle's format.
/// @param s  the battle
/// @return its FormatData
const FormatData& format(const State& s);
/// Whether a move slot can be chosen (player and AI): it has a move and the battler can pay all its costs
/// (D-25).
/// @param s        the battle
/// @param battler  the battler
/// @param slot     0 to move_slots() - 1
/// @return true if usable
bool move_usable(const State& s, uint8_t battler, uint8_t slot);
/// Positions a move may target when chosen by @p battler (bit mask; 0 = no choice needed).
/// @param s        the battle
/// @param battler  the user
/// @param move     move index
/// @return mask of valid target positions
uint8_t target_choices(const State& s, uint8_t battler, uint16_t move);
/// Whether a battler may be switched in now as a turn's choice: bench, not fainted, same party, not taken by an
/// ally's switch, and the active battler is not trapped (is_trapped()). Forced switches (effect `switch`, an
/// effect's SwitchTarget request) are not trapped.
/// @param s        the battle
/// @param active   the battler being replaced
/// @param bench    the candidate
/// @return true if allowed
bool can_switch_to(const State& s, uint8_t active, uint8_t bench);
/// Whether a battler may use an item as this turn's choice (G3): the item's use is battle or both, it has
/// use_battle logic, and the battler's party has one left that this turn's other choices have not taken (a human
/// party's bag, an AI party's PartyData::items).
/// @param s        the battle
/// @param battler  the user
/// @param item     item id
/// @return true if usable
bool item_usable(const State& s, uint8_t battler, uint16_t item);
/// Whether an Item action's target fits its item (G3): a member (Action::battler) of the user's party, a move
/// slot of one (Action::battler, Action::slot: a move there), or a battler on the field (Action::target: an
/// occupied position); a target none takes no target.
/// @param s        the battle
/// @param battler  the user
/// @param a        the action (kind Item)
/// @return true if it fits
bool item_target_valid(const State& s, uint8_t battler, const Action& a);
/// Whether a battler may run: not when the setup has SETUP_NO_ESCAPE or a
/// party on another side has PARTY_NO_ESCAPE, nor while it is trapped (is_trapped()).
/// @param s        the battle
/// @param battler  the battler that would run
/// @return true if running is allowed
bool can_escape(const State& s, uint8_t battler);
/// Whether try_switch hooks keep a battler from switching out or running away (C5): its own owners' handlers
/// (holder = it), then the per_foe handlers of each active foe's owners (holder = the foe, other = it); value = the
/// battler either way, and a handler that blocks traps it. A query: the handlers run with their events dropped and
/// the battle's stream, event ring and fault count restored (try_switch handlers only say and block); choose() shows
/// the texts of a refused switch or run once per turn.
/// @param s        the battle
/// @param battler  the battler that would switch out or run
/// @return true when it is trapped
bool is_trapped(const State& s, uint8_t battler);
/// The human player's side (side of the first human party, 0 if none).
/// @param s  the battle
/// @return side index
uint8_t player_side(const State& s);
/// Effective type chart result of a move type against a battler, as a Q8
/// multiplier: the scaling tiers of its types combined by BattleData::combine (Sequential as Multiply: x1 scaled
/// by each in turn); a decisive Zero tier gives 0 and a decisive Ko tier 0x7FFF.
/// @param s        the battle
/// @param type     attacking type
/// @param battler  defender
/// @return Q8 multiplier
int32_t effectiveness(const State& s, uint8_t type, uint8_t battler);
///@}

/// @name Previews
/// The battle's own calculations for given battlers, without acting: they run
/// the same code as real moves (tools, damage calculators, golden tests). They
/// queue no events and draw from the battle's stream only where noted.
///@{

/// Damage a move would deal (type tiers, then the ruleset's damage pipeline with its points).
/// @param s       the battle
/// @param user    attacker (battler index)
/// @param target  defender (battler index)
/// @param move    move index
/// @param spread  the move hits more than one target
/// @param roll    fixed roll of the pipeline's roll steps (0 = lowest), or -1 to draw from the stream
/// @param crit    a critical hit (taken as given: no draw)
/// @return HP the hit takes (at least 1), 0 if it has no effect, the target's HP for a decisive knockout
int32_t damage_preview(State& s, uint8_t user, uint8_t target, uint16_t move, bool spread, int roll, bool crit);
/// Speed used for turn order (the speed pipeline: stage, points).
/// @param s        the battle
/// @param battler  battler index
/// @return effective speed, 1..stat_cap
int32_t speed_preview(State& s, uint8_t battler);
/// Odds that a move hits (the odds the accuracy pipeline draws with).
/// @param s       the battle
/// @param user    attacker
/// @param target  defender
/// @param move    move index
/// @return n in m (m = 0: the move never misses; n >= m: always hits)
Odds hit_chance_preview(const State& s, uint8_t user, uint8_t target, uint16_t move);
/// Odds of a critical hit (the odds the crit pipeline draws with).
/// @param s     the battle
/// @param user  attacker
/// @param move  move index
/// @return n in m (m = 0: never)
Odds crit_chance_preview(const State& s, uint8_t user, uint16_t move);
/// Odds that a secondary effect or a status of battler @p by lands on battler @p on (the odds the effect logic's
/// `odds(n, m, by, on)` draws with: the ruleset's effect_odds pipeline, E9).
/// @param s     the battle
/// @param by    the battler causing it
/// @param on    the battler it lands on
/// @param n     successes the logic asks for
/// @param m     ...in this many
/// @param move  the context move (point handlers may read it), or kNone16
/// @return n in m after the pipeline (n >= m: always succeeds)
Odds effect_odds_preview(const State& s, uint8_t by, uint8_t on, uint16_t n, uint16_t m, uint16_t move = kNone16);
/// The odds of one recruit check (G3; what effect `recruit` draws each check with).
struct RecruitOdds
{
   uint8_t sure;      ///< 1: every check passes without a draw.
   uint32_t chance;   ///< Successes...
   uint32_t out_of;   ///< ...in this many (up to 65536).
};
/// The odds one recruit check of @p user's item against @p target would draw with (the ruleset's recruit_odds
/// pipeline with the manifest's recruit rate and status bonus; its rolls fixed, no draw; G3).
/// @param s       the battle
/// @param user    the item's user
/// @param target  the battler to recruit
/// @param bonus   the item logic's bonus (0-255)
/// @return the odds (out_of 0 for a battler that does not exist)
RecruitOdds recruit_odds_preview(const State& s, uint8_t user, uint8_t target, int32_t bonus);
/// Damage of a self-hit (a confused battler hitting itself) at Rules::self_hit_power.
/// @param s        the battle
/// @param battler  battler index
/// @param roll     fixed roll of the pipeline's roll steps (0 = lowest), or -1 to draw from the stream
/// @return HP it takes (at least 1)
int32_t self_hit_preview(State& s, uint8_t battler, int roll);
///@}

/// Formats an event's text into @p out (battle placeholders expanded). Field placeholders (FD 80-FE: `{player}`
/// and the other names a game's scripts declare, D-22 decision 6) are kept as they are for the printer, which
/// resolves them through text::set_field_resolver(); one that no longer fits is dropped.
/// @param s    the battle
/// @param e    a Message, StatusText, WeatherText, PresenterText or Text event
/// @param out  output buffer
/// @param cap  buffer size in codes (>= 2)
/// @return false if the event has no text
bool format_text(const State& s, const Event& e, text::Code* out, int cap);

/// Builds a fresh creature record: the layout's defaults, species, level, experience for the level, moves
/// (learn.default_moves: the learnset's last or first move_slots() up to the level), then the
/// manifest's generation list in order (its order is the draw order: random ranges, constants, passive slots,
/// derived roles), then every persisted resource full (fill_resources()) and no status.
/// @param rec      record to fill
/// @param species  species index
/// @param level    level (1 to the ruleset's level_max)
/// @param r        stream for the random parts
/// @return false if the layout lacks a required role or the species is invalid
bool make_monster(monster::Record& rec, uint16_t species, uint8_t level, rng::Stream& r);

/// A role's value in a record: the stored value when the layout holds the role; else the value its
/// generation entry gives (derived: rules::derive() over the record; constant: the constant), which is how an
/// unstored custom role (E13a) and an engine role the layout leaves out are read; else the role's initial value.
/// @param rec      the record
/// @param role     the role
/// @param element  its element
/// @return the value
uint32_t record_value(const monster::Record& rec, monster::RoleId role, int element = 0);

/// Re-derives a record's stored derived roles (E13a): every derived entry of the generation list whose role the
/// layout stores, in list order (a later one reads the ones written before it), except the roles in @p keep.
/// Unstored roles need nothing: record_value() computes them on every read.
/// @param rec   the record
/// @param keep  roles left as they are (an import keeps what the sender gave), or nullptr for none
void rederive(monster::Record& rec, const monster::RoleSet* keep = nullptr);

/// Transfer v2 import into the build's layout with the derived roles the sender did not have re-derived (E13a):
/// monster::import_record() (the import rule, capsule/monster/transfer.h), then rederive() keeping the roles the
/// record held. A role the sender had keeps its value, even a derived one (the sender's own may differ).
/// @param info  the build's transfer table (monster::kTransfer: its layout must be monster::kLayout)
/// @param in    the transferred bytes
/// @param size  their count
/// C9: an egg sent to a build whose layout has no egg role (monster::kTransferEgg) is refused (EggRefused, nothing
/// written) unless the structure's `transfer_eggs` is "hatch": then it arrives hatched (the egg role and counter
/// dropped, the stats derived as for any import).
/// @param rec   receives the record (written only when the result is Ok)
/// @return as monster::import_record(), or EggRefused
/// (Inline: a program that never imports links no transfer code, as the battle modules are linked whole.)
inline monster::TransferResult import_record(const monster::TransferInfo& info, const uint8_t* in, int size,
                                             monster::Record& rec)
{
   monster::RoleSet received = {0, 0};
   monster::Record got{};
   monster::TransferResult result =
      monster::import_record(info, in, size, got.bytes, static_cast<int>(sizeof(got.bytes)), &received);
   if (result.status == monster::TransferStatus::Ok && (result.flags & monster::kTransferEgg) != 0 &&
       !combine::transfer_hatches_eggs())
   {
      result.status = monster::TransferStatus::EggRefused;
   }
   if (result.status == monster::TransferStatus::Ok)
   {
      rec = got;
      rederive(rec, &received);
   }
   return result;
}

/// Computes a record's permanent stats.
/// @param rec  the record
/// @param out  stats, in BattleData::stats order (stat_count written)
/// @return false if the record's species is unknown
bool record_stats(const monster::Record& rec, uint16_t out[kMaxStats]);

/// A battler's maximum hit points (its pool stat).
/// @param b  the battler
/// @return max HP
inline uint16_t max_hp(const Battler& b)
{
   return b.stats[kData.pool_stat];
}

/// @name Resources (D-25; capsule/battle/data/resources.h)
///@{

/// A move's base value of a move resource (MoveData::base).
/// @param move      move index
/// @param resource  resource (BattleData::resources index)
/// @return the base, or 0 for an unknown move or a battler resource
int32_t move_base(uint16_t move, uint8_t resource);

/// A battler's pool of a resource.
/// @param b         the battler
/// @param resource  resource
/// @param slot      move slot (move resources)
/// @return the value (0 for an unknown resource)
int32_t resource_value(const Battler& b, uint8_t resource, int slot = 0);

/// A battler's maximum of a resource: its stat (battler resources) or the max formula of the slot's move base and
/// raises (move resources), capped at what the pool and its persist role hold.
/// @param b         the battler
/// @param resource  resource
/// @param slot      move slot (move resources)
/// @return the maximum (0 for an unknown resource or an empty slot)
int32_t resource_max(const Battler& b, uint8_t resource, int slot = 0);

/// Fills a record's persisted resources: each to its maximum for the record's stats, moves and raises (the faint
/// resource too: full hit points).
/// @param rec  the record
void fill_resources(monster::Record& rec);

/// Fills a record's persisted move resources of one move slot to their maximum for its move and raises (0 for an
/// empty slot).
/// @param rec   the record
/// @param slot  move slot
void fill_move_slot(monster::Record& rec, int slot);
///@}

}   // namespace capsule::battle
