/// @file
/// Script VM, flags, variables and map triggers (see capsule/script/script.h).
#include "capsule/script/script.h"

#include "capsule/asset/asset.h"
#include "capsule/core/assert.h"
#include "capsule/core/clock.h"
#include "capsule/core/endian.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/core/saturate.h"
#include "capsule/map/map.h"
#include "capsule/script/battle_port.h"
#include "capsule/script/combine_port.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/growth_port.h"
#include "capsule/script/nursery_port.h"
#include "capsule/script/port.h"
#include "script_dispatch.h"

#include <cstring>
#include <iterator>

namespace capsule::script
{

using detail::Outcome;

namespace
{

/// @name Bytecode layout
///@{
constexpr int kU16Bytes = 2;            ///< A 16-bit operand (IDs, targets, lengths).
constexpr int kU32Bytes = 4;            ///< A 32-bit operand (constants, asset keys, text offsets).
constexpr int kQueryBytes = 2;          ///< A query operand: query byte and object byte.
constexpr int kFlagBranchBytes = 4;     ///< IfFlagSet / IfFlagClear: flag ID and target.
constexpr int kChoiceHeaderBytes = 4;   ///< Choice: result variable, cancel index, option count.
constexpr int kMenuPlaceBytes = 2;      ///< C3: Menu: x and y.
constexpr int kSelfSwitchBytes = 2;     ///< SelfSwitch: letter and on.
constexpr int kWarpTailBytes = 2;       ///< Warp: facing and transition, after the position.
constexpr int kFadeKindAt = 0;          ///< Fade: offset of its kind.
constexpr int kFadeSpeedAt = 1;         ///< Fade: offset of its speed.
constexpr int kFadeFlagsAt = 2;         ///< Fade: offset of its flags.
constexpr int kFadeBytes = 3;           ///< Fade: kind, speed and flags.
constexpr int kOverrideBytes = 3;       ///< Override: apply and the set (16 bits).
constexpr int kSoundHeaderBytes = 2;    ///< Sound: kind and operand count.
constexpr int kMaxSoundArgs = 2;        ///< most operands of a Sound (a song and its save, a cry and its mode).
constexpr int kColorBytes = 3;          ///< Tint / Flash: red, green, blue (5 bits each).
constexpr int kRed = 0;                 ///< Offset of red in a color operand.
constexpr int kGreen = 1;               ///< Offset of green.
constexpr int kBlue = 2;                ///< Offset of blue.
constexpr int kNameLengthBytes = 1;     ///< Bytes before a name slot's codes (its length).
constexpr int kDecimalBase = 10;        ///< Variable placeholders print in decimal.
constexpr int kMaxDecimalDigits = 10;   ///< Digits of the largest variable value (2^31 - 1).
constexpr uint8_t kNoByteIndex = 0xFF;  ///< A slot or element operand out of range (no record has it).
///@}

/// @name Operand ranges
///@{
constexpr int kVarWidth16 = 2;                ///< VarInfo::width of a 16-bit variable.
constexpr int kBitsPerByte = 8;               ///< Bits of a variable's storage byte (its type's range).
constexpr uint8_t kDirectionMask = 3;         ///< Direction bits of ObjectMove's direction byte.
/// A direction's opposite differs in its lowest bit (0 down / 1 up, 2 left / 3 right).
constexpr uint8_t kOppositeDirectionBit = 1;
/// Draws of `random` in the modulo form are 16-bit (the generator's next16()).
constexpr uint32_t kRandomDrawMask = 0xFFFF;
constexpr uint8_t kSelfSwitchMask = 3;        ///< Self switch letter bits (A-D).
constexpr int32_t kMaxSteps = 0xFFFF;         ///< Most steps of one ObjectMove.
constexpr int32_t kMaxScrollCells = 255;      ///< Most cells of one Scroll.
constexpr int32_t kMaxItemId = 0xFFFF;        ///< Largest item id an item operand names (16-bit ids).
constexpr int32_t kMaxMoveId = 0xFFFF;        ///< C7: largest move index a move operand names (the moves role: 16 bits).
constexpr int32_t kMaxSpeciesId = 0xFFFF;     ///< C8: largest species a species operand names (16-bit ids).
constexpr int32_t kMaxLevel = 0xFF;           ///< C8: largest level a level operand names (8 bits).
constexpr int32_t kMaxAnimationArg = 0xFF;    ///< C9: largest animation argument index (8 bits).
constexpr int32_t kMaxAnimationId = 0xFFFF;   ///< C9: largest animation id a wait keeps (result_var: 16 bits).
constexpr int32_t kMaxTimerSeconds = 5999;    ///< Longest timer, 99:59.
///@}

/// Instructions run_to_end() executes at most (a guard against endless loops).
constexpr int kMaxRunSteps = 0x10000;
/// Trigger kinds that run once when a map loads (on_transition, on_load): those before OnFrame.
constexpr int kRunOnceKinds = static_cast<int>(TriggerKind::OnFrame);
/// Shift from a flag's bit index to its byte (bit / 8).
constexpr int kByteOfBitShift = 3;
/// A flag's bit inside its byte (bit & 7).
constexpr int kBitInByteMask = 7;

/// The bank in use.
/// @return kBank
const Bank& bank()
{
   return kBank;
}

/// @name Flags and variables
///@{

/// Bank of a flag ID.
/// @param id  flag ID
/// @return its lifetime
Lifetime bank_of_flag(uint16_t id)
{
   const Bank& b = bank();
   ENGINE_ASSERT(id < b.flag_first[kNumLifetimes]);
   int l = kNumLifetimes - 1;
   while (l > 0 && id < b.flag_first[l])
   {
      --l;
   }
   return static_cast<Lifetime>(l);
}

/// A value wrapped into a range (kVmVarsWrap): its distance from the range's start modulo the range's span, so a
/// 16-bit variable (0-65535) overflows to 0 and below 0 to 65535 as 16-bit arithmetic does.
/// @param value  the value (wide: a sum may leave 32 bits)
/// @param lo     the range's lowest value
/// @param hi     its highest value
/// @return the wrapped value
int32_t wrap_to(int64_t value, int32_t lo, int32_t hi)
{
   const int64_t span = static_cast<int64_t>(hi) - lo + 1;
   int64_t offset = (value - lo) % span;
   if (offset < 0)
   {
      offset += span;
   }
   return static_cast<int32_t>(lo + offset);
}

/// Whether a variable wraps (kVmVarsWrap): with the bank's switch, a variable declared with its type's whole
/// range (u8 0-255, u16 0-65535) overflows as that integer type does; one with a narrower range still saturates.
/// @param v  its info
/// @return true when it wraps
bool wraps(const VarInfo& v)
{
   if ((bank().vm_flags & kVmVarsWrap) == 0 || v.min != 0 || v.width > kVarWidth16)
   {
      return false;
   }
   return v.max == static_cast<int32_t>((1u << (v.width * kBitsPerByte)) - 1u);
}

/// The variable record.
/// @param id  variable ID
/// @return its info
const VarInfo& info(uint16_t id)
{
   ENGINE_ASSERT(id < bank().var_count);
   return bank().vars[id];
}

/// Reads a variable's stored value.
/// @param v  its info
/// @return the value
int32_t load(const VarInfo& v)
{
   const uint8_t* p = bank().var_storage[static_cast<int>(v.lifetime)] + v.offset;
   switch (v.width)
   {
   case 1:
      return *p;
   case kVarWidth16:
      return load_le16(p);
   default:
      return load_le32s(p);
   }
}

/// Writes a variable's stored value (already in range).
/// @param v      its info
/// @param value  the value
void store(const VarInfo& v, int32_t value)
{
   uint8_t* p = bank().var_storage[static_cast<int>(v.lifetime)] + v.offset;
   switch (v.width)
   {
   case 1:
      *p = static_cast<uint8_t>(value);
      break;
   case kVarWidth16:
      store_le16(p, static_cast<uint16_t>(value));
      break;
   default:
      store_le32(p, static_cast<uint32_t>(value));
      break;
   }
}

///@}

/// @name Map triggers
///@{

/// The armed on_frame triggers.
struct Armed
{
   const MapTrigger* trigger[kMaxTriggers];   ///< Triggers of the current map.
   uint16_t dirty;                            ///< Bit n: trigger n must be re-checked.
   uint8_t count;                             ///< Armed triggers.
};
Armed g_armed{};   ///< Armed triggers.
uint32_t g_state = 0;   ///< state_version().

/// Marks the armed triggers that read a flag or variable as changed.
/// @param dep  flag ID, or kDepVar | variable ID
void touch(uint16_t dep)
{
   ++g_state;
   for (uint8_t i = 0; i < g_armed.count; ++i)
   {
      const MapTrigger& t = *g_armed.trigger[i];
      for (uint8_t d = 0; d < t.dep_count; ++d)
      {
         if (bank().deps[t.dep_first + d] == dep)
         {
            g_armed.dirty = static_cast<uint16_t>(g_armed.dirty | (1u << i));
            break;
         }
      }
   }
}

///@}

/// @name Decoding
///@{

/// Bytecode of a script.
/// @param s  script ID
/// @return its first byte
const uint8_t* code_of(uint16_t s)
{
   ENGINE_ASSERT(s < bank().script_count);
   return bank().code + bank().scripts[s].code;
}

uint8_t g_self = kNoSelf;   ///< Self of the context executing (query operands name it).
/// update() re-runs a native that waited (its next instruction may wait for the next frame).
bool g_native_rerun = false;
/// The statement after the native being run (native_return_point()).
Frame g_native_after{};
/// Where the frame goes on after the native being run (native_goto()), when g_native_goto is set.
Frame g_native_to{};
/// The native being run asked to go on at g_native_to.
bool g_native_goto = false;

/// `random(n)` in the modulo form (kVmRandomModulo): a 16-bit draw modulo n as a 16-bit value, drawn even for
/// n = 0 (then 0: a software modulo by zero returns 0, as the reference game's division routine does).
/// @param n  the bound operand (taken as 16 bits, as the game's variables are)
/// @return the draw modulo n
int32_t random_modulo(int32_t n)
{
   const uint32_t draw = rng::next16(rng::gameplay);
   const uint32_t bound = static_cast<uint32_t>(n) & kRandomDrawMask;
   return bound == 0 ? 0 : static_cast<int32_t>(draw % bound);
}

/// A numbered object operand as a local ID (the bank's numbering may count from object_base).
/// @param v  the operand: below kObjectLast a local ID in the bank's numbering; else a special object
/// @return a local ID, the special object, or kNoSelf (below object_base)
uint8_t local_of(int32_t v)
{
   if (v >= kObjectSelected && v <= kObjectPlayer)
   {
      return static_cast<uint8_t>(v);
   }
   const int32_t local = v - bank().object_base;
   return local >= 0 && local < kObjectSelected ? static_cast<uint8_t>(local) : kNoSelf;
}

/// A party slot or element index from an operand.
/// @param v  the operand's value
/// @return it as a byte, or kNoByteIndex when it is out of 0-254 (no slot or element is there)
uint8_t byte_index(int32_t v)
{
   return v >= 0 && v < kNoByteIndex ? static_cast<uint8_t>(v) : kNoByteIndex;
}

/// Reads one operand and advances past it.
/// @param p  read position; advanced
/// @return the operand's value
int32_t operand(const uint8_t*& p)
{
   const uint8_t tag = *p++;
   int32_t v = 0;
   switch (tag)
   {
   case kOpConst8:
      v = *p;
      p += 1;
      break;
   case kOpConst16:
      v = load_le16s(p);
      p += kU16Bytes;
      break;
   case kOpConst32:
      v = load_le32s(p);
      p += kU32Bytes;
      break;
   case kOpVar:
      v = var(VarId{load_le16(p)});
      p += kU16Bytes;
      break;
   case kOpQuery:
   {
      const uint8_t obj = p[1] == kObjectSelf ? g_self : local_of(p[1]);
      v = (obj == kNoSelf && p[0] <= static_cast<uint8_t>(Query::Facing)) ? 0
                                                                          : world::query(static_cast<Query>(p[0]), obj);
      p += kQueryBytes;
      break;
   }
   default:
      ENGINE_ASSERT(false && "bad operand tag");
      break;
   }
   return v;
}

// C4: decimal() is defined (and documented) with the placeholders below
int decimal(int32_t v, text::Code* out, int capacity);

/// C4: copies a text's codes (no kCodeEnd).
/// @param t         the text, or nullptr (nothing)
/// @param out       where they go
/// @param capacity  most codes
/// @return codes written
int copy_codes(const text::Code* t, text::Code* out, int capacity)
{
   int n = 0;
   for (; t != nullptr && t[n] != text::kCodeEnd && n < capacity; ++n)
   {
      out[n] = t[n];
   }
   return n;
}

/// Reads one text reference and advances past it.
/// @param p  read position; advanced
/// @return the text (a "?" stand-in when the asset is missing)
const text::Code* text_ref(const uint8_t*& p)
{
   static constexpr text::Code kMissing[2] = {'?', text::kCodeEnd};
   const uint8_t tag = *p++;
   const uint32_t v = load_le32(p);
   p += kU32Bytes;
   if (tag == kTextInline)
   {
      return bank().text + v;
   }
   const text::Code* t = asset::text(AssetKey{v});
   ENGINE_ASSERT(t != nullptr);
   return t != nullptr ? t : kMissing;
}

/// Whether a comparison holds.
/// @param c  the comparison
/// @param a  left
/// @param b  right
/// @return the result
bool compare(Compare c, int32_t a, int32_t b)
{
   switch (c)
   {
   case Compare::Eq:
      return a == b;
   case Compare::Ne:
      return a != b;
   case Compare::Lt:
      return a < b;
   case Compare::Le:
      return a <= b;
   case Compare::Gt:
      return a > b;
   case Compare::Ge:
      return a >= b;
   case Compare::NUM_COMPARES:
      break;
   }
   return false;
}

///@}

/// Resolves an object operand.
/// @param c  the context
/// @param v  the operand: a local ID (in the bank's numbering), kObjectSelf, kObjectPlayer, kObjectCamera or
///           kObjectLast
/// @return a local ID, kObjectPlayer or kObjectCamera, or kNoSelf (no such object)
uint8_t object_of(const Context& c, int32_t v)
{
   if (v == kObjectSelf)
   {
      return c.self;
   }
   if (v == kObjectPlayer || v == kObjectCamera)
   {
      return static_cast<uint8_t>(v);
   }
   if (v == kObjectLast)
   {
      return c.last_object;   // the object the context last gave a route
   }
   if (v == kObjectSelected)
   {
      return c.self != kNoSelf ? c.self : kObjectPlayer;   // the selected object (the walker without one)
   }
   if (v == kObjectEncounter)
   {
      return detail::encounter_object();   // C1: the encounter record's current side's object
   }
   return local_of(v);
}

/// Moves a frame's pc past the instruction read up to p (most instructions continue after themselves).
/// @param f     the frame
/// @param p     the position after the instruction
/// @param base  the running script's first byte
void advance(Frame& f, const uint8_t* p, const uint8_t* base)
{
   f.pc = static_cast<uint16_t>(p - base);
}

/// Runs a module op through the game's dispatch table (D-36 package 2; script_dispatch.h): an op the game's scripts
/// never use has no handler there, so its module is not linked.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome run_module(Context& c, Op op, const uint8_t* p, const uint8_t* base)
{
   const detail::OpHandler run = detail::kModuleOps[static_cast<uint8_t>(op)];
   if (run == nullptr)
   {
      ENGINE_ASSERT(false && "a module op the game's dispatch table leaves out");
      c.depth = 0;
      return Outcome::Ended;
   }
   return run(c, op, p, base);
}

/// Polls a module wait through the game's dispatch table.
/// @param c  the context
/// @return true while it still waits
bool poll_module(Context& c)
{
   const detail::WaitPoll poll = detail::kModulePolls[static_cast<uint8_t>(c.wait)];
   ENGINE_ASSERT(poll != nullptr);   // only an op of the table sets its wait
   return poll != nullptr && poll(c);
}

/// Calls a script from a context.
/// @param c  the context
/// @param s  script ID
void push(Context& c, uint16_t s)
{
   ENGINE_ASSERT(s < bank().script_count);
   ENGINE_ASSERT(c.depth < bank().stack_depth && c.depth < kMaxStack);   // the compiler proves this (B-1)
   c.stack[c.depth++] = Frame{s, 0};
}

/// A 16-bit cell operand (clamped).
/// @param p  read position; advanced
/// @return the value
int16_t cell_operand(const uint8_t*& p)
{
   return static_cast<int16_t>(clamp_to<int32_t>(operand(p), INT16_MIN, INT16_MAX));
}

/// Executes one script port instruction (what imported event scripts need).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome execute_port(Context& c, Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   auto next = [&]()
   {
      f.pc = static_cast<uint16_t>(p - base);
   };
   auto wait = [&](Wait w)
   {
      next();
      c.wait = w;
      return Outcome::Waiting;
   };
   switch (op)
   {
   case Op::Lock:
   case Op::Release:
   {
      const bool all = *p++ == kLockAll;
      next();
      if (op == Op::Lock)
      {
         world::lock(c.self, all);
      }
      else
      {
         world::release(c.self, all);
      }
      return Outcome::Continue;
   }
   case Op::MessageEx:
   {
      const uint8_t flags = *p++;
      const text::Code* codes = text_ref(p);
      port::message_mode(flags);
      port::message(codes);
      port::message_mode(0);
      if ((flags & kMessageNoWait) != 0)
      {
         next();
         return Outcome::Continue;
      }
      return wait(Wait::Typing);
   }
   case Op::WaitMessage:
      return wait(Wait::Typing);
   case Op::WaitObject:
   {
      const uint8_t obj = object_of(c, operand(p));
      if (obj == kNoSelf)
      {
         next();
         return Outcome::Continue;
      }
      c.result_var = obj;
      return wait(Wait::Object);
   }
   case Op::ObjectRemove:
   {
      const uint8_t obj = object_of(c, operand(p));
      next();
      if (obj != kNoSelf && obj != kObjectPlayer && obj != kObjectCamera)
      {
         world::remove(obj);
      }
      return Outcome::Continue;
   }
   case Op::ObjectVisible:
   {
      const uint8_t obj = object_of(c, operand(p));
      const bool on = *p++ != 0;
      next();
      if (obj != kNoSelf && obj != kObjectCamera)
      {
         world::set_visible(obj, on);
      }
      return Outcome::Continue;
   }
   case Op::ObjectTemplate:
   {
      const uint8_t obj = object_of(c, operand(p));
      const auto kind = static_cast<TemplateOp>(*p++);
      int32_t a = 0;
      int32_t b = 0;
      if (kind == TemplateOp::Pos)
      {
         a = operand(p);
         b = operand(p);
      }
      else if (kind == TemplateOp::Movement)
      {
         a = operand(p);
      }
      next();
      if (obj != kNoSelf && obj != kObjectPlayer && obj != kObjectCamera)
      {
         world::set_template(obj, kind, a, b);
      }
      return Outcome::Continue;
   }
   case Op::Door:
   {
      const auto kind = static_cast<DoorOp>(*p++);
      if (kind == DoorOp::Wait)
      {
         return wait(Wait::Door);
      }
      const int16_t x = cell_operand(p);
      const int16_t y = cell_operand(p);
      next();
      world::door(kind, x, y);
      return Outcome::Continue;
   }
   case Op::Weather:
   {
      const auto kind = static_cast<WeatherOp>(*p++);
      const int32_t value = kind == WeatherOp::Set ? operand(p) : 0;
      next();
      world::weather(kind, value);
      return Outcome::Continue;
   }
   case Op::MapView:
   {
      // kind8, [Layout: operand (map)]
      const auto kind = static_cast<MapViewOp>(*p++);
      const int32_t n = kind == MapViewOp::Layout ? operand(p) : 0;
      next();
      switch (kind)
      {
      case MapViewOp::Layout:
         world::set_layout(n);
         break;
      case MapViewOp::Banner:   // C12
         world::show_map_banner();
         break;
      default:
         world::redraw_map();
         break;
      }
      return Outcome::Continue;
   }
   case Op::Picture:
   {
      // C12: kind8, [Show: operand (species), operand (x), operand (y)]
      const auto kind = static_cast<PictureOp>(*p++);
      if (kind == PictureOp::Show)
      {
         const int32_t species = operand(p);
         const int32_t x = operand(p);
         const int32_t y = operand(p);
         next();
         world::picture_show(species, x, y);
         return Outcome::Continue;
      }
      next();
      return world::picture_hide() ? wait(Wait::Picture) : Outcome::Continue;   // nothing to close: no wait
   }
   case Op::Screen:
   {
      // C12: operand (screen); a screen that does not hold the script goes on at once, as a native's Done
      const int32_t screen = operand(p);
      next();
      return world::open_screen(screen) ? wait(Wait::Screen) : Outcome::Continue;
   }
   case Op::Animation:
   {
      // kind8, [Arg: operand (index), operand (value)], [Start, Wait: operand (animation)]
      const auto kind = static_cast<AnimationOp>(*p++);
      const int32_t first = operand(p);
      const int32_t value = kind == AnimationOp::Arg ? operand(p) : 0;
      next();
      if (kind == AnimationOp::Arg)
      {
         world::animation_arg(static_cast<uint8_t>(clamp_to<int32_t>(first, 0, kMaxAnimationArg)), value);
         return Outcome::Continue;
      }
      if (kind == AnimationOp::Start)
      {
         world::animation_start(first);
         return Outcome::Continue;
      }
      // polled at once, as a native's wait is: an animation already over does not hold the script a frame
      const auto id = static_cast<uint16_t>(clamp_to<int32_t>(first, 0, kMaxAnimationId));
      if (world::animation_done(id))
      {
         return Outcome::Continue;
      }
      c.result_var = id;   // the one waited for
      return wait(Wait::Animation);
   }
   case Op::Camera:
   {
      // kind8, [Shake: operand (vertical), operand (horizontal), operand (count), operand (delay)]
      const auto kind = static_cast<CameraOp>(*p++);
      if (kind == CameraOp::Shake)
      {
         const int32_t vertical = operand(p);
         const int32_t horizontal = operand(p);
         const int32_t count = operand(p);
         const int32_t delay = operand(p);
         next();
         world::camera_shake(vertical, horizontal, count, delay);
         return Outcome::Continue;
      }
      next();
      kind == CameraOp::Detach ? world::camera_detach() : world::camera_attach();
      return Outcome::Continue;
   }
   case Op::DarknessFade:
   {
      // operand (level)
      const int32_t level = operand(p);
      next();
      world::fade_darkness(static_cast<uint8_t>(clamp_to<int32_t>(level, 0, map::kMaxDarkness)));
      return world::darkness_fading() ? wait(Wait::Darkness) : Outcome::Continue;   // a change done at once: no wait
   }
   case Op::Fade:
   {
      const auto kind = static_cast<FadeOp>(p[kFadeKindAt]);
      const uint8_t speed = p[kFadeSpeedAt];
      const uint8_t flags = p[kFadeFlagsAt];
      p += kFadeBytes;
      world::fade(kind, speed, flags);
      return wait(Wait::Fade);
   }
   case Op::Prop:
   {
      // kind8, id8, [Place: sprite32, operand x, operand y, elevation8, facing8], [Face: facing8]
      const auto kind = static_cast<PropOp>(*p++);
      const uint8_t id = *p++;
      switch (kind)
      {
      case PropOp::Place:
      {
         const AssetKey sprite{load_le32(p)};
         p += kU32Bytes;
         const int32_t x = operand(p);
         const int32_t y = operand(p);
         const uint8_t elevation = *p++;
         const uint8_t facing = *p++;
         next();
         world::place_prop(id, sprite, x, y, elevation, facing);
         return Outcome::Continue;
      }
      case PropOp::Face:
      {
         const uint8_t facing = *p++;
         next();
         world::face_prop(id, facing);
         return Outcome::Continue;
      }
      case PropOp::Remove:
         next();
         world::remove_prop(id);
         return Outcome::Continue;
      case PropOp::NUM_PROP_OPS:
         break;
      }
      ENGINE_ASSERT(false && "bad prop op");
      return Outcome::Ended;
   }
   case Op::Name:
   {
      // slot8, source8, then the source's bytes (script_vm.json)
      const uint8_t slot = *p++;
      const auto source = static_cast<NameSource>(*p++);
      text::Code codes[text::kMaxPlaceholderCodes + 1] = {};
      int n = 0;
      switch (source)
      {
      case NameSource::Text:
         n = copy_codes(text_ref(p), codes, text::kMaxPlaceholderCodes);
         break;
      case NameSource::Entry:
      {
         const AssetKey key{load_le32(p)};
         p += kU32Bytes;
         const int32_t index = operand(p);
         const asset::List* l = asset::list(key);
         const bool in = l != nullptr && index >= 0 && index < l->count;
         n = copy_codes(in ? asset::text(l->items[index]) : nullptr, codes, text::kMaxPlaceholderCodes);
         break;
      }
      case NameSource::Number:
      {
         const int32_t v = operand(p);
         n = decimal(v > 0 ? v : 0, codes, text::kMaxPlaceholderCodes);   // a name holds no sign
         break;
      }
      case NameSource::Kind:
      {
         const uint8_t kind = *p++;
         const int32_t id = operand(p);
         const bool counted = *p++ != 0;
         const int32_t count = counted ? operand(p) : 1;
         n = game::name_text(kind, id, count, counted, codes, text::kMaxPlaceholderCodes);
         n = n < 0 ? 0 : (n > text::kMaxPlaceholderCodes ? text::kMaxPlaceholderCodes : n);
         break;
      }
      case NameSource::NUM_NAME_SOURCES:
         ENGINE_ASSERT(false && "bad name source");
         return Outcome::Ended;
      }
      codes[n] = text::kCodeEnd;
      next();
      set_name(slot, codes);   // cut to the slot's length
      return Outcome::Continue;
   }
   case Op::Counter:
   {
      // kind8, box8, var16, [Show: x8, y8, label text ref]
      const auto kind = static_cast<CounterOp>(*p++);
      const uint8_t box = *p++;
      const int32_t value = var(VarId{load_le16(p)});
      p += kU16Bytes;
      uint8_t x = 0;
      uint8_t y = 0;
      const text::Code* label = nullptr;
      if (kind == CounterOp::Show)
      {
         x = *p++;
         y = *p++;
         label = text_ref(p);
      }
      next();
      world::counter(kind, box, value, x, y, label);
      return Outcome::Continue;
   }
   case Op::Stat:
   {
      // stat8, operand (the amount)
      const uint8_t stat = *p++;
      const int32_t amount = operand(p);
      next();
      world::add_stat(stat, amount);
      return Outcome::Continue;
   }
   case Op::ObjectOrder:
   {
      // operand (object), fixed8, order8
      const uint8_t obj = object_of(c, operand(p));
      const bool fixed = *p++ != 0;
      const uint8_t order = *p++;
      next();
      if (obj != kNoSelf && obj != kObjectCamera)
      {
         world::set_order(obj, fixed, order);
      }
      return Outcome::Continue;
   }
   case Op::Override:
   {
      // apply8, set16 (map::kWorld.override_sets)
      const bool apply = p[0] != 0;
      const uint16_t set = load_le16(p + 1);
      p += kOverrideBytes;
      next();
      world::override_set(set, apply);
      return Outcome::Continue;
   }
   case Op::SetMetatile:
   {
      const int16_t x = cell_operand(p);
      const int16_t y = cell_operand(p);
      const int32_t tile = operand(p);
      const bool solid = operand(p) != 0;
      next();
      world::set_metatile(x, y, tile, solid);
      return Outcome::Continue;
   }
   case Op::Respawn:
      world::set_respawn(operand(p));
      next();
      return Outcome::Continue;
   case Op::SetWarp:
   {
      const auto slot = static_cast<WarpSlot>(*p++);
      const AssetKey map{load_le32(p)};
      p += kU32Bytes;
      const int32_t x = operand(p);
      const int32_t y = operand(p);
      next();
      world::set_warp(slot, map, x, y);
      return Outcome::Continue;
   }
   case Op::Sound:
   {
      const auto kind = static_cast<SoundOp>(p[0]);
      const uint8_t n = p[1];
      p += kSoundHeaderBytes;
      ENGINE_ASSERT(n <= kMaxSoundArgs);
      int32_t args[kMaxSoundArgs] = {};
      for (uint8_t i = 0; i < n; ++i)
      {
         const int32_t v = operand(p);
         if (i < kMaxSoundArgs)
         {
            args[i] = v;
         }
      }
      next();
      audio::play(kind, args, n < kMaxSoundArgs ? n : static_cast<uint8_t>(kMaxSoundArgs));
      return Outcome::Continue;
   }
   case Op::WaitSound:
      c.result_var = *p++;   // the SoundWait kind
      return wait(Wait::Sound);
   case Op::WaitState:
      return wait(Wait::State);
   default:
      break;
   }
   ENGINE_ASSERT(false && "bad port opcode");
   c.depth = 0;
   return Outcome::Ended;
}

/// Executes one instruction of a context.
/// @param c  the context (depth > 0)
/// @return what happened
Outcome execute(Context& c)
{
   g_self = c.self;
   Frame& f = c.stack[c.depth - 1];
   const uint8_t* const base = code_of(f.script);
   const uint8_t* p = base + f.pc;
   const auto op = static_cast<Op>(*p++);
   // Most instructions continue after themselves; jumps set the pc instead.
   auto next = [&]()
   {
      f.pc = static_cast<uint16_t>(p - base);
   };
   switch (op)
   {
   case Op::End:
      c.depth = 0;
      return Outcome::Ended;
   case Op::Return:
      --c.depth;
      return c.depth == 0 ? Outcome::Ended : Outcome::Continue;
   case Op::Goto:
      f.pc = load_le16(p);
      return Outcome::Continue;
   case Op::Jump:
   {
      // The frame goes on in another script (nothing returns here)
      const uint16_t s = load_le16(p);
      ENGINE_ASSERT(s < bank().script_count);
      f = Frame{s, 0};
      return Outcome::Continue;
   }
   case Op::CallLoose:
   {
      // A call that may recurse; on a full stack it goes on as a jump (the imported game's VM does so)
      const uint16_t s = load_le16(p);
      p += kU16Bytes;
      ENGINE_ASSERT(s < bank().script_count);
      if (c.depth < bank().stack_depth && c.depth < kMaxStack)
      {
         next();
         push(c, s);
      }
      else
      {
         f = Frame{s, 0};
      }
      return Outcome::Continue;
   }
   case Op::Call:
   {
      const uint16_t s = load_le16(p);
      p += kU16Bytes;
      next();
      push(c, s);
      return Outcome::Continue;
   }
   case Op::CallTable:
   {
      const int32_t i = operand(p);
      const uint8_t n = *p++;
      const uint8_t* ids = p;
      p += kU16Bytes * n;   // n script IDs
      next();
      ENGINE_ASSERT(i >= 0 && i < n);   // declared targets (B-1); out of range calls nothing
      if (i >= 0 && i < n)
      {
         push(c, load_le16(ids + kU16Bytes * i));
      }
      return Outcome::Continue;
   }
   case Op::IfFlagClear:
   case Op::IfFlagSet:
   {
      const bool on = flag(FlagId{load_le16(p)});
      const uint16_t target = load_le16(p + kU16Bytes);
      p += kFlagBranchBytes;
      if (on == (op == Op::IfFlagSet))
      {
         f.pc = target;
      }
      else
      {
         next();
      }
      return Outcome::Continue;
   }
   case Op::IfCompare:
   {
      const auto cmp = static_cast<Compare>(*p++);
      const int32_t a = operand(p);
      const int32_t b = operand(p);
      const uint16_t target = load_le16(p);
      p += kU16Bytes;
      if (compare(cmp, a, b))
      {
         f.pc = target;
      }
      else
      {
         next();
      }
      return Outcome::Continue;
   }
   case Op::SetFlag:
   case Op::ClearFlag:
      set_flag(FlagId{load_le16(p)}, op == Op::SetFlag);
      p += kU16Bytes;
      next();
      return Outcome::Continue;
   case Op::SetVar:
   case Op::AddVar:
   case Op::SubVar:
   case Op::Random:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const int32_t x = operand(p);
      next();
      if (op == Op::SetVar)
      {
         set_var(v, x);
      }
      else if (op == Op::AddVar)
      {
         add_var(v, x);
      }
      else if (op == Op::SubVar)
      {
         add_var(v, x == INT32_MIN ? INT32_MAX : -x);
      }
      else if ((bank().vm_flags & kVmRandomModulo) != 0)
      {
         set_var(v, random_modulo(x));
      }
      else
      {
         set_var(v, x <= 0 ? 0 : static_cast<int32_t>(rng::below(rng::gameplay, static_cast<uint32_t>(x))));
      }
      return Outcome::Continue;
   }
   case Op::Message:
      port::message(text_ref(p));
      next();
      c.wait = Wait::Typing;
      return Outcome::Waiting;
   case Op::WaitButton:
      next();
      c.wait = Wait::Button;
      return Outcome::Waiting;
   case Op::CloseMessage:
      port::close_message();
      next();
      return Outcome::Continue;
   case Op::YesNo:
      c.result_var = load_le16(p);
      p += kU16Bytes;
      next();
      port::yesno_open();
      c.wait = Wait::YesNo;
      return Outcome::Waiting;
   case Op::Choice:
   {
      c.result_var = load_le16(p);
      c.cancel = p[kU16Bytes];
      const uint8_t n = p[kU16Bytes + 1];
      p += kChoiceHeaderBytes;
      ENGINE_ASSERT(n >= 1 && n <= kMaxChoices);
      const text::Code* options[kMaxChoices] = {};
      for (uint8_t i = 0; i < n && i < kMaxChoices; ++i)
      {
         options[i] = text_ref(p);
      }
      next();
      port::choice_open(options, n, c.cancel);
      c.wait = Wait::Choice;
      return Outcome::Waiting;
   }
   case Op::Menu:
   {
      // var16, flags8, x8, y8, cursor8, columns8, rows8, delay8, cancel8, list32, style32
      c.result_var = load_le16(p);
      p += kU16Bytes;
      const uint8_t flags = *p++;
      port::MenuRequest r{};
      const bool at = (flags & kMenuAt) != 0;
      r.x = at ? p[0] : port::kMenuPortPlace;
      r.y = at ? p[1] : port::kMenuPortPlace;
      p += kMenuPlaceBytes;
      r.cursor = *p++;
      r.columns = *p++;
      r.rows = *p++;
      r.delay = *p++;
      r.cancel = *p++;
      const AssetKey list{load_le32(p)};
      p += kU32Bytes;
      r.style = AssetKey{load_le32(p)};
      p += kU32Bytes;
      r.back = (flags & kMenuBack) != 0;
      r.fade_wait = (flags & kMenuFadeWait) != 0;
      next();
      // The list's texts (its first kMaxMenuItems; a missing text shows as text_ref()'s); a missing or empty list
      // answers the cancel value at once
      static constexpr text::Code kMissingOption[] = {'?', text::kCodeEnd};
      const text::Code* items[kMaxMenuItems] = {};
      const asset::List* l = asset::list(list);
      uint8_t n = 0;
      for (uint16_t i = 0; l != nullptr && i < l->count && n < kMaxMenuItems; ++i)
      {
         const text::Code* t = asset::text(l->items[i]);
         items[n++] = t != nullptr ? t : kMissingOption;
      }
      r.items = items;
      r.count = n;
      const int16_t a = n > 0 ? port::menu_open(r) : static_cast<int16_t>(r.cancel);
      if (a != port::kMenuWaiting)
      {
         set_var(VarId{c.result_var}, a);
         return Outcome::Continue;
      }
      c.wait = Wait::Menu;
      return Outcome::Waiting;
   }
   case Op::Delay:
   {
      const int32_t n = operand(p);
      next();
      if (n <= 0)
      {
         return Outcome::Continue;
      }
      c.delay = static_cast<uint32_t>(n);
      c.wait = Wait::Delay;
      return Outcome::Waiting;
   }
   case Op::Native:
   {
      const uint16_t id = load_le16(p);
      const uint8_t argc = p[kU16Bytes];
      p += kU16Bytes + 1;   // native ID, argument count
      ENGINE_ASSERT(argc <= kMaxNativeArgs);
      int32_t args[kMaxNativeArgs] = {};
      for (uint8_t i = 0; i < argc && i < kMaxNativeArgs; ++i)
      {
         args[i] = operand(p);
      }
      const uint16_t result_var = load_le16(p);
      p += kU16Bytes;
      int32_t result = 0;
      // The native may ask where the script goes on (native_return_point(), native_goto())
      g_native_after = Frame{f.script, static_cast<uint16_t>(p - base)};
      g_native_goto = false;
      if (game::native(id, args, argc, result) == game::NativeStatus::Wait)
      {
         g_native_goto = false;   // a redirect counts only with Done
         c.wait = Wait::Native;   // pc stays: the native runs again next frame
         return Outcome::Waiting;
      }
      if (g_native_goto)
      {
         g_native_goto = false;
         ENGINE_ASSERT(g_native_to.script < bank().script_count);
         f = g_native_to;   // a goto: the frame goes on there, the call stack keeps its depth
      }
      else
      {
         next();
      }
      // A native that waited and is now done ends the frame when the bank resumes on the next one
      const bool yield = g_native_rerun && (bank().vm_flags & kVmResumeNextFrame) != 0;
      if (result_var != kNoVar)
      {
         set_var(VarId{result_var}, result);
      }
      return yield ? Outcome::Waiting : Outcome::Continue;   // Waiting with Wait::None: on from the next frame
   }
   case Op::TriggerTrue:
      c.depth = 0;
      return Outcome::True;
   case Op::ObjectMove:
   {
      const uint8_t obj = object_of(c, operand(p));
      const uint8_t dir = *p++;
      const int32_t steps = operand(p);
      next();
      if (obj == kNoSelf ||
          !world::move(obj, dir & kDirectionMask, static_cast<uint16_t>(clamp_to<int32_t>(steps, 0, kMaxSteps))))
      {
         return Outcome::Continue;   // no such object: nothing to wait for
      }
      c.result_var = obj;
      c.wait = Wait::Object;
      return Outcome::Waiting;
   }
   case Op::ObjectFace:
   {
      const uint8_t obj = object_of(c, operand(p));
      const uint8_t face = *p++;
      next();
      if (obj == kNoSelf)
      {
         return Outcome::Continue;
      }
      if (face < kFacePlayer)   // below the special targets: a direction
      {
         world::face(obj, face);
      }
      else if (face == kFaceOppositePlayer)
      {
         // The walker's facing turned around (down <-> up, left <-> right), whatever the geometry
         const auto facing = static_cast<uint8_t>(world::query(Query::Facing, kObjectPlayer) & kDirectionMask);
         world::face(obj, static_cast<uint8_t>(facing ^ kOppositeDirectionBit));
      }
      else
      {
         // towards the walker, the context's own object or (C1) the encounter's object
         const uint8_t other = face == kFacePlayer ? kObjectPlayer
                             : face == kFaceEncounter ? detail::encounter_object()
                                                      : c.self;
         if (other != kNoSelf && other != obj)
         {
            world::face_toward(obj, other);
         }
      }
      return Outcome::Continue;
   }
   case Op::Route:
   {
      const uint8_t obj = object_of(c, operand(p));
      const uint8_t flags = *p++;
      const uint16_t len = load_le16(p);
      p += kU16Bytes;
      const uint8_t* code = p;
      p += len;
      next();
      c.last_object = obj;   // kObjectLast names it
      if (obj == kNoSelf || !world::route(obj, code, len, true) || (flags & kRouteNoWait) != 0)
      {
         return Outcome::Continue;
      }
      c.result_var = obj;
      c.wait = Wait::Object;
      return Outcome::Waiting;
   }
   case Op::WaitMoves:
      next();
      c.wait = Wait::Moves;
      return Outcome::Waiting;
   case Op::SelfSwitch:
   {
      const uint8_t letter = p[0];
      const bool on = p[1] != 0;
      p += kSelfSwitchBytes;
      next();
      if (c.self != kNoSelf && c.self != kObjectPlayer)
      {
         world::set_self_switch(c.self, letter & kSelfSwitchMask, on);
      }
      return Outcome::Continue;
   }
   case Op::IfSelfSet:
   case Op::IfSelfClear:
   {
      const uint8_t letter = p[0];
      const uint16_t target = load_le16(p + 1);
      p += 1 + kU16Bytes;   // letter, target
      const bool on =
         c.self != kNoSelf && c.self != kObjectPlayer && world::self_switch(c.self, letter & kSelfSwitchMask);
      if (on == (op == Op::IfSelfSet))
      {
         f.pc = target;
      }
      else
      {
         next();
      }
      return Outcome::Continue;
   }
   case Op::VarOp:
   {
      const auto kind = static_cast<VarOpKind>(*p++);
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const int32_t x = operand(p);
      next();
      const int32_t cur = var(v);
      if (kind == VarOpKind::Mul)
      {
         const int64_t r = static_cast<int64_t>(cur) * x;
         set_var(v, r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : static_cast<int32_t>(r)));
      }
      else if (x != 0 && !(x == -1 && cur == INT32_MIN))
      {
         set_var(v, kind == VarOpKind::Div ? cur / x : cur % x);
      }
      return Outcome::Continue;
   }
   case Op::InputNumber:
      c.result_var = load_le16(p);
      port::number_open(p[kU16Bytes]);
      p += kU16Bytes + 1;   // result variable, digits
      next();
      c.wait = Wait::Number;
      return Outcome::Waiting;
   case Op::WaitKey:
      c.result_var = load_le16(p);
      p += kU16Bytes;
      next();
      c.wait = Wait::Key;
      return Outcome::Waiting;
   case Op::MessagePos:
      port::message_position(*p++);
      next();
      return Outcome::Continue;
   case Op::Warp:
   {
      const AssetKey map{load_le32(p)};
      p += kU32Bytes;
      const int32_t x = operand(p);
      const int32_t y = operand(p);
      const uint8_t facing = p[0];
      const uint8_t transition = p[1];
      p += kWarpTailBytes;
      next();
      world::warp(map, static_cast<int16_t>(clamp_to<int32_t>(x, INT16_MIN, INT16_MAX)),
                  static_cast<int16_t>(clamp_to<int32_t>(y, INT16_MIN, INT16_MAX)),
                  static_cast<uint8_t>(facing & kWarpFacingMask), transition);
      c.self = kNoSelf;   // the object stays behind on the old map
      if ((facing & kWarpNoWait) != 0)
      {
         return Outcome::Continue;   // the script goes on while the warp starts
      }
      c.wait = Wait::Warp;
      return Outcome::Waiting;
   }
   case Op::Place:
   {
      const uint8_t obj = object_of(c, operand(p));
      const int32_t x = operand(p);
      const int32_t y = operand(p);
      const uint8_t facing = *p++;
      next();
      if (obj != kNoSelf)
      {
         world::place(obj, static_cast<int16_t>(clamp_to<int32_t>(x, INT16_MIN, INT16_MAX)),
                      static_cast<int16_t>(clamp_to<int32_t>(y, INT16_MIN, INT16_MAX)), facing);
      }
      return Outcome::Continue;
   }
   case Op::Scroll:
   {
      const uint8_t dir = *p++;
      const int32_t cells = operand(p);
      const uint8_t speed = *p++;
      next();
      world::scroll(dir, static_cast<int16_t>(clamp_to<int32_t>(cells, 0, kMaxScrollCells)), speed);
      c.wait = Wait::Scroll;
      return Outcome::Waiting;
   }
   case Op::Tint:
   case Op::Flash:
   {
      const hal::video::Color color = hal::video::rgb5(p[kRed], p[kGreen], p[kBlue]);
      p += kColorBytes;
      const uint8_t strength = op == Op::Tint ? *p++ : 0;
      const uint16_t frames = load_le16(p);
      p += kU16Bytes;
      next();
      if (op == Op::Tint)
      {
         world::tint(color, strength, frames);
      }
      else
      {
         world::flash(color, frames);
      }
      return Outcome::Continue;
   }
   case Op::Shake:
   {
      const uint8_t power = p[0];
      const uint16_t frames = load_le16(p + 1);
      p += 1 + kU16Bytes;   // power, frames
      next();
      world::shake(power, frames);
      return Outcome::Continue;
   }
   case Op::TimerStart:
   {
      const int32_t secs = operand(p);
      next();
      world::timer_start(clamp_to<int32_t>(secs, 1, kMaxTimerSeconds));
      return Outcome::Continue;
   }
   case Op::TimerStop:
      next();
      world::timer_stop();
      return Outcome::Continue;
   case Op::Encounters:
      world::encounters(*p++ != 0);
      next();
      return Outcome::Continue;
   case Op::Vehicle:
      world::set_vehicle(*p++);
      next();
      return Outcome::Continue;
   case Op::TimeOfDay:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      next();
      set_var(v, clock::time_of_day());
      return Outcome::Continue;
   }
   case Op::FieldEffect:
   {
      // F1: a timed field effect (capsule/script/field_effects.h): start it by its stack rule, or read it
      const auto kind = static_cast<FieldEffectOp>(*p++);
      const uint8_t effect = *p++;
      const int32_t amount = kind == FieldEffectOp::Start ? operand(p) : 0;
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      next();
      int32_t result = 0;
      if (kind == FieldEffectOp::Start)
      {
         result = field_effects::start(effect, amount) ? 1 : 0;
      }
      else if (kind == FieldEffectOp::Active)
      {
         result = field_effects::active(effect) ? 1 : 0;
      }
      else
      {
         result = field_effects::remaining(effect);
      }
      if (v != kNoVar)
      {
         set_var(VarId{v}, result);
      }
      return Outcome::Continue;
   }
   case Op::Darkness:
   {
      // The shown map's darkness level until the next map
      const int32_t level = operand(p);
      next();
      world::set_darkness(static_cast<uint8_t>(clamp_to<int32_t>(level, 0, map::kMaxDarkness)));
      return Outcome::Continue;
   }
   case Op::Flight:
   {
      // Record, visit or pick a flight point (capsule/script/field_actions.h)
      const auto kind = static_cast<FlightOp>(*p++);
      if (kind == FlightOp::Menu)
      {
         c.result_var = load_le16(p);
         p += kU16Bytes;
         next();
         actions::menu_open();
         c.wait = Wait::Flight;
         return Outcome::Waiting;
      }
      const uint8_t point = *p++;
      const uint8_t transition = kind == FlightOp::Visit ? *p++ : kTransitionDefault;
      next();
      const FlightPointInfo* fp = actions::flight_point(point);
      if (kind == FlightOp::Record || fp == nullptr)
      {
         actions::record(point);
         return Outcome::Continue;
      }
      world::warp(fp->map, fp->x, fp->y, fp->facing, transition);
      c.self = kNoSelf;   // the object stays behind on the old map
      c.wait = Wait::Warp;
      return Outcome::Waiting;
   }
   case Op::Escape:
   {
      // Warp to the escape point the overworld recorded
      const uint8_t transition = *p++;
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      next();
      const bool went = world::escape(transition);
      if (v != kNoVar)
      {
         set_var(VarId{v}, went ? 1 : 0);
      }
      if (!went)
      {
         return Outcome::Continue;
      }
      c.self = kNoSelf;
      c.wait = Wait::Warp;
      return Outcome::Waiting;
   }
   case Op::FieldAction:
   {
      // The first party member a field action is available to, plus 1 (0: none)
      p++;   // kind: FieldActionOp::Available, the only one
      const uint8_t action = *p++;
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      next();
      const uint8_t member = actions::available(action);
      set_var(VarId{v}, member == actions::kNoMember ? 0 : int32_t{member} + 1);
      return Outcome::Continue;
   }
   case Op::ObjectMoveChecked:
   {
      // The walk by the movement rules: every step allowed, or nothing moves
      const uint8_t obj = object_of(c, operand(p));
      const uint8_t dir = static_cast<uint8_t>(*p++ & kDirectionMask);
      const auto steps = static_cast<uint16_t>(clamp_to<int32_t>(operand(p), 0, kMaxSteps));
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      next();
      const bool clear = obj != kNoSelf && world::path_clear(obj, dir, steps);
      const bool walks = clear && world::move(obj, dir, steps);
      if (v != kNoVar)
      {
         set_var(VarId{v}, walks ? 1 : 0);
      }
      if (!walks)
      {
         return Outcome::Continue;
      }
      c.result_var = obj;
      c.wait = Wait::Object;
      return Outcome::Waiting;
   }
   case Op::ObjectShow:
   case Op::ObjectHide:
   {
      const uint8_t obj = object_of(c, operand(p));
      next();
      if (obj != kNoSelf && obj != kObjectPlayer)
      {
         world::show(obj, op == Op::ObjectShow);
      }
      return Outcome::Continue;
   }
   case Op::Lock:
   case Op::Release:
   case Op::MessageEx:
   case Op::WaitMessage:
   case Op::WaitObject:
   case Op::ObjectRemove:
   case Op::ObjectVisible:
   case Op::ObjectTemplate:
   case Op::Door:
   case Op::Weather:
   case Op::Fade:
   case Op::MapView:
   case Op::Animation:
   case Op::Camera:
   case Op::DarknessFade:
   case Op::Picture:
   case Op::Screen:
   case Op::SetMetatile:
   case Op::Respawn:
   case Op::SetWarp:
   case Op::Sound:
   case Op::WaitSound:
   case Op::WaitState:
   case Op::Override:
   case Op::Prop:
   case Op::ObjectOrder:
   case Op::Name:
   case Op::Counter:
   case Op::Stat:
      return execute_port(c, op, p, base);
   // D-36 package 2: the ops of the bag, boxes, party roles, growth, combination and nursery modules
   case Op::RoleGet:
   case Op::RoleSet:
   case Op::ItemGive:
   case Op::ItemTake:
   case Op::ItemCount:
   case Op::BagOpen:
   case Op::Shop:
   case Op::Mart:
   case Op::BoxCount:
   case Op::BoxDeposit:
   case Op::BoxWithdraw:
   case Op::BoxesOpen:
   case Op::Transform:
   case Op::Combine:
   case Op::Nursery:
   case Op::Encounter:
   case Op::Battle:
   case Op::BattleSetup:
   case Op::Party:
   case Op::ItemQuery:
   case Op::Member:
      return run_module(c, op, p, base);
   case Op::NUM_OPS:
      break;
   }
   ENGINE_ASSERT(false && "bad opcode");
   c.depth = 0;
   return Outcome::Ended;
}

/// Checks a context's wait; clears it when over.
/// @param c  the context
/// @return true while it still waits
bool still_waiting(Context& c)
{
   switch (c.wait)
   {
   case Wait::None:
      return false;
   case Wait::Delay:
      if (--c.delay != 0)
      {
         return true;
      }
      break;
   case Wait::Typing:
      if (port::message_busy())
      {
         return true;
      }
      break;
   case Wait::Button:
      if (!port::button())
      {
         return true;
      }
      break;
   case Wait::YesNo:
   {
      const int8_t a = port::yesno_poll();
      if (a < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, a);
      break;
   }
   case Wait::Choice:
   {
      const int16_t a = port::choice_poll();
      if (a < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, a);
      break;
   }
   case Wait::Menu:
   {
      const int16_t a = port::menu_poll();
      if (a == port::kMenuWaiting)
      {
         return true;
      }
      set_var(VarId{c.result_var}, a);
      break;
   }
   case Wait::Native:
      break;   // the native instruction runs again
   case Wait::Object:
      if (world::moving(static_cast<uint8_t>(c.result_var)))
      {
         return true;
      }
      break;
   case Wait::Moves:
      if (world::routes_busy())
      {
         return true;
      }
      break;
   case Wait::Number:
   {
      const int32_t a = port::number_poll();
      if (a < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, a);
      break;
   }
   case Wait::Key:
   {
      const int8_t k = port::key_poll();
      if (k < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, k);
      break;
   }
   case Wait::Warp:
      if (world::warping())
      {
         return true;
      }
      break;
   case Wait::Scroll:
      if (world::scrolling())
      {
         return true;
      }
      break;
   case Wait::Flight:
   {
      // The flight menu's answer; a point picked warps there (then the warp's wait)
      const int32_t picked = actions::menu_poll();
      if (picked == actions::kFlightWaiting)
      {
         return true;
      }
      const FlightPointInfo* fp = picked >= 0 ? actions::flight_point(static_cast<uint8_t>(picked)) : nullptr;
      set_var(VarId{c.result_var}, fp != nullptr ? 1 : 0);
      if (fp == nullptr)
      {
         break;
      }
      world::warp(fp->map, fp->x, fp->y, fp->facing, kTransitionDefault);
      c.self = kNoSelf;
      c.wait = Wait::Warp;
      return true;
   }
   case Wait::Door:
      if (world::door_busy())
      {
         return true;
      }
      break;
   case Wait::Fade:
      if (world::fading())
      {
         return true;
      }
      break;
   case Wait::Animation:
      if (!world::animation_done(c.result_var))
      {
         return true;
      }
      break;
   case Wait::Darkness:
      if (world::darkness_fading())
      {
         return true;
      }
      break;
   case Wait::Picture:
      if (world::picture_busy())
      {
         return true;
      }
      break;
   case Wait::Screen:
      if (world::screen_busy())
      {
         return true;
      }
      break;
   case Wait::Sound:
      if (audio::playing(static_cast<SoundWait>(c.result_var)))
      {
         return true;
      }
      break;
   case Wait::State:
      if (game::state_busy())
      {
         return true;
      }
      break;
   // D-36 package 2: the module waits
   case Wait::Bag:
   case Wait::Shop:
   case Wait::Mart:
   case Wait::Boxes:
   case Wait::Transform:
   case Wait::Combine:
   case Wait::Nursery:
   case Wait::Battle:
   case Wait::Approach:
      if (poll_module(c))
      {
         return true;
      }
      break;
   case Wait::NUM_WAITS:
      break;
   }
   c.wait = Wait::None;
   return false;
}

/// Runs a context that must not wait until it ends.
/// @param s  the script
/// @return true when it ended with TriggerTrue
bool run_to_end_body(ScriptId s)
{
   Context c{};
   c.self = kNoSelf;
   c.last_object = kNoSelf;
   push(c, raw(s));
   for (int i = 0; i < kMaxRunSteps && c.depth != 0; ++i)
   {
      const Outcome o = execute(c);
      ENGINE_ASSERT(o != Outcome::Waiting);   // the compiler proves on_load / conditions never wait
      if (o == Outcome::True)
      {
         return true;
      }
      // Waiting: a native declared without `waits` that is not done; the
      // instruction runs again (bounded by the loop).
   }
   ENGINE_ASSERT(c.depth == 0);   // an endless loop in a trigger condition
   return false;
}

bool g_immediate = false;   ///< run_to_end() runs (immediate()).

/// Runs a context that must not wait until it ends (immediate() is true meanwhile).
/// @param s  the script
/// @return true when it ended with TriggerTrue
bool run_to_end(ScriptId s)
{
   // immediate() is true meanwhile (nested runs keep it so)
   const bool outer = g_immediate;
   g_immediate = true;
   const bool result = run_to_end_body(s);
   g_immediate = outer;
   return result;
}

} // namespace

namespace detail
{

/// Mart (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_mart(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const uint8_t kind = *p++;
   const uint16_t n = load_le16(p);
   p += kU16Bytes;
   const uint8_t* items = p;
   p += kU16Bytes * n;
   items::mart_open(kind, items, n);
   advance(f, p, base);
   c.wait = Wait::Mart;
   return Outcome::Waiting;
}

/// RoleGet (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_role_get(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   const uint8_t slot = byte_index(operand(p));
   const uint8_t role = *p++;
   const uint8_t element = byte_index(operand(p));
   advance(f, p, base);
   set_var(v, party::role_get(slot, role, element));   // clamps to the variable's range
   return Outcome::Continue;
}

/// RoleSet (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_role_set(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const uint8_t slot = byte_index(operand(p));
   const uint8_t role = *p++;
   const uint8_t element = byte_index(operand(p));
   const int32_t value = operand(p);
   advance(f, p, base);
   party::role_set(slot, role, element, value);
   return Outcome::Continue;
}

/// ItemGive / ItemTake (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_item_give(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // The bag through the items port (all or nothing); the result variable gets 1 when it worked
   const uint16_t v = load_le16(p);
   p += kU16Bytes;
   const int32_t item = operand(p);
   const int32_t n = operand(p);
   advance(f, p, base);
   const auto id = static_cast<uint16_t>(clamp_to<int32_t>(item, 0, kMaxItemId));
   const bool ok = op == Op::ItemGive ? items::give(id, n) : items::take(id, n);
   if (v != kNoVar)
   {
      set_var(VarId{v}, ok ? 1 : 0);
   }
   return Outcome::Continue;
}

/// ItemCount (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_item_count(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   const int32_t item = operand(p);
   advance(f, p, base);
   set_var(v, items::count(static_cast<uint16_t>(clamp_to<int32_t>(item, 0, kMaxItemId))));
   return Outcome::Continue;
}

/// ItemQuery (C7: a module op, reached through the game's dispatch table): the bag's room for an item, or its pocket.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_item_query(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // kind8, var16, operand (item), [Room: operand (n)]
   const auto kind = static_cast<ItemQueryOp>(*p++);
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   const auto item = static_cast<uint16_t>(clamp_to<int32_t>(operand(p), 0, kMaxItemId));
   const int32_t n = kind == ItemQueryOp::Room ? operand(p) : 0;
   advance(f, p, base);
   set_var(v, kind == ItemQueryOp::Room ? (items::room(item, n) ? 1 : 0) : items::pocket(item));
   return Outcome::Continue;
}

/// Party (C7: a module op, reached through the game's dispatch table): heal, stash, restore, count, find a move.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_party(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // kind8, [Count: hatched8, var16], [Find: var16 (slot), var16 (species or kNoVar), operand (move)],
   // [Give: var16, operand (species), operand (level), operand (item)], [GiveEgg: var16, operand (species)]
   const auto kind = static_cast<PartyOp>(*p++);
   switch (kind)
   {
   case PartyOp::Heal:
   case PartyOp::Stash:
   case PartyOp::Restore:
      advance(f, p, base);
      kind == PartyOp::Heal ? party::heal() : kind == PartyOp::Stash ? party::stash() : party::restore();
      break;
   case PartyOp::Count:
   {
      const bool hatched = *p++ != 0;
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      advance(f, p, base);
      set_var(v, party::count(hatched));
      break;
   }
   case PartyOp::Find:
   {
      const VarId slot{load_le16(p)};
      p += kU16Bytes;
      const uint16_t species_var = load_le16(p);
      p += kU16Bytes;
      const auto move = static_cast<uint16_t>(clamp_to<int32_t>(operand(p), 0, kMaxMoveId));
      advance(f, p, base);
      int32_t at = 0;
      int32_t species = 0;
      const bool found = party::find_move(move, at, species);
      set_var(slot, at);
      if (species_var != kNoVar && found)
      {
         set_var(VarId{species_var}, species);   // the species only when a member knows it
      }
      break;
   }
   case PartyOp::Give:
   case PartyOp::GiveEgg:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const auto species = static_cast<uint16_t>(clamp_to<int32_t>(operand(p), 0, kMaxSpeciesId));
      int32_t result = kGivenNone;
      if (kind == PartyOp::Give)
      {
         const auto level = static_cast<uint8_t>(clamp_to<int32_t>(operand(p), 0, kMaxLevel));
         const auto item = static_cast<uint16_t>(clamp_to<int32_t>(operand(p), 0, kMaxItemId));
         advance(f, p, base);
         result = party::give(species, level, item);
      }
      else
      {
         advance(f, p, base);
         result = party::give_egg(species);
      }
      set_var(v, result);
      break;
   }
   case PartyOp::Lead:   // C13: var16
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      advance(f, p, base);
      set_var(v, party::lead());
      break;
   }
   case PartyOp::CountAlive:   // C13: var16, operand (slot left out, or kNoMember)
   case PartyOp::HasType:      // C13: var16, operand (type)
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const int32_t x = operand(p);
      advance(f, p, base);
      set_var(v, kind == PartyOp::CountAlive ? party::count_alive(x) : (party::has_type(x) ? 1 : 0));
      break;
   }
   case PartyOp::NUM_PARTY_OPS:
      advance(f, p, base);
      break;
   }
   return Outcome::Continue;
}

/// C13: Member (a module op, with Party).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_member(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // field8 (MemberField), var16, operand (slot)
   const auto field = static_cast<MemberField>(*p++);
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   const int32_t slot = operand(p);
   advance(f, p, base);
   set_var(v, field < MemberField::NUM_MEMBER_FIELDS ? party::member_field(slot, field) : 0);
   return Outcome::Continue;
}

/// BagOpen (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_bag_open(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   c.result_var = load_le16(p);
   p += kU16Bytes;
   advance(f, p, base);
   items::bag_open();
   c.wait = Wait::Bag;
   return Outcome::Waiting;
}

/// Shop (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_shop(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const uint8_t shop = *p++;
   const VarId money{load_le16(p)};
   p += kU16Bytes;
   advance(f, p, base);
   items::shop_open(shop, money);
   c.wait = Wait::Shop;
   return Outcome::Waiting;
}

/// BoxCount (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_box_count(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // The storage boxes through the boxes port
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   advance(f, p, base);
   set_var(v, boxes::count());
   return Outcome::Continue;
}

/// BoxDeposit / BoxWithdraw (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_box_deposit(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const uint16_t v = load_le16(p);
   p += kU16Bytes;
   const int32_t at = operand(p);
   advance(f, p, base);
   const bool ok = op == Op::BoxDeposit ? boxes::deposit(at) : boxes::withdraw(at);
   if (v != kNoVar)
   {
      set_var(VarId{v}, ok ? 1 : 0);
   }
   return Outcome::Continue;
}

/// BoxesOpen (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_boxes_open(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   c.result_var = load_le16(p);
   p += kU16Bytes;
   advance(f, p, base);
   boxes::open();
   c.wait = Wait::Boxes;
   return Outcome::Waiting;
}

/// Transform (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_transform(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // A party member's transformation (the script trigger's rules) through the growth port; waits
   c.result_var = load_le16(p);
   p += kU16Bytes;
   const int32_t slot = operand(p);
   advance(f, p, base);
   // a slot past the party is refused by the port (nothing changes)
   growth::transform_open(static_cast<uint8_t>(clamp_to<int32_t>(slot, 0, monster::kMaxPartySize)));
   c.wait = Wait::Transform;
   return Outcome::Waiting;
}

/// Combine (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_combine(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // A combination method through the combine port (Open / Run wait; the queries answer at once)
   const auto kind = static_cast<CombineOp>(*p++);
   const uint8_t method = *p++;
   uint8_t slots[kMaxCombineMembers] = {};
   uint8_t count = 0;
   if (kind != CombineOp::Open)
   {
      const uint8_t given = *p++;
      for (uint8_t k = 0; k < given; ++k)
      {
         const int32_t slot = operand(p);
         if (count < kMaxCombineMembers)
         {
            // a slot past the party is refused by the port
            slots[count++] = static_cast<uint8_t>(clamp_to<int32_t>(slot, 0, monster::kMaxPartySize));
         }
      }
   }
   const uint16_t v = load_le16(p);
   p += kU16Bytes;
   advance(f, p, base);
   if (kind == CombineOp::Open || kind == CombineOp::Run)
   {
      c.result_var = v;
      if (kind == CombineOp::Open)
      {
         combine::open(method);
      }
      else
      {
         combine::run(method, slots, count);
      }
      c.wait = Wait::Combine;
      return Outcome::Waiting;
   }
   set_var(VarId{v}, combine::query(kind, method, slots, count));
   return Outcome::Continue;
}

/// Nursery (D-36 package 2: a module op, reached through the game's dispatch table).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_nursery(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // The nursery and the nickname prompt through the nursery port (Pick / Name wait; the rest answer at once)
   const auto kind = static_cast<NurseryOp>(*p++);
   const bool has_arg = kind == NurseryOp::Deposit || kind == NurseryOp::Withdraw || kind == NurseryOp::Levels ||
                        kind == NurseryOp::Fee || kind == NurseryOp::Name;
   const int32_t arg = has_arg ? operand(p) : 0;
   uint16_t money = kNoVar;
   if (kind == NurseryOp::Withdraw)
   {
      money = load_le16(p);
      p += kU16Bytes;
   }
   const uint16_t v = load_le16(p);
   p += kU16Bytes;
   advance(f, p, base);
   if (kind == NurseryOp::Pick || kind == NurseryOp::Name)
   {
      c.result_var = v;
      if (kind == NurseryOp::Pick)
      {
         nursery::pick_open();
      }
      else
      {
         nursery::name_open(static_cast<uint8_t>(clamp_to<int32_t>(arg, 0, monster::kMaxPartySize)));
      }
      c.wait = Wait::Nursery;
      return Outcome::Waiting;
   }
   if (kind == NurseryOp::Withdraw)
   {
      // a negative money value takes no fee (no money variable)
      const int32_t have = money != kNoVar ? var(VarId{money}) : -1;
      int32_t paid = 0;
      const int32_t r = nursery::withdraw(arg, have, paid);
      if (money != kNoVar && paid > 0)
      {
         set_var(VarId{money}, have - paid);
      }
      set_var(VarId{v}, r);
      return Outcome::Continue;
   }
   set_var(VarId{v}, nursery::query(kind, arg));
   return Outcome::Continue;
}

/// Wait::Bag (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_bag(Context& c)
{
   const int32_t item = items::bag_poll();
   if (item < 0)
   {
      return true;
   }
   set_var(VarId{c.result_var}, item);
   return false;
}

/// Wait::Shop (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_shop([[maybe_unused]] Context& c)
{
   if (items::shop_poll())
   {
      return true;
   }
   return false;
}

/// Wait::Boxes (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_boxes(Context& c)
{
   const int32_t box = boxes::poll();
   if (box < 0)
   {
      return true;
   }
   set_var(VarId{c.result_var}, box);
   return false;
}

/// Wait::Transform (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_transform(Context& c)
{
   const int16_t result = growth::transform_poll();
   if (result == growth::kTransformRunning)
   {
      return true;
   }
   set_var(VarId{c.result_var}, result);
   return false;
}

/// Wait::Combine (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_combine(Context& c)
{
   const int16_t result = combine::poll();
   if (result == combine::kCombineRunning)
   {
      return true;
   }
   set_var(VarId{c.result_var}, result);
   return false;
}

/// Wait::Nursery (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_nursery(Context& c)
{
   const int16_t result = nursery::poll();
   if (result == nursery::kNurseryRunning)
   {
      return true;
   }
   set_var(VarId{c.result_var}, result);
   return false;
}

/// Wait::Mart (D-36 package 2: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while it still waits (the result variable is written when it ends)
bool poll_mart([[maybe_unused]] Context& c)
{
   if (items::mart_poll())
   {
      return true;
   }
   return false;
}

namespace
{

/// @name The encounter record (D-36 package 2 C1: Op::Encounter, Op::Battle)
///@{

/// Texts a side of the encounter record holds (intro, defeat, cannot battle).
constexpr int kEncounterTexts = 3;
/// Bytes of a stored text reference (its tag and its 32-bit value, as the bytecode has it).
constexpr int kTextRefBytes = 1 + kU32Bytes;
/// Sides of the encounter record (A, B and C: REF-14's most positions).
constexpr int kEncounterSides = 3;
/// The record's slot of each side code (kEncounterSide...: C's code is 3, after kEncounterSideCurrent).
constexpr uint8_t kSideCodes[kEncounterSides] = {kEncounterSideA, kEncounterSideB, kEncounterSideC};
/// The slot of side A.
constexpr uint8_t kSlotA = 0;
/// g_load_slot while no Load runs.
constexpr uint8_t kNotLoading = 0xFF;

/// One side of the encounter record.
struct EncounterSide
{
   uint8_t text[kEncounterTexts][kTextRefBytes];   ///< The texts' references (by kEncounterHas... bit order).
   uint16_t party;                                 ///< The game's party number (battles::kNoParty: empty).
   uint16_t beaten;                                ///< Its beaten flag, or kEncounterNoFlag.
   uint16_t after;                                 ///< Its after-script, or kNoScriptId.
   uint8_t object;                                 ///< Its map object (local ID), or kNoSelf.
   uint8_t parts;                                  ///< Which texts it has (kEncounterHas... bits).
};

/// The encounter record (session state: a party battle's set-up, as the scripts fill it).
struct EncounterRecord
{
   EncounterSide sides[kEncounterSides];   ///< Sides A and B.
   Frame back;                             ///< Where Continue goes on (the instruction after the last Set).
   bool has_back;                          ///< A Set gave the return point.
   uint8_t bits;                           ///< The mode bits (kEncounterPair ...).
   uint8_t mode;                           ///< The game's own mode number (opaque to the engine).
   uint8_t current;                        ///< The current side's slot (kSideCodes index).
};

/// The encounter record (EWRAM: IWRAM is the stack's).
CAPSULE_EWRAM_BSS EncounterRecord g_encounter;
/// C1b: the slot a Load fills while a spotter's script runs to its Set, or kNotLoading.
uint8_t g_load_slot = kNotLoading;

/// The slot of a side code.
/// @param side  kEncounterSideA, B or C
/// @return its slot, or kEncounterSides for another value
constexpr int slot_of(uint8_t side)
{
   for (int i = 0; i < kEncounterSides; ++i)
   {
      if (kSideCodes[i] == side)
      {
         return i;
      }
   }
   return kEncounterSides;
}

/// An empty side.
/// @return it
constexpr EncounterSide empty_side()
{
   EncounterSide s{};
   s.party = battles::kNoParty;
   s.beaten = kEncounterNoFlag;
   s.after = kNoScriptId;
   s.object = kNoSelf;
   return s;
}

/// The current side's slot.
/// @return it (side A's for a bad value)
uint8_t current_slot()
{
   return g_encounter.current < kEncounterSides ? g_encounter.current : kSlotA;
}

/// The current side.
/// @return it
EncounterSide& current_side()
{
   return g_encounter.sides[g_encounter.current < kEncounterSides ? g_encounter.current : kSlotA];
}

/// Empties the record (Clear, Load).
void clear_record()
{
   g_encounter = EncounterRecord{};
   for (EncounterSide& s : g_encounter.sides)
   {
      s = empty_side();
   }
}

/// A side's texts by index: their kEncounterHas... bits.
constexpr uint8_t kEncounterTextBits[kEncounterTexts] = {kEncounterHasIntro, kEncounterHasDefeat, kEncounterHasCannot};

/// The index of a text in a side (its kEncounterHas... bit's position in kEncounterTextBits).
/// @param bit  kEncounterHasIntro, kEncounterHasDefeat or kEncounterHasCannot
/// @return the index, or kEncounterTexts for another value
int text_index(uint8_t bit)
{
   for (int i = 0; i < kEncounterTexts; ++i)
   {
      if (kEncounterTextBits[i] == bit)
      {
         return i;
      }
   }
   return kEncounterTexts;
}

/// Reads Encounter Set's operands into a side and the record.
/// @param c  the context
/// @param p  the position after the side byte; moved past the operands
/// @param s  the side
void read_side(Context& c, const uint8_t*& p, EncounterSide& s)
{
   s = empty_side();
   s.party = static_cast<uint16_t>(operand(p));
   s.object = object_of(c, operand(p));
   s.beaten = load_le16(p);
   p += kU16Bytes;
   s.parts = *p++;
   // the texts the parts name, in bit order, as references copied from the bytecode
   for (const uint8_t bit : kEncounterTextBits)
   {
      if ((s.parts & bit) != 0)
      {
         std::memcpy(s.text[text_index(bit)], p, kTextRefBytes);
         p += kTextRefBytes;
      }
   }
   if ((s.parts & kEncounterHasAfter) != 0)
   {
      s.after = load_le16(p);
      p += kU16Bytes;
   }
}

/// Moves a frame to the record's return point, or ends the script without one.
/// @param c  the context
/// @param f  the running frame
/// @return what happened
Outcome go_back(Context& c, Frame& f)
{
   if (!g_encounter.has_back)
   {
      c.depth = 0;   // nothing to go on with (the imported game's fallback is the game's own)
      return Outcome::Ended;
   }
   f = g_encounter.back;
   return Outcome::Continue;
}

/// C1b (Load): runs a spotter's script, its object the spotter, until its first Set fills a slot (the Set takes no
/// then-script and ends the run). The script must reach its Set without waiting; a wait ends the run unfilled.
/// @param slot    the slot
/// @param script  the spotter's script
/// @param self    the spotter's local ID
/// @return true when the slot was filled
bool load_side(uint8_t slot, uint16_t script, uint8_t self)
{
   ENGINE_ASSERT(script < bank().script_count);
   Context c{};
   c.self = self;
   c.last_object = kNoSelf;
   push(c, raw(ScriptId{script}));
   const bool outer = g_immediate;   // nothing in it may wait (natives see immediate())
   g_immediate = true;
   g_load_slot = slot;
   for (int i = 0; i < kMaxRunSteps && c.depth != 0; ++i)
   {
      if (execute(c) == Outcome::Waiting)
      {
         break;   // it waits before its Set: the slot stays empty
      }
   }
   g_load_slot = kNotLoading;
   g_immediate = outer;
   return g_encounter.sides[slot].party != battles::kNoParty;
}

///@}

}   // namespace

/// The encounter record's current side's object (kObjectEncounter, kFaceEncounter).
/// @return its local ID, or kNoSelf
uint8_t encounter_object()
{
   return current_side().object;
}

}   // namespace detail

namespace battles
{

Side encounter_side(uint8_t side)
{
   const int slot = detail::slot_of(side);
   const detail::EncounterSide& s = detail::g_encounter.sides[slot < detail::kEncounterSides ? slot : detail::kSlotA];
   return Side{s.party, s.object};
}

uint8_t encounter_current_side()
{
   return detail::kSideCodes[detail::current_slot()];
}

uint8_t encounter_bits()
{
   return detail::g_encounter.bits;
}

uint8_t encounter_mode()
{
   return detail::g_encounter.mode;
}

}   // namespace battles

namespace detail
{

/// Encounter (C1): the encounter record.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_encounter(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   const auto kind = static_cast<EncounterOp>(*p++);
   switch (kind)
   {
   case EncounterOp::Clear:
      advance(f, p, base);
      clear_record();
      return Outcome::Continue;
   case EncounterOp::Set:
   {
      const uint8_t side = *p++;
      const bool loading = g_load_slot != kNotLoading;
      // C1b: under Load the side is the spotter's slot, whatever the Set names
      const int named = side == kEncounterSideCurrent ? current_slot() : slot_of(side);
      ENGINE_ASSERT(loading || named < kEncounterSides);
      const int slot = loading ? g_load_slot : named < kEncounterSides ? named : kSlotA;
      if (!loading && slot == kSlotA)
      {
         // side A starts a new encounter: the other sides of an earlier one go
         for (int i = kSlotA + 1; i < kEncounterSides; ++i)
         {
            g_encounter.sides[i] = empty_side();
         }
      }
      EncounterSide& s = g_encounter.sides[slot];
      read_side(c, p, s);
      uint16_t then = kNoScriptId;
      if ((s.parts & kEncounterHasThen) != 0)
      {
         then = load_le16(p);
         p += kU16Bytes;
      }
      const uint8_t bits = *p++;
      const uint8_t mode = *p++;
      if (loading)
      {
         // the spotter's side, its object the spotter; side A's mode is the encounter's; the run ends here
         s.object = c.self;
         if (slot == kSlotA)
         {
            g_encounter.bits = bits;
            g_encounter.mode = mode;
         }
         c.depth = 0;
         return Outcome::Ended;
      }
      g_encounter.bits = bits;
      g_encounter.mode = mode;
      advance(f, p, base);
      // the instruction after the Set is where Continue goes on
      g_encounter.back = f;
      g_encounter.has_back = true;
      if (then != kNoScriptId)
      {
         ENGINE_ASSERT(then < bank().script_count);
         f = Frame{then, 0};   // the frame goes on in the then-script (a jump)
      }
      return Outcome::Continue;
   }
   case EncounterOp::Select:
   {
      const int32_t side = operand(p);
      advance(f, p, base);
      const int slot = side >= 0 ? slot_of(static_cast<uint8_t>(side)) : kEncounterSides;
      g_encounter.current = static_cast<uint8_t>(slot < kEncounterSides ? slot : kSlotA);
      return Outcome::Continue;
   }
   case EncounterOp::Get:
   {
      const auto field = static_cast<EncounterField>(*p++);
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      advance(f, p, base);
      const EncounterSide& s = current_side();
      int32_t value = 0;
      switch (field)
      {
      case EncounterField::Party:
         value = s.party;
         break;
      case EncounterField::Object:
         value = s.object;
         break;
      case EncounterField::Beaten:
         value = s.beaten != kEncounterNoFlag && flag(FlagId{s.beaten}) ? 1 : 0;
         break;
      case EncounterField::Bits:
         value = g_encounter.bits;
         break;
      case EncounterField::Side:
         value = kSideCodes[current_slot()];
         break;
      case EncounterField::Mode:
         value = g_encounter.mode;
         break;
      case EncounterField::Sides:
         for (const EncounterSide& each : g_encounter.sides)
         {
            value += each.party != battles::kNoParty ? 1 : 0;
         }
         break;
      case EncounterField::NUM_ENCOUNTER_FIELDS:
         break;
      }
      set_var(VarId{v}, value);
      return Outcome::Continue;
   }
   case EncounterOp::Text:
   {
      const uint8_t which = *p++;
      const uint8_t flags = *p++;   // kMessage... bits, as MessageEx's
      advance(f, p, base);
      const EncounterSide& s = current_side();
      const int i = text_index(which);
      if (i >= kEncounterTexts || (s.parts & which) == 0)
      {
         return Outcome::Continue;   // the side has no such text: nothing to type
      }
      const uint8_t* ref = s.text[i];
      port::message_mode(flags);
      port::message(text_ref(ref));
      port::message_mode(0);
      if ((flags & kMessageNoWait) != 0)
      {
         return Outcome::Continue;   // the script goes on while it types (a later wait_msg waits)
      }
      c.wait = Wait::Typing;
      return Outcome::Waiting;
   }
   case EncounterOp::Music:
   {
      advance(f, p, base);
      const int32_t music = (g_encounter.bits & kEncounterNoMusic) != 0
                               ? battles::kNoMusic
                               : battles::encounter_music(current_side().party);
      if (music != battles::kNoMusic)
      {
         const int32_t args[] = {music, 0};   // the song, not kept as the map's
         audio::play(SoundOp::Bgm, args, static_cast<uint8_t>(std::size(args)));
      }
      return Outcome::Continue;
   }
   case EncounterOp::Mark:
      advance(f, p, base);
      for (const EncounterSide& s : g_encounter.sides)
      {
         if (s.party != battles::kNoParty && s.beaten != kEncounterNoFlag)
         {
            set_flag(FlagId{s.beaten}, true);
         }
      }
      return Outcome::Continue;
   case EncounterOp::Continue:
      return go_back(c, f);
   case EncounterOp::After:
   {
      const uint16_t after = current_side().after;
      advance(f, p, base);
      if (after == kNoScriptId)
      {
         return Outcome::Continue;   // no after-script: the next instruction runs
      }
      ENGINE_ASSERT(after < bank().script_count);
      f = Frame{after, 0};
      return Outcome::Continue;
   }
   case EncounterOp::Load:
   {
      advance(f, p, base);
      ENGINE_ASSERT(g_load_slot == kNotLoading);   // a spotter's script loads no group itself
      if (g_load_slot != kNotLoading)
      {
         return Outcome::Continue;
      }
      clear_record();
      uint8_t objects[kEncounterSides] = {};
      uint16_t scripts[kEncounterSides] = {};
      const uint8_t n = world::spotters(objects, scripts, kEncounterSides);
      uint8_t slot = kSlotA;
      for (uint8_t i = 0; i < n && slot < kEncounterSides; ++i)
      {
         // a spotter whose script reaches no Set leaves its slot to the next one
         slot = static_cast<uint8_t>(slot + (load_side(slot, scripts[i], objects[i]) ? 1 : 0));
      }
      if (slot < kEncounterSides)
      {
         g_encounter.sides[slot] = empty_side();   // a Set without a party (an empty side) left nothing
      }
      return Outcome::Continue;
   }
   case EncounterOp::Approach:
   {
      advance(f, p, base);
      const uint8_t obj = current_side().object;
      if (obj == kNoSelf || !world::approach(obj))
      {
         return Outcome::Continue;   // nothing walks up: nothing to wait for
      }
      c.wait = Wait::Approach;
      return Outcome::Waiting;
   }
   case EncounterOp::Next:
   {
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      advance(f, p, base);
      int next = current_slot() + 1;
      while (next < kEncounterSides && g_encounter.sides[next].party == battles::kNoParty)
      {
         ++next;
      }
      const bool more = next < kEncounterSides;
      g_encounter.current = static_cast<uint8_t>(more ? next : kSlotA);
      if (v != kNoVar)
      {
         set_var(VarId{v}, more ? 1 : 0);
      }
      return Outcome::Continue;
   }
   case EncounterOp::NUM_ENCOUNTER_OPS:
      break;
   }
   ENGINE_ASSERT(false && "bad encounter op");
   c.depth = 0;
   return Outcome::Ended;
}

/// Battle (C1): a battle through the game's battle port; waits for its outcome.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_battle(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   battles::Request r{};
   r.kind = static_cast<BattleKind>(*p++);
   if (r.kind == BattleKind::Unowned)
   {
      r.species = operand(p);
      r.level = operand(p);
      r.item = operand(p);
   }
   else if (r.kind == BattleKind::Scripted)
   {
      r.number = operand(p);
   }
   else
   {
      // a party battle takes the encounter record's sides
      const EncounterSide& a = g_encounter.sides[slot_of(kEncounterSideA)];
      const EncounterSide& b = g_encounter.sides[slot_of(kEncounterSideB)];
      const EncounterSide& sc = g_encounter.sides[slot_of(kEncounterSideC)];
      r.bits = g_encounter.bits;
      r.a = battles::Side{a.party, a.object};
      r.b = battles::Side{b.party, b.object};
      r.c = battles::Side{sc.party, sc.object};
   }
   c.result_var = load_le16(p);
   p += kU16Bytes;
   advance(f, p, base);
   if (!battles::start(r))
   {
      // no battle could start: it counts as won at once
      if (c.result_var != kNoVar)
      {
         set_var(VarId{c.result_var}, static_cast<int32_t>(battle::Outcome::Victory));
      }
      return Outcome::Continue;
   }
   c.wait = Wait::Battle;
   return Outcome::Waiting;
}

/// BattleSetup (C10: a module op): prepare an opponent, read the last outcome, ask about the current side's rematch.
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome op_battle_setup(Context& c, [[maybe_unused]] Op op, const uint8_t* p, const uint8_t* base)
{
   Frame& f = c.stack[c.depth - 1];
   // kind8, [Prepare: operand (species), operand (level), operand (item)], [Outcome, RematchDue, RematchReady: var16]
   const auto kind = static_cast<BattleSetupOp>(*p++);
   if (kind == BattleSetupOp::Prepare)
   {
      const int32_t species = operand(p);
      const int32_t level = operand(p);
      const int32_t item = operand(p);
      advance(f, p, base);
      battles::prepare(species, level, item);
      return Outcome::Continue;
   }
   const VarId v{load_le16(p)};
   p += kU16Bytes;
   advance(f, p, base);
   const int32_t value = kind == BattleSetupOp::Outcome     ? battles::outcome_code()
                         : kind == BattleSetupOp::RematchDue ? (battles::rematch_due() ? 1 : 0)
                                                              : (battles::rematch_ready() ? 1 : 0);
   set_var(v, value);
   return Outcome::Continue;
}

/// Wait::Battle (C1: a module wait, polled through the game's dispatch table).
/// @param c  the context
/// @return true while the battle runs (the result variable is written when it ends)
bool poll_battle(Context& c)
{
   if (battles::busy())
   {
      return true;
   }
   if (c.result_var != kNoVar)
   {
      set_var(VarId{c.result_var}, static_cast<int32_t>(battles::outcome()));
   }
   return false;
}

/// Wait::Approach (C1b: encounter approach, a module wait polled through the game's dispatch table).
/// @param c  the context
/// @return true while the walk-up runs
bool poll_approach([[maybe_unused]] Context& c)
{
   return world::approaching();
}

}   // namespace detail

bool immediate()
{
   return g_immediate;
}

Frame native_return_point()
{
   return g_native_after;
}

void native_goto(Frame to)
{
   g_native_to = to;
   g_native_goto = true;
}

void native_jump(ScriptId s)
{
   ENGINE_ASSERT(raw(s) < bank().script_count);
   native_goto(Frame{raw(s), 0});
}

bool flag(FlagId f)
{
   const uint16_t id = raw(f);
   const Lifetime l = bank_of_flag(id);
   const uint16_t bit = static_cast<uint16_t>(id - bank().flag_first[static_cast<int>(l)]);
   return (bank().flag_storage[static_cast<int>(l)][bit >> kByteOfBitShift] >> (bit & kBitInByteMask)) & 1;
}

void set_flag(FlagId f, bool on)
{
   const uint16_t id = raw(f);
   const Lifetime l = bank_of_flag(id);
   const uint16_t bit = static_cast<uint16_t>(id - bank().flag_first[static_cast<int>(l)]);
   uint8_t& byte = bank().flag_storage[static_cast<int>(l)][bit >> kByteOfBitShift];
   const auto mask = static_cast<uint8_t>(1u << (bit & kBitInByteMask));
   const auto was = static_cast<uint8_t>(byte);
   byte = static_cast<uint8_t>(on ? (byte | mask) : (byte & ~mask));
   if (byte != was)
   {
      touch(id);
   }
}

int32_t var(VarId v)
{
   return load(info(raw(v)));
}

void set_var(VarId v, int32_t value)
{
   const VarInfo& i = info(raw(v));
   const int32_t clamped = wraps(i) ? wrap_to(value, i.min, i.max) : clamp_to(value, i.min, i.max);
   if (load(i) == clamped)
   {
      return;
   }
   store(i, clamped);
   touch(static_cast<uint16_t>(kDepVar | raw(v)));
}

void add_var(VarId v, int32_t delta)
{
   const VarInfo& i = info(raw(v));
   if (wraps(i))
   {
      // The sum wraps around the range (computed wide, so it never overflows first)
      set_var(v, wrap_to(static_cast<int64_t>(load(i)) + delta, i.min, i.max));
      return;
   }
   set_var(v, sat_add(load(i), delta, i.min, i.max));
}

Lifetime lifetime_of(FlagId f)
{
   return bank_of_flag(raw(f));
}

void clear(Lifetime l)
{
   const Bank& b = bank();
   const auto i = static_cast<int>(l);
   for (uint16_t n = 0; n < b.flag_bytes[i]; ++n)
   {
      b.flag_storage[i][n] = 0;
   }
   // A variable whose range excludes 0 starts at its minimum.
   for (uint16_t n = 0; n < b.var_bytes[i]; ++n)
   {
      b.var_storage[i][n] = 0;
   }
   for (uint16_t v = 0; v < b.var_count; ++v)
   {
      if (b.vars[v].lifetime == l && (b.vars[v].min > 0 || b.vars[v].max < 0))
      {
         store(b.vars[v], b.vars[v].min > 0 ? b.vars[v].min : b.vars[v].max);
      }
   }
   g_armed.dirty = static_cast<uint16_t>((1u << g_armed.count) - 1);   // anything may have changed
   ++g_state;
}

void clear_all()
{
   for (int l = 0; l < kNumLifetimes; ++l)
   {
      clear(static_cast<Lifetime>(l));
   }
   // Name slots start empty: a length byte of 0 each (the whole storage zeroed).
   for (uint16_t n = 0; n < bank().name_bytes; ++n)
   {
      bank().names[n] = 0;
   }
}

void new_day()
{
   clear(Lifetime::Daily);
}

void change_map(uint16_t from_area, uint16_t to_area)
{
   clear(Lifetime::Map);
   if (from_area != to_area)
   {
      clear(Lifetime::Area);
   }
}

uint32_t state_version()
{
   return g_state;
}

void bump_state()
{
   ++g_state;
   g_armed.dirty = static_cast<uint16_t>((1u << g_armed.count) - 1);
}

uint32_t save_size()
{
   uint32_t n = 0;
   for (int l = 0; l < kNumLifetimes; ++l)
   {
      n += bank().flag_bytes[l] + bank().var_bytes[l];
   }
   return n + bank().name_bytes;
}

namespace
{
/// Writes a value in decimal.
/// @param v         the value (0 or more)
/// @param out       codes
/// @param capacity  most codes
/// @return codes written
int decimal(int32_t v, text::Code* out, int capacity)
{
   text::Code digits[kMaxDecimalDigits];
   int count = 0;
   do
   {
      digits[count++] = static_cast<text::Code>(text::kCodeDigitZero + v % kDecimalBase);
      v /= kDecimalBase;
   }
   while (v > 0 && count < kMaxDecimalDigits);
   int n = 0;
   while (count > 0 && n < capacity)
   {
      out[n++] = digits[--count];
   }
   return n;
}
} // namespace

const Placeholder* placeholder(uint8_t index)
{
   return index < bank().placeholder_count ? &bank().placeholders[index] : nullptr;
}

int placeholder_text(uint8_t index, text::Code* out, int capacity)
{
   const Placeholder* p = placeholder(index);
   if (p == nullptr || capacity <= 0)
   {
      return 0;
   }
   int n = 0;
   switch (p->kind)
   {
   case PlaceholderKind::Name:
   {
      // A slot is a length byte, then its codes.
      const uint8_t* slot = bank().names + p->text;
      for (; n < slot[0] && n < p->param && n < capacity; ++n)
      {
         out[n] = slot[kNameLengthBytes + n];
      }
      break;
   }
   case PlaceholderKind::Var:
      // Variables are never negative (their declared ranges start at 0 or above).
      n = decimal(var(VarId{p->index}), out, capacity);
      break;
   case PlaceholderKind::Term:
      for (const text::Code* t = bank().text + p->text; *t != text::kCodeEnd && n < capacity; ++t)
      {
         out[n++] = *t;
      }
      break;
   case PlaceholderKind::Record:
   {
      const uint8_t role = static_cast<uint8_t>(p->index);
      if (p->text == 0)
      {   // a number (role values are never negative)
         n = decimal(party::role_get(p->param, role, 0), out, capacity);
         break;
      }
      // a text role: its codes up to the first end code (or 0)
      for (uint32_t e = 0; e < p->text && n < capacity; ++e)
      {
         const int32_t code = party::role_get(p->param, role, static_cast<uint8_t>(e));
         if (code == 0 || code == text::kCodeEnd)
         {
            break;
         }
         out[n++] = static_cast<text::Code>(code);
      }
      break;
   }
   case PlaceholderKind::NUM_PLACEHOLDER_KINDS:
      break;
   }
   return n;
}

void set_name(uint8_t index, const text::Code* codes)
{
   const Placeholder* p = placeholder(index);
   ENGINE_ASSERT(p != nullptr && p->kind == PlaceholderKind::Name && codes != nullptr);
   if (p == nullptr || p->kind != PlaceholderKind::Name || codes == nullptr)
   {
      return;
   }
   uint8_t* slot = bank().names + p->text;
   uint8_t n = 0;
   for (; n < p->param && codes[n] != text::kCodeEnd; ++n)
   {
      slot[kNameLengthBytes + n] = codes[n];
   }
   slot[0] = n;
   bump_state();   // text that shows a name may be chosen again
}

void start(Context& c, ScriptId s, uint8_t self)
{
   c = Context{};
   c.self = self;
   c.last_object = kNoSelf;
   push(c, raw(s));
}

void frame()
{
   port::tick();
}

bool update(Context& c)
{
   if (c.depth == 0)
   {
      return false;
   }
   const Wait was = c.wait;
   if (still_waiting(c))
   {
      return true;
   }
   if (was != Wait::None && was != Wait::Native && (bank().vm_flags & kVmResumeNextFrame) != 0)
   {
      return true;   // the wait ended this frame; the next instruction runs on the next one
   }
   g_native_rerun = was == Wait::Native;
   for (int i = 0; i < kOpsPerFrame && c.depth != 0; ++i)
   {
      const Outcome o = execute(c);
      g_native_rerun = false;
      if (o == Outcome::Waiting)
      {
            // A message that is already typed, or a delay of one frame, still
            // waits this frame: waits are checked from the next update().
         return true;
      }
      if (o == Outcome::True)
      {
         break;
      }   // TriggerTrue outside a condition: ends
   }
   return c.depth != 0;
}

void stop(Context& c)
{
   c = Context{};
}

void run_now(ScriptId s)
{
   run_to_end(s);
}

namespace
{

/// A map's triggers.
/// @param map  the map's asset key
/// @return its entry, or nullptr when it has no scripts
const MapScripts* find_map(AssetKey map)
{
   const Bank& b = bank();
   // Binary search by key.
   uint16_t lo = 0;
   uint16_t hi = b.map_count;
   while (lo < hi)
   {
      const auto mid = static_cast<uint16_t>((lo + hi) / kHalf);
      if (raw(b.maps[mid].map) < raw(map))
      {
         lo = static_cast<uint16_t>(mid + 1);
      }
      else
      {
         hi = mid;
      }
   }
   return lo < b.map_count && b.maps[lo].map == map ? &b.maps[lo] : nullptr;
}

} // namespace

bool run_map_scripts(AssetKey map, TriggerKind kind)
{
   const MapScripts* ms = find_map(map);
   if (ms == nullptr)
   {
      return false;
   }
   const Bank& b = bank();
   bool ran = false;
   for (uint8_t i = 0; i < ms->trigger_count; ++i)
   {
      const MapTrigger& t = b.triggers[ms->trigger_first + i];
      if (t.kind != kind)
      {
         continue;
      }
      if (kind == TriggerKind::OnWarpInto)
      {
         // the first entry whose condition holds is the one that runs
         if (!run_to_end(ScriptId{t.condition}))
         {
            continue;
         }
         run_to_end(ScriptId{t.script});
         return true;
      }
      run_to_end(ScriptId{t.script});
      ran = true;
   }
   return ran;
}

namespace
{
/// enter_map() and enter_saved_map(): the run-once kinds from @p first_kind on, then the on_frame triggers.
/// @param map         the map's asset key
/// @param first_kind  the first kind run (on_transition: all of them; on_load: a saved game's)
void enter_map_from(AssetKey map, int first_kind)
{
   g_armed = Armed{};
   const Bank& b = bank();
   const MapScripts* found = find_map(map);
   if (found == nullptr)
   {
      return;
   }
   const MapScripts& ms = *found;
   for (int kind = first_kind; kind < kRunOnceKinds; ++kind)
   {   // on_transition, then on_load
      for (uint8_t i = 0; i < ms.trigger_count; ++i)
      {
         const MapTrigger& t = b.triggers[ms.trigger_first + i];
         if (static_cast<int>(t.kind) == kind)
         {
            run_to_end(ScriptId{t.script});
         }
      }
   }
   for (uint8_t i = 0; i < ms.trigger_count && g_armed.count < kMaxTriggers; ++i)
   {
      const MapTrigger& t = b.triggers[ms.trigger_first + i];
      if (t.kind == TriggerKind::OnFrame)
      {
         g_armed.trigger[g_armed.count++] = &t;
      }
   }
   g_armed.dirty = static_cast<uint16_t>((1u << g_armed.count) - 1);
}
} // namespace

void enter_map(AssetKey map)
{
   enter_map_from(map, static_cast<int>(TriggerKind::OnTransition));
}

void enter_saved_map(AssetKey map)
{
   enter_map_from(map, static_cast<int>(TriggerKind::OnLoad));
}

bool poll_triggers(ScriptId& out)
{
   for (uint8_t i = 0; i < g_armed.count; ++i)
   {
      if (!(g_armed.dirty & (1u << i)))
      {
         continue;
      }
      const MapTrigger& t = *g_armed.trigger[i];
      if (run_to_end(ScriptId{t.condition}))
      {   // true: stays armed (fires again until its state changes)
         out = ScriptId{t.script};
         return true;
      }
      g_armed.dirty = static_cast<uint16_t>(g_armed.dirty & ~(1u << i));
   }
   return false;
}

void disarm_triggers()
{
   g_armed = Armed{};
}

} // namespace capsule::script
