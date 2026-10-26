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
/// asset; kTextInline: 4-byte offset into Bank::text) and its 4 bytes.
///
/// Opcodes (Op) and their bytes after the opcode:
///
/// | Op | Bytes | Does |
/// |---|---|---|
/// | End | — | ends the script (and every caller) |
/// | Return | — | returns to the caller, or ends at the outermost level |
/// | Goto | off16 | jumps |
/// | Call | id16 | calls a script |
/// | CallTable | operand, n8, n x id16 | calls the script at the operand's index (declared targets, B-1) |
/// | IfFlagClear | flag16, off16 | jumps when the flag is clear |
/// | IfFlagSet | flag16, off16 | jumps when the flag is set |
/// | IfCompare | cmp8, operand, operand, off16 | jumps when the comparison holds |
/// | SetFlag / ClearFlag | flag16 | |
/// | SetVar / AddVar / SubVar | var16, operand | the variable clamps or saturates to its declared range (P-18) |
/// | Random | var16, operand | var = 0 to operand - 1 from the gameplay stream (A-4; C10: kVmRandomModulo) |
/// | Message | text | opens the box if needed and types the text; waits until it is typed |
/// | WaitButton | — | waits for Accept or Back |
/// | CloseMessage | — | closes the box |
/// | YesNo | var16 | asks; var = 1 for yes, 0 for no (Back = no) |
/// | Choice | var16, cancel8, n8, n x text | menu; var = chosen index, or cancel (0xFF: Back does nothing) |
/// | Delay | operand | waits that many frames |
/// | Native | id16, argc8, argc x operand, var16 | game action (P-13); result into var16 unless 0xFFFF |
/// | TriggerTrue | — | ends a trigger condition as true |
/// | ObjectMove | operand (object), dir8, operand (steps) | walks the object that many cells through anything; waits until it stands |
/// | ObjectFace | operand (object), face8 | turns it: a direction 0-3, kFacePlayer or kFaceSelf (towards that one), kFaceOppositePlayer (C10) |
/// | ObjectShow / ObjectHide | operand (object) | shows (at its template cell) or hides a map object |
/// | Route | operand (object), flags8 (kRouteNoWait), len16, len bytes | runs a move route (capsule/script/route.h); waits until it ends unless kRouteNoWait |
/// | WaitMoves | — | waits until no route started by a script (without repeat) runs |
/// | SelfSwitch | letter8 (0-3 = A-D), on8 | sets a self switch of the context's object |
/// | IfSelfSet / IfSelfClear | letter8, off16 | jumps on a self switch of the context's object |
/// | VarOp | kind8 (VarOpKind), var16, operand | *=, /=, %= (dividing by 0 changes nothing) |
/// | InputNumber | var16, digits8 | number entry; waits for the answer |
/// | WaitKey | var16 | waits for a key press; var = 1 A, 2 B, 3 Select, 4 Start, 5 Right, 6 Left, 7 Up, 8 Down, 9 R, 10 L |
/// | MessagePos | pos8 (0 bottom, 1 middle, 2 top) | where the next messages show |
/// | Warp | key32 (map), operand x, operand y, facing8 (4 keeps), transition8 (Transition, 0xFF the default) | moves the player; waits until the new map is shown |
/// | Place | operand (object), operand x, operand y, facing8 (4 keeps) | puts it on a cell at once |
/// | Scroll | dir8 (4 = back to the player), operand cells, speed8 (1-4) | pans the camera; waits |
/// | Tint | r8, g8, b8 (0-31), strength8 (0-16), frames16 | tints the screen towards a color over frames |
/// | Flash | r8, g8, b8, frames16 | flashes the screen with a color, fading over frames |
/// | Shake | power8, frames16 | shakes the screen |
/// | TimerStart | operand (seconds) / TimerStop | the countdown timer, shown on screen |
/// | Encounters | on8 | encounters on or off |
/// | Vehicle | class8 | the walker's vehicle class (A2, field::set_vehicle()) |
/// | RoleGet | var16, operand (slot), role8, operand (element) | var = an element of a party record's role (party::role_get(); clamps to the variable's range) |
/// | RoleSet | operand (slot), role8, operand (element), operand (value) | writes it (party::role_set(); truncated to the role's width) |
/// | ItemGive | var16 (or kNoVar), operand (item), operand (n) | var = 1 when the bag took all n (items::give(); G2) |
/// | ItemTake | var16 (or kNoVar), operand (item), operand (n) | var = 1 when the bag held n (they are gone; items::take()) |
/// | ItemCount | var16, operand (item) | var = how many the bag holds (items::count()) |
/// | BagOpen | var16 | the bag menu (items::bag_open()); waits; var = the chosen item, 0 for none |
/// | Shop | shop8, var16 (money) | a shop list (items::shop_open()); waits until it closes |
/// | TimeOfDay | var16 | var = the game's time of day (capsule/core/clock.h clock::time_of_day()) |
/// | BoxCount | var16 | var = the records the storage boxes hold (boxes::count(); G3) |
/// | BoxDeposit | var16 (or kNoVar), operand (party member) | moves a party member into the boxes (boxes::deposit()); var = 1 when it went |
/// | BoxWithdraw | var16 (or kNoVar), operand (storage slot) | moves a boxed record into the party (boxes::withdraw()); var = 1 when it came |
/// | BoxesOpen | var16 | the boxes menu (boxes::open()); waits; var = the chosen box + 1, 0 for none |
/// | Transform | var16, operand (party member) | the member's transformation by the script trigger's rules (growth::transform_open(), capsule/script/growth_port.h; G4); waits; var = 0 nothing, 1 changed, 2 stopped |
/// | FieldEffect | kind8 (FieldEffectOp), effect8, [Start: operand (amount)], var16 (or kNoVar) | a timed field effect (capsule/script/field_effects.h; F1): Start starts it by its stack rule (var = 1 started, 0 refused), Active (var = 1 while it runs), Remaining (var = its steps or minutes left) |
/// | Darkness | operand (level) | the shown map's darkness level, 0 (lit) to map::kMaxDarkness, until the next map (world::set_darkness(); F2) |
/// | Flight | kind8 (FlightOp), [Record / Visit: point8], [Visit: transition8], [Menu: var16] | a flight point (capsule/script/field_actions.h): Record sets its flag, Visit warps there (waits), Menu lists the recorded points and warps to the one picked (waits; var = 1 warped, 0 backed out) |
/// | Escape | transition8, var16 (or kNoVar) | warps to the escape point when one is recorded (world::escape(); waits); var = 1 when it warped |
/// | FieldAction | kind8 (FieldActionOp), action8, var16 | Available: var = the first party member the action's requirement holds for, plus 1 (0: none; actions::available()) |
/// | ObjectMoveChecked | operand (object), dir8, operand (steps), var16 (or kNoVar) | C8: as ObjectMove when every step of the walk is allowed by the field's movement rules (world::path_clear(): terrain, elevation, vehicle class, objects); else nothing moves; var = 1 when it walks (waits until it stands), 0 when blocked |
/// | Combine | kind8 (CombineOp), method8, [Run / Preview / Status / Fee: count8, count operands (party slots)], var16 | B1: a combination method (capsule/script/combine_port.h): Open picks the inputs through the menu, previews, asks and combines, Run combines the given members (both wait; var = 0 backed out, 1 combined, 2 refused); Preview: var = the result species + 1 (0: none, kCombineRandom: a random pick), Status: the combine::Status, Fee: the fee |
/// | Nursery | kind8 (NurseryOp), [Deposit / Withdraw / Levels / Fee / Name: operand], [Withdraw: var16 money (or kNoVar)], var16 | B2: the nursery (capsule/script/nursery_port.h): Pick opens the party menu (waits; var = member + 1, 0 backed out), Deposit (var = 1 when it went), Withdraw with the fee taken from the money variable (var = 1 done, 0 refused, kNurseryPoor too poor), Count, Levels (gained), Fee, Ready (var = 1 when a result waits), Odds, Take (var = 1 when it joined the party), Name (the nickname prompt; waits; var = 1 named) |
/// | Jump | id16 | I4: goes on in another script in the same frame (nothing returns here; a `goto` naming a script) |
/// | Lock / Release | kind8 (kLockSelf, kLockAll) | I4: stops / lets go the objects' own movement (world::lock() / release()) |
/// | MessageEx | flags8 (kMessage... bits), text | I4: as Message, typed at once (kMessageInstant), scrolling on by itself (kMessageAutoScroll), or without waiting for the typing (kMessageNoWait) |
/// | WaitMessage | — | I4: waits until the message is typed |
/// | WaitObject | operand (object, or kObjectLast) | I4: waits until that object (or the walker) stands |
/// | ObjectRemove | operand (object) | I4: removes a map object for good on this map (world::remove(): hidden, its hide flag set) |
/// | ObjectVisible | operand (object), on8 | I4: shows or hides its sprite where it stands (it still blocks) |
/// | ObjectTemplate | operand (object), kind8 (TemplateOp), [Pos: operand x, operand y], [Movement: operand] | I4: changes the object's template on the shown map until the next map (world::set_template()) |
/// | Door | kind8 (DoorOp), [all but Wait: operand x, operand y] | I4: a door's animation on a cell (world::door()); Wait waits until it ends |
/// | Weather | kind8 (WeatherOp), [Set: operand] | I4: the weather the next Apply shows (world::weather()) |
/// | Fade | kind8 (FadeOp), speed8 (0: the default), flags8 (kFadeSwap) | I4: fades the screen (world::fade()); waits |
/// | SetMetatile | operand x, operand y, operand (metatile), operand (solid) | I4: changes a cell of the shown map until the next map (world::set_metatile()) |
/// | Respawn | operand | I4: the respawn point (world::set_respawn(): the game's own numbering) |
/// | SetWarp | kind8 (WarpSlot), key32 (map), operand x, operand y | I4: remembers a warp destination (world::set_warp()) |
/// | Sound | kind8 (SoundOp), n8, n x operand | I4: music, sound effects, fanfares, cries (audio::play()) |
/// | WaitSound | kind8 (SoundWait) | I4: waits until that sound ends (audio::playing()) |
/// | Mart | kind8, n16, n x item16 | I4: a mart list (items::mart_open()); waits until it closes |
/// | CallLoose | id16 | I4: calls a script that may recurse; with the stack full (Bank::stack_depth) it goes on as a Jump, as the imported game's own VM does |
/// | WaitState | — | I4: waits while the game is busy (game::state_busy(): a special the game runs over frames) |
/// | Override | apply8 (1 apply, 0 clear), set16 | DS1: a cell override set of the world (index into map::kWorld.override_sets; world::override_set()) |
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

/// Lifetime of a flag or variable (B-2).
enum class Lifetime : uint8_t
{
   Permanent,   ///< Kept until the game changes it.
   Daily,   ///< Cleared at the day rollover (RTC).
   Area,   ///< Cleared when the player leaves an outdoor area and its interiors.
   Map,   ///< Cleared on any map change.
   Session,   ///< C10: kept in RAM only: never saved, cleared at a new game (a reference game's unsaved flags).
   NUM_LIFETIMES   ///< Number of lifetimes.
};
inline constexpr int kNumLifetimes = static_cast<int>(Lifetime::NUM_LIFETIMES);   ///< Lifetime banks.
/// C10: the lifetimes a save keeps (every lifetime before Session; Session is last, so the save's sub-banks keep
/// their numbers).
inline constexpr int kSavedLifetimes = static_cast<int>(Lifetime::Session);

/// Opcodes (see the file comment for operands).
enum class Op : uint8_t
{
   End,   ///< End the script.
   Return,   ///< Return to the caller.
   Goto,   ///< Jump.
   Call,   ///< Call a script.
   CallTable,   ///< Call one of a declared list by index.
   IfFlagClear,   ///< Jump if a flag is clear.
   IfFlagSet,   ///< Jump if a flag is set.
   IfCompare,   ///< Jump if a comparison holds.
   SetFlag,   ///< Set a flag.
   ClearFlag,   ///< Clear a flag.
   SetVar,   ///< Assign a variable.
   AddVar,   ///< Add, saturating.
   SubVar,   ///< Subtract, saturating.
   Random,   ///< Random value below a bound.
   Message,   ///< Type text in the box.
   WaitButton,   ///< Wait for Accept / Back.
   CloseMessage,   ///< Close the box.
   YesNo,   ///< Yes / no question.
   Choice,   ///< Menu.
   Delay,   ///< Wait frames.
   Native,   ///< Game action.
   TriggerTrue,   ///< Trigger condition result.
   ObjectMove,   ///< Walk a map object or the player (B-21).
   ObjectFace,   ///< Turn a map object or the player.
   ObjectShow,   ///< Show a map object.
   ObjectHide,   ///< Hide a map object.
   Route,   ///< Run a move route (D-17).
   WaitMoves,   ///< Wait until no move route runs.
   SelfSwitch,   ///< Set or clear a self switch of the context's object.
   IfSelfSet,   ///< Jump if a self switch is on.
   IfSelfClear,   ///< Jump if a self switch is off.
   VarOp,   ///< Multiply, divide or take the remainder of a variable.
   InputNumber,   ///< Number entry.
   WaitKey,   ///< Wait for a key press.
   MessagePos,   ///< Text box position.
   Warp,   ///< Move the player to another map (or cell), with a transition.
   Place,   ///< Put an object or the player on a cell.
   Scroll,   ///< Pan the camera.
   Tint,   ///< Tint the screen.
   Flash,   ///< Flash the screen.
   Shake,   ///< Shake the screen.
   TimerStart,   ///< Start the countdown timer.
   TimerStop,   ///< Stop it.
   Encounters,   ///< Encounters on or off.
   Vehicle,   ///< Change the walker's vehicle class.
   RoleGet,   ///< Read a party record's role (D-22 decision 2).
   RoleSet,   ///< Write a party record's role.
   ItemGive,   ///< Put items into the bag (G2).
   ItemTake,   ///< Take items from the bag.
   ItemCount,   ///< Count an item in the bag.
   BagOpen,   ///< Open the bag menu.
   Shop,   ///< Open a shop list.
   TimeOfDay,   ///< Read the game's time of day.
   BoxCount,     ///< Count the records in the storage boxes (G3).
   BoxDeposit,   ///< Move a party member into the boxes.
   BoxWithdraw,  ///< Move a boxed record into the party.
   BoxesOpen,    ///< Open the boxes menu.
   Transform,    ///< A party member's transformation (G4).
   FieldEffect,  ///< Start or read a timed field effect (F1, D-27 section 2).
   Darkness,     ///< Set the shown map's darkness level (F2, D-27 section 3).
   Flight,       ///< Record, visit or pick a flight point (F2).
   Escape,       ///< Warp to the escape point (F2).
   FieldAction,  ///< Ask whether a field action is available (F2).
   ObjectMoveChecked,   ///< Walk a map object or the player by the movement rules, or not at all (C8).
   Combine,      ///< Run or ask a combination method (B1).
   Nursery,      ///< The nursery: deposit, withdraw, queries, the result, the nickname prompt (B2).
   // I4 (D-29): what imported event scripts need
   Jump,         ///< Go on in another script (the frame is replaced).
   Lock,         ///< Stop the objects' own movement.
   Release,      ///< Let them move again.
   MessageEx,    ///< A message typed at once, scrolling on, or not waited for.
   WaitMessage,  ///< Wait until the message is typed.
   WaitObject,   ///< Wait until one object stands.
   ObjectRemove, ///< Remove a map object until its flag clears.
   ObjectVisible,   ///< Show or hide an object's sprite where it stands.
   ObjectTemplate,  ///< Change an object's template on the shown map.
   Door,         ///< A door's animation.
   Weather,      ///< Set, reset or apply the weather.
   Fade,         ///< Fade the screen.
   SetMetatile,  ///< Change a cell of the shown map.
   Respawn,      ///< Set the respawn point.
   SetWarp,      ///< Remember a warp destination.
   Sound,        ///< Music, sound effects, fanfares, cries.
   WaitSound,    ///< Wait until a sound ends.
   Mart,         ///< A mart list.
   CallLoose,    ///< A call that becomes a jump on a full stack.
   WaitState,    ///< Wait while the game is busy.
   Override,     ///< DS1: apply or clear a cell override set.
   NUM_OPS   ///< Number of opcodes.
};

/// Game values a script can read (kOpQuery).
enum class Query : uint8_t
{
   PosX,   ///< An object's or the player's cell column.
   PosY,   ///< Its row.
   Facing,   ///< Its facing (0 down, 1 up, 2 left, 3 right).
   MapIndex,   ///< The shown map's index (world_ids.h MAP_...).
   Steps,   ///< The player's steps.
   Timer,   ///< Seconds left on the countdown timer (0 when stopped).
   NUM_QUERIES   ///< Number of queries.
};

/// VarOp operations.
enum class VarOpKind : uint8_t
{
   Mul,   ///< *= (saturating)
   Div,   ///< /= (truncating; by 0: unchanged)
   Mod,   ///< %= (by 0: unchanged)
   NUM_VAR_OPS   ///< Number of operations.
};

/// Screen transitions of warps (D-17).
enum class Transition : uint8_t
{
   None,   ///< Cut.
   Fade,   ///< Fade to black and back.
   White,   ///< Fade to white and back.
   Mosaic,   ///< Mosaic while fading to black.
   WipeH,   ///< Black wipe from the left, uncovered to the right.
   WipeV,   ///< Black wipe from the top, uncovered downwards.
   Box,   ///< A shrinking box, then a growing one.
   Silent,   ///< I4: a fade to black and back without the warp's sound.
   Door,   ///< I4: walking into a door (the game's door sequence).
   Fall,   ///< I4: falling through a hole.
   Teleport,   ///< I4: a teleport.
   Spin,   ///< I4: spinning away.
   NUM_TRANSITIONS   ///< Number of transitions.
};

/// Comparisons of IfCompare.
enum class Compare : uint8_t
{
   Eq,   ///< ==
   Ne,   ///< !=
   Lt,   ///< <
   Le,   ///< <=
   Gt,   ///< >
   Ge,   ///< >=
   NUM_COMPARES   ///< Number of comparisons.
};

/// @name Operand and text-reference tags
///@{
inline constexpr uint8_t kOpConst8 = 0;   ///< 1-byte unsigned constant.
inline constexpr uint8_t kOpConst16 = 1;   ///< 2-byte signed constant.
inline constexpr uint8_t kOpConst32 = 2;   ///< 4-byte signed constant.
inline constexpr uint8_t kOpVar = 3;   ///< 2-byte variable ID.
inline constexpr uint8_t kOpQuery = 4;   ///< Query byte and object byte.
inline constexpr uint8_t kRouteNoWait = 0x80;   ///< Route: the script goes on at once.
inline constexpr uint8_t kTransitionDefault = 0xFF;   ///< Warp: the default transition.
inline constexpr uint8_t kKeepFacing = 4;   ///< Warp / Place: facing kept.
inline constexpr uint8_t kTextAsset = 0;   ///< 4-byte asset key of a text asset.
inline constexpr uint8_t kTextInline = 1;   ///< 4-byte offset into Bank::text.
inline constexpr uint16_t kNoVar = 0xFFFF;   ///< Native without a result variable.
inline constexpr uint8_t kNoCancel = 0xFF;   ///< Choice: Back does nothing.
inline constexpr uint8_t kObjectSelf = 254;   ///< Object operand: the context's own object.
inline constexpr uint8_t kObjectPlayer = 255;   ///< Object operand: the walker.
inline constexpr uint8_t kNoSelf = 253;   ///< Context::self: no object started the script.
inline constexpr uint8_t kFacePlayer = 4;   ///< ObjectFace: towards the walker.
inline constexpr uint8_t kFaceSelf = 5;   ///< ObjectFace: towards the context's own object.
inline constexpr uint8_t kObjectCamera = 252;   ///< I4: object operand: the camera.
inline constexpr uint8_t kObjectLast = 251;   ///< I4: object operand: the object the context last gave a route.
/// C10: object operand: the selected object, the context's own object or, when the context has none, the walker
/// (a reference game's "selected object", which its field input resets to the player every frame).
inline constexpr uint8_t kObjectSelected = 250;
/// C10: ObjectFace: the direction opposite the walker's facing (whatever the geometry; a reference game's way).
inline constexpr uint8_t kFaceOppositePlayer = 6;
inline constexpr uint8_t kWarpNoWait = 0x80;   ///< I4: Warp facing bit: the script goes on at once.
inline constexpr uint8_t kWarpFacingMask = 0x7F;   ///< I4: Warp facing bits without kWarpNoWait.
inline constexpr uint8_t kLockSelf = 0;   ///< I4: Lock / Release: the context's object (and the others' movement).
inline constexpr uint8_t kLockAll = 1;   ///< I4: Lock / Release: every object.
inline constexpr uint8_t kMessageNoWait = 1;   ///< I4: MessageEx: the script goes on while it types.
inline constexpr uint8_t kMessageInstant = 2;   ///< I4: MessageEx: typed at once.
inline constexpr uint8_t kMessageAutoScroll = 4;   ///< I4: MessageEx: scrolls on without a button.
inline constexpr uint8_t kFadeSwap = 1;   ///< I4: Fade flags: keep the palettes the scene changes meanwhile.
///@}

/// I4: ObjectTemplate kinds.
enum class TemplateOp : uint8_t
{
   Pos,   ///< Its cell (x, y).
   Movement,   ///< Its movement (the game's own numbering).
   Here,   ///< Its cell from where the object stands.
   NUM_TEMPLATE_OPS   ///< Number of kinds.
};

/// I4: Door kinds.
enum class DoorOp : uint8_t
{
   Open,   ///< Animate it opening.
   Close,   ///< Animate it closing.
   SetOpen,   ///< Show it open at once.
   SetClosed,   ///< Show it closed at once.
   Wait,   ///< Wait until the animation ends.
   NUM_DOOR_OPS   ///< Number of kinds.
};

/// I4: Weather kinds.
enum class WeatherOp : uint8_t
{
   Set,   ///< The weather the next Apply shows (the game's numbering).
   Reset,   ///< Back to the map's own weather.
   Apply,   ///< Show it.
   NUM_WEATHER_OPS   ///< Number of kinds.
};

/// I4: Fade kinds.
enum class FadeOp : uint8_t
{
   ToBlack,   ///< Out to black.
   FromBlack,   ///< In from black.
   ToWhite,   ///< Out to white.
   FromWhite,   ///< In from white.
   NUM_FADE_OPS   ///< Number of kinds.
};

/// I4: the warp destinations SetWarp remembers.
enum class WarpSlot : uint8_t
{
   Next,   ///< The next warp's destination (the game's warp command takes it).
   Dynamic,   ///< The dynamic warp (a warp that leads back where the player came from).
   Dive,   ///< The dive / emerge destination.
   Hole,   ///< Where a hole leads.
   Escape,   ///< The escape warp.
   NUM_WARP_SLOTS   ///< Number of slots.
};

/// I4: Sound kinds (operands as the game numbers them).
enum class SoundOp : uint8_t
{
   Bgm,   ///< Play music (song, save: 1 to keep it as the map's).
   BgmSave,   ///< Save a song to play after the next map's.
   FadeDefault,   ///< Fade to the map's own music.
   FadeNew,   ///< Fade to a song.
   FadeOut,   ///< Fade the music out (speed).
   FadeIn,   ///< Fade it back in (speed).
   Se,   ///< A sound effect.
   Fanfare,   ///< A fanfare (the music pauses).
   Cry,   ///< A cry (species, mode).
   NUM_SOUND_OPS   ///< Number of kinds.
};

/// I4: WaitSound kinds.
enum class SoundWait : uint8_t
{
   Se,   ///< The sound effect.
   Fanfare,   ///< The fanfare.
   Cry,   ///< The cry.
   NUM_SOUND_WAITS   ///< Number of kinds.
};

/// @name Limits
///@{
inline constexpr int kMaxStack = 32;   ///< VM call stack slots (the manifest's stack_depth is at most this).
inline constexpr int kMaxNativeArgs = 8;   ///< Arguments per native call.
inline constexpr int kMaxChoices = 8;   ///< Options of one Choice.
inline constexpr int kMaxTriggers = 16;   ///< On-frame triggers armed at once (one map).
inline constexpr int kMaxTriggerDeps = 8;   ///< Flags and variables one trigger reads.
///@}

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

/// When a map trigger runs.
enum class TriggerKind : uint8_t
{
   OnTransition,   ///< Once when the map is entered, before on_load (must not wait).
   OnLoad,   ///< Once while the map loads (must not wait).
   OnFrame,   ///< While its condition holds; re-checked only after a flag or variable it reads changes.
   OnResume,   ///< I4: once when the field resumes on the map (after on_load; must not wait).
   OnWarpInto,   ///< I4: after a warp's arrival, the first entry whose condition holds (must not wait).
   OnDive,   ///< I4: when a dive or emerge leads to the map (must not wait).
   OnReturn,   ///< I4: when the field comes back from a menu or a battle (must not wait).
   NUM_TRIGGER_KINDS   ///< Number of kinds.
};

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

/// FieldEffect op kinds.
enum class FieldEffectOp : uint8_t
{
   Start,       ///< Start it (amount operand); var = 1 started, 0 refused.
   Active,      ///< var = 1 while it runs.
   Remaining,   ///< var = what it has left.
   NUM_FIELD_EFFECT_OPS   ///< Number of kinds.
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

/// Flight op kinds.
enum class FlightOp : uint8_t
{
   Record,   ///< Record a point (its flag).
   Visit,    ///< Warp to a point (transition operand).
   Menu,     ///< The menu of recorded points: warp to the one picked; var = 1 warped, 0 backed out.
   NUM_FLIGHT_OPS   ///< Number of kinds.
};

/// FieldAction op kinds.
enum class FieldActionOp : uint8_t
{
   Available,   ///< var = the first member it is available to, plus 1; 0 when none (or no party).
   NUM_FIELD_ACTION_OPS   ///< Number of kinds.
};

/// Combine op kinds (B1; tools/script_compiler.py COMBINE_KINDS).
enum class CombineOp : uint8_t
{
   Open,      ///< The menu flow: pick the inputs, preview, ask, combine (waits).
   Run,       ///< Combine the given party members (waits: the move picks).
   Preview,   ///< var = the result species + 1 (0: it does not combine; kCombineRandom: a random pick).
   Status,    ///< var = what a combination would say (capsule::combine::Status).
   Fee,       ///< var = its fee (0 when it does not combine or the result is a random pick).
   NUM_COMBINE_OPS   ///< Number of kinds.
};

/// Preview's value of a random pick (its species is drawn when it combines).
inline constexpr int32_t kCombineRandom = 0xFFFF;
/// Most party members a Combine op names (any platform's capsule::combine::kMaxInputs).
inline constexpr int kMaxCombineMembers = 8;

/// Nursery op kinds (B2; tools/script_compiler.py NURSERY_KINDS).
enum class NurseryOp : uint8_t
{
   Pick,       ///< The party menu (waits): var = the member + 1, 0 backed out.
   Deposit,    ///< Leave a party member (operand): var = 1 when it went.
   Withdraw,   ///< Take a slot's record back (operand) paying its fee from a money variable: var = 1 done, 0 refused,
               ///< kNurseryPoor not enough money.
   Count,      ///< var = the records held.
   Levels,     ///< var = the levels a slot's record gained (operand).
   Fee,        ///< var = what taking a slot's record back costs (operand).
   Ready,      ///< var = 1 when a result waits.
   Odds,       ///< var = the method's odds over the held records.
   Take,       ///< The waiting result joins the party: var = 1 when it did.
   Name,       ///< The nickname prompt for a party member (operand; waits): var = 1 when named.
   NUM_NURSERY_OPS   ///< Number of kinds.
};

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
