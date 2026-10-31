/// @file
/// Script VM, flags, variables and map triggers (see capsule/script/script.h).
#include "capsule/script/script.h"

#include "capsule/asset/asset.h"
#include "capsule/core/assert.h"
#include "capsule/core/clock.h"
#include "capsule/core/endian.h"
#include "capsule/core/random.h"
#include "capsule/core/saturate.h"
#include "capsule/map/map.h"
#include "capsule/script/combine_port.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/growth_port.h"
#include "capsule/script/nursery_port.h"
#include "capsule/script/port.h"

namespace capsule::script
{
namespace
{

/// @name Bytecode layout
///@{
constexpr int kU16Bytes = 2;            ///< A 16-bit operand (IDs, targets, lengths).
constexpr int kU32Bytes = 4;            ///< A 32-bit operand (constants, asset keys, text offsets).
constexpr int kQueryBytes = 2;          ///< A query operand: query byte and object byte.
constexpr int kFlagBranchBytes = 4;     ///< IfFlagSet / IfFlagClear: flag ID and target.
constexpr int kChoiceHeaderBytes = 4;   ///< Choice: result variable, cancel index, option count.
constexpr int kSelfSwitchBytes = 2;     ///< SelfSwitch: letter and on.
constexpr int kWarpTailBytes = 2;       ///< Warp: facing and transition, after the position.
constexpr int kFadeKindAt = 0;          ///< I4: Fade: offset of its kind.
constexpr int kFadeSpeedAt = 1;         ///< I4: Fade: offset of its speed.
constexpr int kFadeFlagsAt = 2;         ///< I4: Fade: offset of its flags.
constexpr int kFadeBytes = 3;           ///< I4: Fade: kind, speed and flags.
constexpr int kOverrideBytes = 3;       ///< DS1: Override: apply and the set (16 bits).
constexpr int kSoundHeaderBytes = 2;    ///< I4: Sound: kind and operand count.
constexpr int kMaxSoundArgs = 2;        ///< I4: most operands of a Sound (a song and its save, a cry and its mode).
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
constexpr int kBitsPerByte = 8;               ///< Bits of a variable's storage byte (C10: its type's range).
constexpr uint8_t kDirectionMask = 3;         ///< Direction bits of ObjectMove's direction byte.
/// C10: a direction's opposite differs in its lowest bit (0 down / 1 up, 2 left / 3 right).
constexpr uint8_t kOppositeDirectionBit = 1;
/// C10: draws of `random` in the modulo form are 16-bit (the generator's next16()).
constexpr uint32_t kRandomDrawMask = 0xFFFF;
constexpr uint8_t kSelfSwitchMask = 3;        ///< Self switch letter bits (A-D).
constexpr int32_t kMaxSteps = 0xFFFF;         ///< Most steps of one ObjectMove.
constexpr int32_t kMaxScrollCells = 255;      ///< Most cells of one Scroll.
constexpr int32_t kMaxItemId = 0xFFFF;        ///< Largest item id an item operand names (G2; 16-bit ids).
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

/// C10: a value wrapped into a range (kVmVarsWrap): its distance from the range's start modulo the range's span, so a
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

/// C10: whether a variable wraps (kVmVarsWrap): with the bank's switch, a variable declared with its type's whole
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
/// C10: update() re-runs a native that waited (its next instruction may wait for the next frame).
bool g_native_rerun = false;
/// N1: the statement after the native being run (native_return_point()).
Frame g_native_after{};
/// N1: where the frame goes on after the native being run (native_goto()), when g_native_goto is set.
Frame g_native_to{};
/// N1: the native being run asked to go on at g_native_to.
bool g_native_goto = false;

/// C10: `random(n)` in the modulo form (kVmRandomModulo): a 16-bit draw modulo n as a 16-bit value, drawn even for
/// n = 0 (then 0: a software modulo by zero returns 0, as the reference game's division routine does).
/// @param n  the bound operand (taken as 16 bits, as the game's variables are)
/// @return the draw modulo n
int32_t random_modulo(int32_t n)
{
   const uint32_t draw = rng::next16(rng::gameplay);
   const uint32_t bound = static_cast<uint32_t>(n) & kRandomDrawMask;
   return bound == 0 ? 0 : static_cast<int32_t>(draw % bound);
}

/// I4: a numbered object operand as a local ID (the bank's numbering may count from object_base).
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
      return c.last_object;   // I4: the object the context last gave a route
   }
   if (v == kObjectSelected)
   {
      return c.self != kNoSelf ? c.self : kObjectPlayer;   // C10: the selected object (the walker without one)
   }
   return local_of(v);
}

/// What one call to execute() ended with.
enum class Outcome : uint8_t
{
   Continue,   ///< More instructions.
   Waiting,    ///< The context waits.
   Ended,      ///< The context has no more frames.
   True,       ///< TriggerTrue (conditions).
};

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

/// Executes one I4 instruction (D-29: what imported event scripts need).
/// @param c     the context
/// @param op    the opcode
/// @param p     the position after the opcode
/// @param base  the running script's first byte
/// @return what happened
Outcome execute_i4(Context& c, Op op, const uint8_t* p, const uint8_t* base)
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
   case Op::Fade:
   {
      const auto kind = static_cast<FadeOp>(p[kFadeKindAt]);
      const uint8_t speed = p[kFadeSpeedAt];
      const uint8_t flags = p[kFadeFlagsAt];
      p += kFadeBytes;
      world::fade(kind, speed, flags);
      return wait(Wait::Fade);
   }
   case Op::Override:
   {
      // DS1: apply8, set16 (map::kWorld.override_sets)
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
   case Op::Mart:
   {
      const uint8_t kind = *p++;
      const uint16_t n = load_le16(p);
      p += kU16Bytes;
      const uint8_t* items = p;
      p += kU16Bytes * n;
      items::mart_open(kind, items, n);
      return wait(Wait::Mart);
   }
   case Op::WaitState:
      return wait(Wait::State);
   default:
      break;
   }
   ENGINE_ASSERT(false && "bad I4 opcode");
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
      // I4: the frame goes on in another script (nothing returns here)
      const uint16_t s = load_le16(p);
      ENGINE_ASSERT(s < bank().script_count);
      f = Frame{s, 0};
      return Outcome::Continue;
   }
   case Op::CallLoose:
   {
      // I4: a call that may recurse; on a full stack it goes on as a jump (the imported game's VM does so)
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
      // N1: the native may ask where the script goes on (native_return_point(), native_goto())
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
      // C10: a native that waited and is now done ends the frame when the bank resumes on the next one
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
         // C10: the walker's facing turned around (down <-> up, left <-> right), whatever the geometry
         const auto facing = static_cast<uint8_t>(world::query(Query::Facing, kObjectPlayer) & kDirectionMask);
         world::face(obj, static_cast<uint8_t>(facing ^ kOppositeDirectionBit));
      }
      else
      {
         const uint8_t other = face == kFacePlayer ? kObjectPlayer : c.self;
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
      c.last_object = obj;   // I4: kObjectLast names it
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
         return Outcome::Continue;   // I4: the script goes on while the warp starts
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
   case Op::RoleGet:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const uint8_t slot = byte_index(operand(p));
      const uint8_t role = *p++;
      const uint8_t element = byte_index(operand(p));
      next();
      set_var(v, party::role_get(slot, role, element));   // clamps to the variable's range
      return Outcome::Continue;
   }
   case Op::RoleSet:
   {
      const uint8_t slot = byte_index(operand(p));
      const uint8_t role = *p++;
      const uint8_t element = byte_index(operand(p));
      const int32_t value = operand(p);
      next();
      party::role_set(slot, role, element, value);
      return Outcome::Continue;
   }
   case Op::ItemGive:
   case Op::ItemTake:
   {
      // G2: the bag through the items port (all or nothing); the result variable gets 1 when it worked
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      const int32_t item = operand(p);
      const int32_t n = operand(p);
      next();
      const auto id = static_cast<uint16_t>(clamp_to<int32_t>(item, 0, kMaxItemId));
      const bool ok = op == Op::ItemGive ? items::give(id, n) : items::take(id, n);
      if (v != kNoVar)
      {
         set_var(VarId{v}, ok ? 1 : 0);
      }
      return Outcome::Continue;
   }
   case Op::ItemCount:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      const int32_t item = operand(p);
      next();
      set_var(v, items::count(static_cast<uint16_t>(clamp_to<int32_t>(item, 0, kMaxItemId))));
      return Outcome::Continue;
   }
   case Op::BagOpen:
      c.result_var = load_le16(p);
      p += kU16Bytes;
      next();
      items::bag_open();
      c.wait = Wait::Bag;
      return Outcome::Waiting;
   case Op::Shop:
   {
      const uint8_t shop = *p++;
      const VarId money{load_le16(p)};
      p += kU16Bytes;
      next();
      items::shop_open(shop, money);
      c.wait = Wait::Shop;
      return Outcome::Waiting;
   }
   case Op::TimeOfDay:
   {
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      next();
      set_var(v, clock::time_of_day());
      return Outcome::Continue;
   }
   case Op::BoxCount:
   {
      // G3: the storage boxes through the boxes port
      const VarId v{load_le16(p)};
      p += kU16Bytes;
      next();
      set_var(v, boxes::count());
      return Outcome::Continue;
   }
   case Op::BoxDeposit:
   case Op::BoxWithdraw:
   {
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      const int32_t at = operand(p);
      next();
      const bool ok = op == Op::BoxDeposit ? boxes::deposit(at) : boxes::withdraw(at);
      if (v != kNoVar)
      {
         set_var(VarId{v}, ok ? 1 : 0);
      }
      return Outcome::Continue;
   }
   case Op::BoxesOpen:
      c.result_var = load_le16(p);
      p += kU16Bytes;
      next();
      boxes::open();
      c.wait = Wait::Boxes;
      return Outcome::Waiting;
   case Op::Transform:
   {
      // G4: a party member's transformation (the script trigger's rules) through the growth port; waits
      c.result_var = load_le16(p);
      p += kU16Bytes;
      const int32_t slot = operand(p);
      next();
      // a slot past the party is refused by the port (nothing changes)
      growth::transform_open(static_cast<uint8_t>(clamp_to<int32_t>(slot, 0, monster::kMaxPartySize)));
      c.wait = Wait::Transform;
      return Outcome::Waiting;
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
      // F2: the shown map's darkness level until the next map
      const int32_t level = operand(p);
      next();
      world::set_darkness(static_cast<uint8_t>(clamp_to<int32_t>(level, 0, map::kMaxDarkness)));
      return Outcome::Continue;
   }
   case Op::Flight:
   {
      // F2: record, visit or pick a flight point (capsule/script/field_actions.h)
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
      // F2: warp to the escape point the overworld recorded
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
      // F2: the first party member a field action is available to, plus 1 (0: none)
      p++;   // kind: FieldActionOp::Available, the only one
      const uint8_t action = *p++;
      const uint16_t v = load_le16(p);
      p += kU16Bytes;
      next();
      const uint8_t member = actions::available(action);
      set_var(VarId{v}, member == actions::kNoMember ? 0 : int32_t{member} + 1);
      return Outcome::Continue;
   }
   case Op::Combine:
   {
      // B1: a combination method through the combine port (Open / Run wait; the queries answer at once)
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
      next();
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
   case Op::Nursery:
   {
      // B2: the nursery and the nickname prompt through the nursery port (Pick / Name wait; the rest answer at once)
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
      next();
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
   case Op::ObjectMoveChecked:
   {
      // C8: the walk by the movement rules: every step allowed, or nothing moves
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
   case Op::SetMetatile:
   case Op::Respawn:
   case Op::SetWarp:
   case Op::Sound:
   case Op::WaitSound:
   case Op::Mart:
   case Op::WaitState:
   case Op::Override:
      return execute_i4(c, op, p, base);
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
   case Wait::Bag:
   {
      const int32_t item = items::bag_poll();
      if (item < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, item);
      break;
   }
   case Wait::Shop:
      if (items::shop_poll())
      {
         return true;
      }
      break;
   case Wait::Boxes:
   {
      const int32_t box = boxes::poll();
      if (box < 0)
      {
         return true;
      }
      set_var(VarId{c.result_var}, box);
      break;
   }
   case Wait::Flight:
   {
      // F2: the flight menu's answer; a point picked warps there (then the warp's wait)
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
   case Wait::Transform:
   {
      const int16_t result = growth::transform_poll();
      if (result == growth::kTransformRunning)
      {
         return true;
      }
      set_var(VarId{c.result_var}, result);
      break;
   }
   case Wait::Combine:
   {
      const int16_t result = combine::poll();
      if (result == combine::kCombineRunning)
      {
         return true;
      }
      set_var(VarId{c.result_var}, result);
      break;
   }
   case Wait::Nursery:
   {
      const int16_t result = nursery::poll();
      if (result == nursery::kNurseryRunning)
      {
         return true;
      }
      set_var(VarId{c.result_var}, result);
      break;
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
   case Wait::Sound:
      if (audio::playing(static_cast<SoundWait>(c.result_var)))
      {
         return true;
      }
      break;
   case Wait::Mart:
      if (items::mart_poll())
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

bool g_immediate = false;   ///< I4: run_to_end() runs (immediate()).

/// Runs a context that must not wait until it ends (immediate() is true meanwhile).
/// @param s  the script
/// @return true when it ended with TriggerTrue
bool run_to_end(ScriptId s)
{
   // I4: immediate() is true meanwhile (nested runs keep it so)
   const bool outer = g_immediate;
   g_immediate = true;
   const bool result = run_to_end_body(s);
   g_immediate = outer;
   return result;
}

} // namespace

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
      // C10: the sum wraps around the range (computed wide, so it never overflows first)
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
      return true;   // C10: the wait ended this frame; the next instruction runs on the next one
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
