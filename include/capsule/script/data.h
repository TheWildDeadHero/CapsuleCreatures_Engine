/// @file
/// Script bank layout (B-1, B-2, B-4): what tools/script_compiler.py emits
/// from a game's `.cscript` sources.
///
/// **Bytecode.** Little-endian, no pointers. Script IDs are 16 bits; branch
/// targets are 16-bit offsets from the first byte of their own script, so a
/// script is relocatable and identical scripts are stored once. An
/// **operand** is a tag byte followed by its value:
///
/// | Tag | Value | Meaning |
/// |---|---|---|
/// | kOpConst8 | 1 byte | constant 0-255 |
/// | kOpConst16 | 2 bytes | signed 16-bit constant |
/// | kOpConst32 | 4 bytes | signed 32-bit constant |
/// | kOpVar | 2 bytes | the value of a variable (constants and variables never alias, B-2) |
/// | kOpQuery | 2 bytes | a game value: Query8, object8 (D-17) |
///
/// A **text reference** is a tag byte (kTextAsset: 4-byte asset key of a text
/// asset; kTextInline: 4-byte offset into Bank::text; C19 kTextBuffer: 4-byte
/// index of a game text buffer, world::text_buffer()) and its 4 bytes.
///
/// The opcodes (Op), their bytes after the opcode, the kind enums their operands select and the operand
/// constants are generated from src/script/script_vm.json into capsule/script/ops.h (D-36 package 2:
/// tools/script_vm.py; the script compiler reads the same manifest).
///
/// I4: Warp's facing8 may carry kWarpNoWait (the script goes on at once), and Route sets the context's last object
/// (kObjectLast). A bank's object_base (I4) is the first local ID of its numbering: object operands below
/// kObjectLast name local ID (operand - object_base).
///
/// A role byte is a capsule::monster::RoleId: an engine role, or a custom
/// role of the game's layout (0x80 + its index; script_ids.h ROLE_<NAME> when
/// the manifest names the game's "battle").
///
/// An object operand is a local ID of the shown map's objects (the map's
/// object list index; game code has world_ids.h OBJ_...), kObjectSelf (the object whose talk or sight started the script)
/// or kObjectPlayer (the walker).
///
/// **Field placeholders (D-22 decision 6)**: the manifest's "placeholders"
/// become Bank::placeholders, in declaration order (field text encodes
/// placeholder i as kCodePlaceholder, text::kFirstFieldArg + i). Each is
/// bound to a name slot (text the game sets, kept in Bank::names), a
/// variable (printed in decimal), a term (fixed text in Bank::text) or a
/// role of a party record (read through party::role_get()); see
/// script::placeholder_text() and capsule/text/placeholder.h.
///
/// **Flags and variables (B-2)** live in four lifetime banks. Flag IDs are
/// numbered bank by bank (flag_first), so a flag's bank and bit follow from
/// its ID. Every variable has a width (u8, u16, u32), a bank, a byte offset
/// in its bank and a declared range. The storage is emitted with the bank,
/// sized exactly to the declarations; the save size is the sum of the banks.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/script/ops.h"
#include "capsule/core/types.h"
#include "capsule/text/encode.h"

#include <cstdint>

/// Scripts, flags and variables.
namespace capsule::script
{

/// A script (index into Bank::scripts).
enum class ScriptId : uint16_t
{
};
/// A flag.
enum class FlagId : uint16_t
{
};
/// A variable.
enum class VarId : uint16_t
{
};

inline constexpr int kNumLifetimes = static_cast<int>(Lifetime::NUM_LIFETIMES);   ///< Lifetime banks.
/// C10: the lifetimes a save keeps (every lifetime before Session; Session is last, so the save's sub-banks keep
/// their numbers).
inline constexpr int kSavedLifetimes = static_cast<int>(Lifetime::Session);

/// ScriptInfo::flags bit: the script (or something it calls) can wait.
inline constexpr uint8_t kScriptYields = 1;

/// One script (ROM).
struct ScriptInfo
{
   uint32_t code;   ///< Offset of its first byte in Bank::code.
   uint16_t length;   ///< Bytes.
   uint8_t flags;   ///< kScriptYields.
   uint8_t reserved;   ///< 0.
};
CAPSULE_POD(ScriptInfo, 8);

/// One variable (ROM).
struct VarInfo
{
   int32_t min;   ///< Lowest value (P-18).
   int32_t max;   ///< Highest value.
   uint16_t offset;   ///< Byte offset in its lifetime bank's storage (aligned to its width).
   Lifetime lifetime;   ///< Bank.
   uint8_t width;   ///< 1, 2 or 4 bytes.
};
CAPSULE_POD(VarInfo, 12);

/// One map trigger (ROM).
struct MapTrigger
{
   uint16_t script;   ///< ScriptId to run.
   uint16_t condition;   ///< OnFrame / OnWarpInto: ScriptId of its condition (ends in TriggerTrue or End); else 0xFFFF.
   uint16_t dep_first;   ///< First entry of Bank::deps: flags, or kDepVar | variable.
   uint8_t dep_count;   ///< Entries (0 to kMaxTriggerDeps).
   TriggerKind kind;   ///< When.
};
CAPSULE_POD(MapTrigger, 8);

/// Bank::deps entry bit: a variable rather than a flag.
inline constexpr uint16_t kDepVar = 0x8000;

/// A map's triggers (ROM; sorted by map key).
struct MapScripts
{
   AssetKey map;   ///< The map's asset key (C-02d).
   uint16_t trigger_first;   ///< First entry in Bank::triggers.
   uint8_t trigger_count;   ///< Triggers.
   uint8_t reserved;   ///< 0.
};
CAPSULE_POD(MapScripts, 8);

/// What a field placeholder is bound to (capsule.scripts/1 "placeholders").
enum class PlaceholderKind : uint8_t
{
   Name,     ///< A name slot: Placeholder::param codes of text the game sets (set_name()).
   Var,      ///< A variable (Placeholder::index), printed in decimal.
   Term,     ///< Fixed text: Placeholder::text is its offset in Bank::text.
   Record,   ///< A party record's role: Placeholder::param = party slot, index = its monster::RoleId, text = codes
             ///< of a text role (0: a number, printed in decimal); read through party::role_get().
   NUM_PLACEHOLDER_KINDS   ///< Number of kinds.
};

/// One field placeholder (ROM).
struct Placeholder
{
   PlaceholderKind kind;   ///< What it is bound to.
   uint8_t param;          ///< Name: capacity in codes; Record: party slot; else 0.
   uint16_t index;         ///< Var: VarId; Record: monster::Role; else 0.
   uint32_t text;          ///< Name: its slot's offset in Bank::names; Term: offset in Bank::text; Record: text
                           ///< codes (0: decimal); else 0.
};
CAPSULE_POD(Placeholder, 8);

/// The number of save sub-banks (C6: the save image's Script section, capsule/save/save.h; see SaveRemap): the
/// flags of each lifetime, the variables of each lifetime, the name slots.
inline constexpr int kSaveSubBanks = 2 * kSavedLifetimes + 1;
/// The sub-bank of a lifetime's flags.
/// @param l  lifetime index
/// @return its sub-bank
constexpr int flag_sub_bank(int l)
{
   return l;
}
/// The sub-bank of a lifetime's variables.
/// @param l  lifetime index
/// @return its sub-bank
constexpr int var_sub_bank(int l)
{
   return kSavedLifetimes + l;
}
/// The name slots' sub-bank.
inline constexpr int kNameSubBank = 2 * kSavedLifetimes;
/// SaveMove::to of an entry the build no longer has (its value is dropped).
inline constexpr uint16_t kSaveDropped = 0xFFFF;

/// One entry of an older save shape: where it was and where it goes.
struct SaveMove
{
   uint16_t from;        ///< Flags: its bit in the old bank; variables / names: its byte offset in the old bank.
   uint16_t to;          ///< Flags: the new FlagId; variables: the new VarId; names: the new slot's offset; or
                         ///< kSaveDropped.
   uint8_t from_size;    ///< Variables: the old width (1, 2, 4); names: the old capacity in codes; flags: 1.
   uint8_t to_size;      ///< Names: the new capacity in codes; variables: 1 when the old one was signed (its
                         ///< bytes are sign-extended); flags: 0.
   uint8_t reserved[kPadAfterTwoBytes];   ///< 0.
};
CAPSULE_POD(SaveMove, 8);

/// The bank's storage is kept in a save as sub-banks: the flags of each saved lifetime (kSavedLifetimes: Session is
/// never saved), the variables of each saved lifetime,
/// then the name slots. Each has a shape hash (tools/save_shape.py: FNV-1a 32 over its entries' names, widths and
/// offsets in order), so a save knows the shape its bytes were written in. A game records the shapes it ships in its
/// save history (the scripts manifest's "save_history"); the compiler emits, for each recorded shape that differs
/// from the build's, a SaveRemap of SaveMoves: where each old entry went (a flag to its new FlagId, a variable to its
/// new VarId, a name slot to its new offset), so an older save loads into a newer build. A sub-bank of a shape with
/// no remap is reset when its lifetime is a transient one (daily, area, map) and refuses the save otherwise.
///
/// This struct: one recorded older shape of a sub-bank (its SaveMoves are sorted by from).
struct SaveRemap
{
   uint32_t from_hash;   ///< The older shape's hash.
   uint16_t from_bytes;  ///< Its bytes.
   uint16_t first;       ///< Its first entry in Bank::save_moves.
   uint16_t count;       ///< Its entries.
   uint8_t sub_bank;     ///< The sub-bank (0 to kSaveSubBanks - 1).
   uint8_t reserved;     ///< 0.
};
CAPSULE_POD(SaveRemap, 12);

/// @name Timed field effects (D-27 section 2, package F1; capsule/script/field_effects.h)
/// The structure manifest's `field_effects` (a ward against weak creatures, ...): the script compiler gives each one an
/// implicit permanent variable `field_effect_<name>` (u16: the steps or minutes it has left, 0 = not running), so
/// it is saved in the save image's Script section under the save shape and remap rules (tools/save_shape.py), and,
/// when it has an expiry message, an implicit script `field_effect_end_<name>` that shows it.
///@{

/// What a field effect counts down in.
enum class FieldEffectUnit : uint8_t
{
   Steps,     ///< The walker's steps (the overworld runner counts them).
   Minutes,   ///< Minutes of the game's clock (capsule/core/clock.h clock::minutes()).
   NUM_UNITS  ///< Number of units.
};

/// What starting a field effect that already runs does.
enum class FieldEffectStack : uint8_t
{
   Refresh,    ///< It starts over with the new amount.
   Add,        ///< The new amount is added (up to the variable's maximum).
   Refuse,     ///< Nothing: the start is refused (an item used for it stays in the bag).
   NUM_STACKS  ///< Number of stack rules.
};

/// FieldEffectInfo::flags bit: the effect survives a save (else a loaded save has it stopped).
inline constexpr uint8_t kFieldEffectPersists = 1;
/// FieldEffectInfo::expiry_script: no expiry message.
inline constexpr uint16_t kNoExpiryScript = 0xFFFF;
/// Most field effects a game declares (a pending-expiry bit each in one 32-bit word).
inline constexpr int kMaxFieldEffects = 32;

/// One timed field effect (ROM).
struct FieldEffectInfo
{
   uint32_t label;           ///< Its label's offset in Bank::text (field text, for menus and HUDs).
   uint16_t var;             ///< VarId of its implicit variable (what it has left).
   uint16_t expiry_script;   ///< ScriptId of its implicit expiry script, or kNoExpiryScript.
   FieldEffectUnit unit;     ///< What it counts down in.
   FieldEffectStack stack;   ///< What starting it again does.
   uint8_t flags;            ///< kFieldEffectPersists.
   uint8_t reserved;         ///< 0.
};
CAPSULE_POD(FieldEffectInfo, 12);
///@}

/// @name Step effects (D-27 section 4, package F3; capsule/script/step_effects.h)
/// The structure manifest's `step_effects` (a poison-like status hurting on the walk, a friendship-like role
/// growing, ...): every StepEffectInfo::every steps the engine runs the effect-logic hook `steps` on each party
/// member (capsule/battle/step_effects.h). The script compiler gives each one an implicit permanent variable
/// `step_effect_<name>` (u16, 0 to every - 1: the steps counted towards its next run), kept by the save image's
/// Script section under the save shape and remap rules (tools/save_shape.py), and, when any effect names a script,
/// the implicit variables `step_effect_member` and `step_effect_outcome` that script reads.
///@{

/// What a member whose hit points a step effect's run took to 0 does.
enum class StepDamage : uint8_t
{
   Faint,     ///< It stays at 0 (it fainted).
   Survive,   ///< It keeps 1 hit point.
   NUM_RULES  ///< Number of rules.
};

/// The moments a step effect's messages name (StepEffectInfo::messages index).
enum class StepMoment : uint8_t
{
   Hurt,        ///< The run took hit points (the member still has some).
   Survived,    ///< The survive rule kept it at 1.
   Fainted,     ///< It reached 0.
   NUM_MOMENTS  ///< Number of moments.
};

/// Most step effects a game declares (a due bit each).
inline constexpr int kMaxStepEffects = 16;
/// StepEffectInfo::messages entry without a message.
inline constexpr uint8_t kNoStepMessage = 0xFF;
/// StepEffectInfo::script without a script.
inline constexpr uint16_t kNoStepScript = 0xFFFF;
/// Messages per step effect.
inline constexpr int kStepMoments = static_cast<int>(StepMoment::NUM_MOMENTS);
/// Reserved bytes of a StepEffectInfo (to its 16-byte size).
inline constexpr int kStepInfoReserved = 2;

/// One step effect (ROM).
struct StepEffectInfo
{
   uint32_t label;                   ///< Its label's offset in Bank::text (field text, for menus and HUDs).
   uint16_t var;                     ///< VarId of its implicit variable (steps counted towards the next run).
   uint16_t every;                   ///< Steps between two runs (1 or more).
   uint16_t script;                  ///< ScriptId run after a run with an outcome, or kNoStepScript.
   StepDamage damage;                ///< The damage rule.
   uint8_t messages[kStepMoments];   ///< The battle's message id per StepMoment, or kNoStepMessage.
   uint8_t reserved[kStepInfoReserved];   ///< 0.
};
CAPSULE_POD(StepEffectInfo, 16);
///@}

/// @name Field actions, flight points and the escape point (D-27 section 3, package F2; capsule/script/field_actions.h)
/// The structure manifest's `field_actions` (cross water, clear a bush, light a dark cave, ...): the script compiler
/// resolves each one's target against the world (a behavior, an object kind, a map tag, or none: a menu action), its
/// script, and the flags and variables its requirement reads (the battle data's rules::field_state() references);
/// an asking action gets an implicit wrapper script `field_action_ask_<name>` (its prompt as a yes / no question,
/// then its script). The dispatcher writes the member's party slot to the implicit variable `field_action_member`.
/// The world's `flight_points` get an implicit permanent flag `flight_<name>` each (recorded = visited); a world
/// with escape points (maps with `"escape_point": true`) gets the implicit permanent variables of the escape point.
///@{

/// What a field action is used on.
enum class ActionTarget : uint8_t
{
   None,       ///< Nothing: a menu action anywhere.
   Behavior,   ///< A faced cell's behavior (FieldActionInfo::value: the world's behavior index).
   Object,     ///< A faced map object's kind (value: the world's object kind index + 1, map::ObjectTemplate::kind).
   Tag,        ///< A menu action on maps with a tag (value: the world's map tag index, map::Map::tags bit).
   NUM_TARGETS ///< Number of kinds.
};

/// FieldActionInfo::flags bit: the action asks first (FieldActionInfo::script is its wrapper).
inline constexpr uint8_t kActionAsks = 1;
/// Most field actions a game declares (tools/battlec/field.py MAX_FIELD_ACTIONS).
inline constexpr int kMaxFieldActions = 32;
/// Most flags and variables the field actions' requirements read together (field.py MAX_GAME_REFS).
inline constexpr int kMaxFieldActionRefs = 64;
/// Most flight points of a world (map_compiler.py MAX_FLIGHT_POINTS).
inline constexpr int kMaxFlightPoints = 32;
/// Script ID meaning none (FieldActionInfo::script of a broken record, EscapeVars of a world without escape points).
inline constexpr uint16_t kNoScriptId = 0xFFFF;

/// One field action (ROM; in the structure's order: battle_ids.h FIELD_ACTION_<NAME>).
struct FieldActionInfo
{
   uint32_t label;        ///< Its label's offset in Bank::text (field text, for menus).
   uint16_t script;       ///< ScriptId it runs (an asking action: its wrapper, which asks, then calls the script).
   ActionTarget target;   ///< What it is used on.
   uint8_t value;         ///< The target's index (see ActionTarget).
   uint8_t flags;         ///< kActionAsks.
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
CAPSULE_POD(FieldActionInfo, 12);

/// FieldActionRef::kind: a flag.
inline constexpr uint8_t kRefFlag = 0;
/// FieldActionRef::kind: a variable.
inline constexpr uint8_t kRefVar = 1;

/// One flag or variable a field action's requirement reads (the battle data's rules::field_state() index order).
struct FieldActionRef
{
   uint16_t id;      ///< FlagId or VarId.
   uint8_t kind;     ///< kRefFlag or kRefVar.
   uint8_t reserved; ///< 0.
};
CAPSULE_POD(FieldActionRef, 4);

/// One flight point (ROM; the world's `flight_points` order).
struct FlightPointInfo
{
   uint32_t label;   ///< Its label's offset in Bank::text.
   AssetKey map;     ///< The map it lands on.
   int16_t x;        ///< Landing column.
   int16_t y;        ///< Landing row.
   uint16_t flag;    ///< FlagId of its implicit flag (set: recorded, the flight menu lists it).
   uint8_t facing;   ///< Landing facing (field::Direction).
   uint8_t reserved; ///< 0.
};
CAPSULE_POD(FlightPointInfo, 16);

/// Preview's value of a random pick (its species is drawn when it combines).
inline constexpr int32_t kCombineRandom = 0xFFFF;
/// Most party members a Combine op names (any platform's capsule::combine::kMaxInputs).
inline constexpr int kMaxCombineMembers = 8;

/// Withdraw's value when the money variable holds less than the fee (nothing changes).
inline constexpr int32_t kNurseryPoor = 2;

/// The implicit permanent variables of the escape point (the overworld records it on a warp that leaves a map
/// with `"escape_point": true` for one without): VarIds, or kNoScriptId each without escape points. C8: the map is
/// its asset key (as a flight point's), kept in two u16 variables (a u32 variable stops at 2^31 - 1), so a save
/// still names the same map after maps are added, removed or reordered.
struct EscapeVars
{
   uint16_t map_hi;   ///< The map's asset key's high 16 bits (with map_lo 0: none recorded).
   uint16_t map_lo;   ///< Its low 16 bits.
   uint16_t x;        ///< The cell's column + kEscapeBias.
   uint16_t y;        ///< Its row + kEscapeBias.
   uint16_t facing;   ///< The facing on arrival.
   uint16_t reserved; ///< kNoScriptId.
};
CAPSULE_POD(EscapeVars, 12);

/// Bits of an escape map key half (EscapeVars::map_hi / map_lo).
inline constexpr uint32_t kEscapeKeyHalfBits = 16;
/// Mask of an escape map key half.
inline constexpr uint32_t kEscapeKeyHalfMask = (uint32_t{1} << kEscapeKeyHalfBits) - 1;

/// What an escape coordinate variable holds over the cell (u16 variables hold cells -32768 to 32767).
inline constexpr int32_t kEscapeBias = 0x8000;
///@}

/// A game's compiled scripts, flags and variables.
struct Bank
{
   const uint8_t* code;   ///< Bytecode of every script.
   const ScriptInfo* scripts;   ///< script_count entries.
   const text::Code* text;   ///< Inline texts, each ending with kCodeEnd.
   const VarInfo* vars;   ///< var_count entries.
   const MapScripts* maps;   ///< map_count entries, by key.
   const MapTrigger* triggers;   ///< Every map's triggers.
   const uint16_t* deps;   ///< Trigger dependencies.
   uint8_t* flag_storage[kNumLifetimes];   ///< Flag bits per bank (RAM).
   uint8_t* var_storage[kNumLifetimes];   ///< Variable bytes per bank (RAM).
   uint16_t flag_bytes[kNumLifetimes];   ///< Size of each flag bank.
   uint16_t var_bytes[kNumLifetimes];   ///< Size of each variable bank.
   uint16_t flag_first[kNumLifetimes + 1];   ///< First flag ID of each bank; [kNumLifetimes] = flag count.
   uint16_t script_count;   ///< Scripts.
   uint16_t var_count;   ///< Variables.
   uint16_t map_count;   ///< Maps with triggers.
   uint8_t stack_depth;   ///< Call depth the compiler proved enough (1 to kMaxStack).
   uint8_t placeholder_count;   ///< Field placeholders (0 to text::kMaxFieldArgs).
   const char* const* script_names;   ///< Debug names, or nullptr.
   const char* const* flag_names;   ///< Debug names, or nullptr.
   const char* const* var_names;   ///< Debug names, or nullptr.
   const Placeholder* placeholders;   ///< placeholder_count entries, or nullptr.
   uint8_t* names;   ///< Name slots (RAM): per slot a length byte, then its capacity in codes; or nullptr.
   uint16_t name_bytes;   ///< Size of names.
   uint16_t save_remap_count;   ///< Entries of save_remaps (C6).
   uint32_t save_hashes[kSaveSubBanks];   ///< The build's shape hash of each sub-bank (0: an empty sub-bank; C6).
   const SaveRemap* save_remaps;   ///< The recorded older shapes (save history), or nullptr.
   const SaveMove* save_moves;     ///< Their entries, remap by remap.
   const FieldEffectInfo* field_effects;   ///< field_effect_count entries (F1), in the structure's order, or nullptr.
   uint8_t field_effect_count;             ///< Timed field effects (0 to kMaxFieldEffects).
   // F2 (a bank without them leaves these out of its initializer)
   const FieldActionInfo* field_actions = nullptr;      ///< field_action_count entries, or nullptr.
   const FieldActionRef* field_action_refs = nullptr;   ///< field_action_ref_count entries, or nullptr.
   const FlightPointInfo* flight_points = nullptr;      ///< flight_point_count entries, or nullptr.
   uint8_t field_action_count = 0;                      ///< Field actions (0 to kMaxFieldActions).
   uint8_t field_action_ref_count = 0;                  ///< Their game references (0 to kMaxFieldActionRefs).
   uint8_t flight_point_count = 0;                      ///< Flight points (0 to kMaxFlightPoints).
   uint16_t action_member_var = kNoVar;                 ///< VarId of `field_action_member`, or kNoVar.
   EscapeVars escape = {kNoScriptId, kNoScriptId, kNoScriptId, kNoScriptId, kNoScriptId,
                        kNoScriptId};   ///< The escape point's variables.
   /// C8: VarId of the implicit map-lifetime `map_darkness` (a script-set darkness level + 1, kept across a save
   /// until the next map; 0: the map's own level), or kNoVar.
   uint16_t darkness_var = kNoVar;
   // F3 (a bank without step effects leaves these out of its initializer)
   const StepEffectInfo* step_effects = nullptr;   ///< step_effect_count entries (the structure's order), or nullptr.
   uint8_t step_effect_count = 0;                  ///< Step effects (0 to kMaxStepEffects).
   uint16_t step_member_var = kNoVar;              ///< VarId of `step_effect_member`, or kNoVar.
   uint16_t step_outcome_var = kNoVar;             ///< VarId of `step_effect_outcome`, or kNoVar.
   // B2 (a bank without eggs leaves these out of its initializer)
   uint16_t hatch_script = kNoStepScript;          ///< ScriptId run for each egg that hatches, or kNoStepScript.
   uint16_t egg_member_var = kNoVar;               ///< VarId of `egg_member` (the egg's party slot), or kNoVar.
   // I4 (a bank of the engine's numbering leaves it out of its initializer)
   uint8_t object_base = 0;                        ///< First local ID of the scripts' object numbering (0 or 1).
   // C10 (a bank with the engine's default VM leaves it out of its initializer)
   uint8_t vm_flags = 0;                           ///< kVm... bits: how the VM runs (the manifest's "vm").
};

/// @name Bank::vm_flags (C10: the scripts manifest's "vm" object; 0 is the engine's own behaviour)
///@{

/// "resume": "next_frame": when a wait ends, the next instruction runs on the following frame (a frame-counted
/// game whose script engine ends the frame on the instruction that waited); else it runs the same frame.
inline constexpr uint8_t kVmResumeNextFrame = 1;
/// "random": "modulo": `random(n)` is a 16-bit draw modulo n, drawn even when n is 0 (the result is then 0, as a
/// software modulo by zero gives); else (draw * n) >> 16 and no draw for n <= 0.
inline constexpr uint8_t kVmRandomModulo = 2;
/// "vars": "wrap": a value outside the range of a variable declared with its type's whole range (u8 0-255, u16
/// 0-65535) wraps around it as that integer type overflows (65535 + 1 is 0); other variables, and every variable
/// without the switch, saturate at the range's ends.
inline constexpr uint8_t kVmVarsWrap = 4;
///@}

/// The game's bank (generated; an empty one is linked when a game has no scripts).
extern const Bank kBank;

}   // namespace capsule::script
