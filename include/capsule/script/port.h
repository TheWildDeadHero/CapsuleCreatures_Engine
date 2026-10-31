/// @file
/// What the script VM needs from the rest of the game (B-1, P-13).
///
/// - namespace port: presentation and input for messages and menus. The
///   engine's port (src/script/script_port_hal.cpp) uses the text box
///   (capsule/text/text_layer.h) and the pad; tests link their own.
/// - namespace party: the player's party records for role_get / role_set and
///   record placeholders (D-22 decision 2). The engine's port reads and
///   writes the party given to party::attach(); tests link their own.
/// - namespace items (G2): the bag the bag statements reach, the bag and shop
///   menus and the learn question of a move-teaching item; the engine's
///   defaults (src/script/script_items.cpp) use the attached bag and the
///   text box's choice menu; tests link their own.
/// - namespace boxes (G3): the storage boxes and party the box statements move
///   records between, and the boxes menu; the engine's defaults
///   (src/script/script_boxes.cpp) use the attached storage and party.
/// - namespace game: natives, the game's own actions (give an item, start a
///   battle, play a cutscene, ...). The engine's default
///   (src/script/script_game_default.cpp) knows none; a game builds its own
///   file defining game::native() instead of that one.
/// - I4 (D-29): what imported event scripts need beyond those: message modes,
///   object locks, removal, visibility and templates, doors, weather, fades,
///   metatiles, the respawn point, remembered warps (world), sound (audio),
///   marts (items) and the game's busy state (game). Their engine defaults
///   (src/script/script_i4_default.cpp) are weak (CAPSULE_WEAK): the field
///   ones act through capsule::field, the rest do nothing at once; a game that
///   has them defines its own.
#pragma once

#include "capsule/core/types.h"
#include "capsule/hal/video.h"
#include "capsule/inventory/bag.h"
#include "capsule/monster/monster.h"
#include "capsule/script/data.h"
#include "capsule/storage/storage.h"
#include "capsule/text/encode.h"

#include <cstdint>

/// Presentation for the script VM.
namespace capsule::script::port
{

/// Shows text in the message box (opening it if closed), typed.
/// @param codes  encoded text ending with kCodeEnd
void message(const text::Code* codes);
/// Whether the message is still typing, or waits at a prompt or page break
/// (capsule/text/encode.h FA, FB). The VM calls it once per frame while it
/// waits for the message; a port goes on past a prompt or page break when
/// Accept or Back is pressed (the engine's port: text::layer::resume()).
/// @return true while typing or waiting there
bool message_busy();
/// Closes the message box.
void close_message();
/// Whether Accept or Back was pressed this frame (and consumes the press).
/// @return true once per press
bool button();
/// Opens a yes / no question under the current message.
void yesno_open();
/// The yes / no answer.
/// @return -1 while waiting, 0 = no (or Back), 1 = yes
int8_t yesno_poll();
/// Opens a menu.
/// @param options  @p count encoded option texts
/// @param count    1 to kMaxChoices
/// @param cancel   index Back selects, or kNoCancel
void choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel);
/// The menu answer.
/// @return -1 while waiting, else the chosen index
int16_t choice_poll();
/// Sets where the box shows from the next message on.
/// @param pos  0 bottom, 1 middle, 2 top
void message_position(uint8_t pos);
/// Opens a number entry (Up / Down change the digit, Left / Right pick it, A answers, B answers 0).
/// @param digits  1-9
void number_open(uint8_t digits);
/// The number entry's answer.
/// @return -1 while waiting, else the number
int32_t number_poll();
/// A key pressed since the last tick().
/// @return -1 while none was, else 1 A, 2 B, 3 Select, 4 Start, 5 Right, 6 Left, 7 Up, 8 Down, 9 R, 10 L
int8_t key_poll();
/// Advances the port one frame (input edges, cursor); called by
/// script::frame() once per frame, before any context runs.
void tick();

/// I4: how the next message() types (MessageEx; the default: kMessage... bits clear, typed and waited for). The VM
/// calls it before each message() of a MessageEx and with 0 after it.
/// @param flags  kMessageInstant, kMessageAutoScroll (kMessageNoWait is the VM's)
void message_mode(uint8_t flags);

} // namespace capsule::script::port

/// The overworld for the object commands (B-21); the field module
/// (src/field/field.cpp) implements it, tests link their own.
namespace capsule::script::world
{

/// Starts a walk through anything (the reference games' scripted walk).
/// @param obj    local ID, or kObjectPlayer
/// @param dir    direction 0-3 (down, up, left, right)
/// @param steps  cells (0: just turns)
/// @return false when there is no such (shown) object
bool move(uint8_t obj, uint8_t dir, uint16_t steps);

/// Whether a walk would take every step by the field's movement rules (C8: the `move ... checked` statement;
/// nothing moves): the walker's terrain, elevation, vehicle class (one that dismounts may step onto land) and the
/// objects in the way, or an object's (field::object_path_clear()).
/// @param obj    local ID, or kObjectPlayer
/// @param dir    direction 0-3 (down, up, left, right)
/// @param steps  cells
/// @return false when a step is blocked, or there is no such (shown) object
bool path_clear(uint8_t obj, uint8_t dir, uint16_t steps);

/// Whether a walk is still under way.
/// @param obj  local ID, or kObjectPlayer
/// @return true while it walks
bool moving(uint8_t obj);

/// Turns an object or the walker.
/// @param obj  local ID, or kObjectPlayer
/// @param dir  direction 0-3
void face(uint8_t obj, uint8_t dir);

/// Turns an object or the walker towards another.
/// @param obj    local ID, or kObjectPlayer
/// @param other  local ID, or kObjectPlayer
void face_toward(uint8_t obj, uint8_t other);

/// Shows or hides a map object.
/// @param obj  local ID
/// @param on   show it
void show(uint8_t obj, bool on);

/// Starts a move route (capsule/script/route.h), replacing one in progress.
/// @param obj   local ID, or kObjectPlayer
/// @param code  the route (flags byte, steps, End); stays valid (ROM)
/// @param len   its bytes
/// @param from_script  started by a script (wait_moves waits for it unless it repeats)
/// @return false when there is no such (shown) object
bool route(uint8_t obj, const uint8_t* code, uint16_t len, bool from_script);

/// Whether a route started by a script (not repeating) still runs.
/// @return true while one does
bool routes_busy();

/// A self switch of an object of the shown map.
/// @param obj     local ID
/// @param letter  0-3 (A-D)
/// @return its state (false for an object without self switches)
bool self_switch(uint8_t obj, uint8_t letter);

/// Sets a self switch (asserts for an object the compiler gave none).
/// @param obj     local ID
/// @param letter  0-3
/// @param on      set
void set_self_switch(uint8_t obj, uint8_t letter, bool on);

/// A game value.
/// @param q    what
/// @param obj  local ID or kObjectPlayer, for the per-object queries
/// @return its value
int32_t query(Query q, uint8_t obj);

/// Moves the player to a map (the same one too) with a transition.
/// @param map         the map's asset key
/// @param x           cell column
/// @param y           cell row
/// @param facing      direction, or kKeepFacing
/// @param transition  Transition, or kTransitionDefault
void warp(AssetKey map, int16_t x, int16_t y, uint8_t facing, uint8_t transition);

/// Whether a warp is still under way.
/// @return true until the new map is shown
bool warping();

/// Puts an object or the player on a cell at once.
/// @param obj     local ID or kObjectPlayer
/// @param x       column
/// @param y       row
/// @param facing  direction, or kKeepFacing
void place(uint8_t obj, int16_t x, int16_t y, uint8_t facing);

/// Pans the camera (dir 0-3), or back to the player (dir 4).
/// @param dir    direction or 4
/// @param cells  cells
/// @param speed  1-4 (as route speeds)
void scroll(uint8_t dir, int16_t cells, uint8_t speed);

/// Whether the camera is still panning.
/// @return true while it is
bool scrolling();

/// Tints the screen towards a color.
/// @param color     the color
/// @param strength  0 (off) to 16 (the color)
/// @param frames    frames to get there (0: at once)
void tint(hal::video::Color color, uint8_t strength, uint16_t frames);

/// Flashes the screen with a color, fading back over frames.
/// @param color   the color
/// @param frames  1-65535
void flash(hal::video::Color color, uint16_t frames);

/// Shakes the screen.
/// @param power   pixels
/// @param frames  frames
void shake(uint8_t power, uint16_t frames);

/// Starts the countdown timer (shown on screen).
/// @param seconds  1-5999
void timer_start(int32_t seconds);

/// Stops the countdown timer.
void timer_stop();

/// Turns encounters on or off.
/// @param on  on
void encounters(bool on);

/// Changes the walker's vehicle class (A2; implemented by capsule::field).
/// @param vehicle  vehicle class (the compiler checks it against the world)
void set_vehicle(uint8_t vehicle);

/// Sets the shown map's darkness level until the next map (F2; implemented by capsule::overworld).
/// @param level  0 (lit) to map::kMaxDarkness
void set_darkness(uint8_t level);

/// Warps to the recorded escape point (F2; capsule/script/field_actions.h escape_point(); implemented by
/// capsule::overworld), as `warp` does.
/// @param transition  Transition, or kTransitionDefault
/// @return false when none is recorded (nothing happens)
bool escape(uint8_t transition);

/// @name I4 (D-29)
///@{

/// Stops the objects' own movement (lock / lock all; the default: field::lock()).
/// @param self  the context's object (kNoSelf: none)
/// @param all   every object (lockall), else all but the walker's partner in talk
void lock(uint8_t self, bool all);

/// Lets the objects move on their own again (the default: field::release()).
/// @param self  the context's object
/// @param all   every object
void release(uint8_t self, bool all);

/// Removes a map object from the shown map until its hide flag clears (the default: hides it and sets its
/// template's hide flag, so it stays away).
/// @param obj  local ID
void remove(uint8_t obj);

/// Shows or hides an object's sprite where it stands; it still blocks (the default: field::set_object_visible()).
/// @param obj  local ID, or kObjectPlayer
/// @param on   shown
void set_visible(uint8_t obj, bool on);

/// Changes an object's template on the shown map until the next map (the default: field::set_template_cell() for
/// Pos and Here; Movement does nothing).
/// @param obj   local ID
/// @param kind  TemplateOp
/// @param a     Pos: column; Movement: the game's movement number
/// @param b     Pos: row
void set_template(uint8_t obj, TemplateOp kind, int32_t a, int32_t b);

/// A door's animation on a cell (DS1: the default plays the door bound to the cell, capsule/field/doors.h).
/// @param kind  DoorOp (not Wait)
/// @param x     column
/// @param y     row
void door(DoorOp kind, int16_t x, int16_t y);

/// Whether a door animation runs.
/// @return true while one does (the default: field::doors::busy())
bool door_busy();

/// DS1: applies or clears a cell override set (D-30 section 2; the default: map::set_override_set() on the shown
/// map, the set's flag or variable written when it persists).
/// @param set    index into map::kWorld.override_sets
/// @param apply  apply it (else clear it)
void override_set(uint16_t set, bool apply);

/// The weather (the default: nothing).
/// @param kind   WeatherOp
/// @param value  Set: the game's weather number
void weather(WeatherOp kind, int32_t value);

/// Fades the screen (the default: nothing).
/// @param kind   FadeOp
/// @param speed  frames per step (0: the default)
/// @param flags  kFadeSwap
void fade(FadeOp kind, uint8_t speed, uint8_t flags);

/// Whether a fade runs.
/// @return true while one does (the default: false)
bool fading();

/// Changes a cell of the shown map until the next map (the default: nothing).
/// @param x       column
/// @param y       row
/// @param tile    the game's metatile number
/// @param solid   collision on
void set_metatile(int16_t x, int16_t y, int32_t tile, bool solid);

/// Sets the respawn point (the default: nothing).
/// @param value  the game's numbering
void set_respawn(int32_t value);

/// Remembers a warp destination (the default: nothing).
/// @param slot  WarpSlot
/// @param map   the map's asset key
/// @param x     column (or the game's warp number)
/// @param y     row
void set_warp(WarpSlot slot, AssetKey map, int32_t x, int32_t y);
///@}

} // namespace capsule::script::world

/// I4: sound for the script VM (the default: silence; src/script/script_i4_default.cpp).
namespace capsule::script::audio
{

/// Plays music, a sound effect, a fanfare or a cry.
/// @param kind  SoundOp
/// @param args  the operands (the game's numbering)
/// @param argc  how many
void play(SoundOp kind, const int32_t* args, uint8_t argc);

/// Whether a sound still plays.
/// @param kind  SoundWait
/// @return true while it does (the default: false)
bool playing(SoundWait kind);

} // namespace capsule::script::audio

/// The player's party records (RoleGet, RoleSet, record placeholders).
namespace capsule::script::party
{

/// Reads an element of a party record's role.
/// @param slot     party slot
/// @param role     the role (monster::RoleId: an engine role or a custom role of the layout)
/// @param element  element index
/// @return its value (values above INT32_MAX read as INT32_MAX); 0 for an empty slot or a role the layout
///         does not hold
int32_t role_get(uint8_t slot, uint8_t role, uint8_t element);

/// Writes an element of a party record's role (truncated to the role's width; negative values write 0).
/// Does nothing for an empty slot or a role the layout does not hold.
/// @param slot     party slot
/// @param role     the role
/// @param element  element index
/// @param value    new value
void role_set(uint8_t slot, uint8_t role, uint8_t element, int32_t value);

/// Gives the engine's port (src/script/script_port_hal.cpp) the party role_get() and role_set() reach
/// (a game calls it once its party exists; ports linked by tests ignore it).
/// @param records  the party, or nullptr (every role reads 0)
void attach(monster::Party* records);

} // namespace capsule::script::party

/// The bag, shops and move teaching from scripts (G2, D-26 section 2). The engine's defaults
/// (src/script/script_items.cpp) work on the bag given to attach() and show the menus in the text box through the
/// port's choice menu; a game replaces any of them by defining it (link time). N1: give(), take() and count() are
/// weak, so a game with its own bag rules defines those three and keeps the default menus.
namespace capsule::script::items
{

/// Gives the defaults the bag scripts give to and take from (a game calls it once its bag exists).
/// @param bag  the bag, or nullptr (every give and take fails, every count is 0)
void attach(inventory::Bag* bag);

/// The attached bag.
/// @return it, or nullptr
inventory::Bag* bag();

/// Puts items into the bag (ItemGive).
/// @param item  item id
/// @param n     how many
/// @return true if the bag took all of them
bool give(uint16_t item, int32_t n);

/// Takes items from the bag (ItemTake).
/// @param item  item id
/// @param n     how many
/// @return true if the bag held that many (they are gone)
bool take(uint16_t item, int32_t n);

/// How many of an item the bag holds (ItemCount).
/// @param item  item id
/// @return the count
int32_t count(uint16_t item);

/// Opens the bag menu (BagOpen).
void bag_open();

/// Polls the bag menu.
/// @return -1 while it is open, else the chosen item (0: none)
int32_t bag_poll();

/// Opens a shop list (Shop): the shop's items at their prices, paid from a script variable.
/// @param shop   shop index (battle_ids.h SHOP_...)
/// @param money  the variable holding the money
void shop_open(uint8_t shop, VarId money);

/// Polls the shop list.
/// @return true while it is open
bool shop_poll();

/// Asks which move a party member forgets for a move a used item teaches (a field use's learn offer,
/// capsule/inventory/use.h).
/// @param member  party slot
/// @param move    the offered move
void learn_open(uint8_t member, uint16_t move);

/// Polls the learn question.
/// @return -1 while it is open, else the move slot to forget or battle::kKeepMoves (0xFF) to keep every move
int16_t learn_poll();

/// I4: opens a mart list (Mart; the default: nothing, it closes at once).
/// @param kind   the game's mart kind (0: items)
/// @param items  @p count little-endian 16-bit item numbers (in the bytecode: unaligned)
/// @param count  items
void mart_open(uint8_t kind, const uint8_t* items, uint16_t count);

/// I4: polls the mart list.
/// @return true while it is open
bool mart_poll();

} // namespace capsule::script::items

/// The storage boxes for the script VM's box statements (G3, D-26 section 3); the engine's defaults
/// (src/script/script_boxes.cpp) move records between the attached party and storage and show the boxes in the
/// text box's choice menu; tests link their own.
namespace capsule::script::boxes
{

/// Gives the defaults the storage and the party the box statements move records between (a game calls it once
/// they exist).
/// @param st     the storage boxes, or nullptr (every move fails, every count is 0)
/// @param party  the party, or nullptr
void attach(storage::Storage* st, monster::Party* party);

/// The attached storage.
/// @return it, or nullptr
storage::Storage* attached();

/// Records the boxes hold (BoxCount).
/// @return the count
int32_t count();

/// Moves a party member into the first free slot (BoxDeposit; the party keeps at least one member).
/// @param member  party member index
/// @return true if it went
bool deposit(int32_t member);

/// Moves a boxed record to the end of the party (BoxWithdraw).
/// @param slot  storage slot
/// @return true if it came (a free party slot and a used storage slot)
bool withdraw(int32_t slot);

/// Opens the boxes menu (BoxesOpen).
void open();

/// Polls the boxes menu.
/// @return -1 while it is open, else the chosen box + 1 (0: none)
int32_t poll();

} // namespace capsule::script::boxes

/// The game's natives.
namespace capsule::script::game
{

/// Result of a native call.
enum class NativeStatus : uint8_t
{
   Done,              ///< Finished; the result is used.
   Wait,              ///< Not finished: called again next frame with the same arguments.
   NUM_NATIVE_STATUS  ///< Number of results.
};

/// Runs a native (IDs from script_ids.h, NATIVE_...).
/// @param id      the native
/// @param args    @p argc arguments, already evaluated
/// @param argc    0 to kMaxNativeArgs
/// @param result  result value (written when Done)
/// @return Done or Wait
NativeStatus native(uint16_t id, const int32_t* args, uint8_t argc, int32_t& result);

/// I4: whether the game is busy with something a native started over frames (WaitState; the default: false).
/// @return true while it is
bool state_busy();

} // namespace capsule::script::game
