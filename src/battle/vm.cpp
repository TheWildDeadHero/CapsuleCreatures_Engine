/// @file
/// The battle script machine (capsule/battle/vm.h): bank reading and the interpreter loop.
#include "capsule/battle/vm.h"

namespace capsule::battle::vm
{
namespace
{

/// @name Header field offsets
///@{
constexpr uint32_t kMagicAt = 0;          ///< magic (u32)
constexpr uint32_t kVersionAt = 4;        ///< version (u16)
constexpr uint32_t kLabelCountAt = 6;     ///< label count (u16)
constexpr uint32_t kCodeOffsetAt = 8;     ///< code offset (u32)
constexpr uint32_t kCodeSizeAt = 12;      ///< code size (u32)
constexpr uint32_t kLabelsAt = 16;        ///< label table offset (u32)
constexpr uint32_t kNamesAt = 20;         ///< name table offset (u32)
///@}

constexpr uint32_t kWordBytes = 4;        ///< Bytes of a u32.
constexpr uint32_t kHalfBytes = 2;        ///< Bytes of a u16.
constexpr int kByteBits = 8;              ///< Bits of a byte.

/// A little-endian u16 at a pointer (unaligned).
/// @param p  the bytes
/// @return the value
uint16_t read16(const uint8_t* p)
{
   return static_cast<uint16_t>(p[0] | (p[1] << kByteBits));
}

/// A little-endian u32 at a pointer (unaligned).
/// @param p  the bytes
/// @return the value
uint32_t read32(const uint8_t* p)
{
   uint32_t v = 0;
   // the highest byte first, each shifted down a byte by the next
   for (uint32_t i = kWordBytes; i > 0; --i)
   {
      v = (v << kByteBits) | p[i - 1];
   }
   return v;
}

/// Whether [@p offset, @p offset + @p bytes) lies inside @p size bytes (no overflow).
/// @param offset  the start
/// @param bytes   the length
/// @param size    the room
/// @return true when it does
bool inside(uint32_t offset, uint32_t bytes, uint32_t size)
{
   return offset <= size && bytes <= size - offset;
}

/// Faults the machine.
/// @param m    the machine
/// @param why  the fault
void fail(Machine& m, Fault why)
{
   m.status = Status::Fault;
   m.fault = why;
}

/// The code bytes of an operand, or nullptr (a fault) outside the code.
/// @param m      the machine
/// @param at     bytes after the position
/// @param bytes  the operand's size
/// @return the bytes
const uint8_t* operand(Machine& m, uint32_t at, uint32_t bytes)
{
   const Bank& b = *m.bank;
   if (m.ip > b.code_bytes || !inside(m.ip, at, b.code_bytes) || !inside(m.ip + at, bytes, b.code_bytes))
   {
      fail(m, Fault::OutOfCode);
      return nullptr;
   }
   return b.code + m.ip + at;
}

} // namespace

BankError open(Bank& bank, const uint8_t* data, uint32_t size)
{
   bank = Bank{};
   if (data == nullptr || size < kHeaderBytes)
   {
      return BankError::TooSmall;
   }
   if (read32(data + kMagicAt) != kBankMagic)
   {
      return BankError::BadMagic;
   }
   if (read16(data + kVersionAt) != kBankVersion)
   {
      return BankError::BadVersion;
   }
   const uint16_t labels = read16(data + kLabelCountAt);
   const uint32_t code = read32(data + kCodeOffsetAt);
   const uint32_t code_bytes = read32(data + kCodeSizeAt);
   const uint32_t label_table = read32(data + kLabelsAt);
   const uint32_t names = read32(data + kNamesAt);
   const uint32_t table_bytes = static_cast<uint32_t>(labels) * kWordBytes;
   if (!inside(code, code_bytes, size) || !inside(label_table, table_bytes, size) ||
       (names != 0 && !inside(names, table_bytes, size)))
   {
      return BankError::BadLayout;
   }
   // every label inside the code (its end included: a label may close the code), every name inside the bank
   for (uint16_t i = 0; i < labels; ++i)
   {
      if (read32(data + label_table + i * kWordBytes) > code_bytes)
      {
         return BankError::BadLayout;
      }
      if (names != 0 && read32(data + names + i * kWordBytes) >= size)
      {
         return BankError::BadLayout;
      }
   }
   bank.data = data;
   bank.size = size;
   bank.code = data + code;
   bank.code_bytes = code_bytes;
   bank.labels = data + label_table;
   bank.names = names != 0 ? data + names : nullptr;
   bank.label_count = labels;
   return BankError::None;
}

uint32_t label(const Bank& bank, uint16_t id)
{
   return id < bank.label_count ? read32(bank.labels + id * kWordBytes) : 0;
}

const char* label_name(const Bank& bank, uint16_t id)
{
   if (bank.names == nullptr || id >= bank.label_count)
   {
      return nullptr;
   }
   return reinterpret_cast<const char*>(bank.data + read32(bank.names + id * kWordBytes));
}

uint16_t label_at(const Bank& bank, uint32_t offset)
{
   for (uint16_t i = 0; i < bank.label_count; ++i)
   {
      if (label(bank, i) == offset)
      {
         return i;
      }
   }
   return kNoLabel;
}

uint16_t label_before(const Bank& bank, uint32_t offset)
{
   uint16_t best = kNoLabel;
   uint32_t best_at = 0;
   for (uint16_t i = 0; i < bank.label_count; ++i)
   {
      const uint32_t at = label(bank, i);
      // the nearest at or before the offset; the lowest id among equals
      if (at <= offset && (best == kNoLabel || at > best_at))
      {
         best = i;
         best_at = at;
      }
   }
   return best;
}

void reset(Machine& m, const Bank& bank)
{
   m = Machine{};
   m.bank = &bank;
   m.status = Status::Idle;
   m.fault = Fault::None;
   m.wait_mask = ~0u;
}

void wait_on(Machine& m, const uint32_t* flags, uint32_t mask)
{
   m.waits_on = flags;
   m.wait_mask = mask;
}

void start(Machine& m, uint32_t offset)
{
   m.ip = offset;
   m.depth = 0;
   m.pause_frames = 0;
   m.yielded = false;
   m.fault = Fault::None;
   m.status = Status::Running;
   if (offset > m.bank->code_bytes)
   {
      fail(m, Fault::OutOfCode);
   }
}

void start_label(Machine& m, uint16_t id)
{
   start(m, label(*m.bank, id));
}

void stop(Machine& m)
{
   m.status = Status::Idle;
}

bool blocked(const Machine& m)
{
   return m.waits_on != nullptr && (*m.waits_on & m.wait_mask) != 0;
}

Result step(Machine& m, const CommandTable& table, void* context)
{
   if (m.status != Status::Running)
   {
      return Result::Stopped;
   }
   if (blocked(m))
   {
      return Result::Blocked;
   }
   const uint8_t* at = operand(m, 0, 1);
   if (at == nullptr)
   {
      return Result::Stopped;
   }
   const uint8_t id = *at;
   m.yielded = false;
   if (table.trace != nullptr)
   {
      table.trace(m, id, context);
   }
   if (id < table.count && table.commands[id] != nullptr)
   {
      table.commands[id](m, context);
   }
   else if (table.unknown != nullptr)
   {
      table.unknown(m, context);
   }
   else
   {
      fail(m, Fault::NoCommand);
   }
   return Result::Ran;
}

uint16_t run(Machine& m, const CommandTable& table, void* context, uint16_t limit)
{
   uint16_t ran = 0;
   while (ran < limit && step(m, table, context) == Result::Ran)
   {
      ++ran;
      if (m.yielded)
      {
         break;
      }
   }
   return ran;
}

uint8_t u8_at(Machine& m, uint32_t at)
{
   const uint8_t* p = operand(m, at, 1);
   return p != nullptr ? *p : 0;
}

int8_t s8_at(Machine& m, uint32_t at)
{
   return static_cast<int8_t>(u8_at(m, at));
}

uint16_t u16_at(Machine& m, uint32_t at)
{
   const uint8_t* p = operand(m, at, kHalfBytes);
   return p != nullptr ? read16(p) : 0;
}

uint32_t u32_at(Machine& m, uint32_t at)
{
   const uint8_t* p = operand(m, at, kWordBytes);
   return p != nullptr ? read32(p) : 0;
}

Ref ref_at(Machine& m, uint32_t at)
{
   return u32_at(m, at);
}

const uint8_t* code_bytes(Machine& m, uint32_t offset, uint32_t bytes)
{
   if (!inside(offset, bytes, m.bank->code_bytes))
   {
      fail(m, Fault::OutOfCode);
      return nullptr;
   }
   return m.bank->code + offset;
}

void advance(Machine& m, uint32_t bytes)
{
   jump(m, m.ip + bytes);
}

void jump(Machine& m, uint32_t offset)
{
   if (offset > m.bank->code_bytes)
   {
      fail(m, Fault::OutOfCode);
      return;
   }
   m.ip = offset;
}

void call(Machine& m, uint32_t offset, uint32_t resume)
{
   push(m, resume);
   if (m.status == Status::Running)
   {
      jump(m, offset);
   }
}

void push(Machine& m, uint32_t offset)
{
   if (m.depth >= kStackDepth)
   {
      fail(m, Fault::StackOverflow);
      return;
   }
   m.stack[m.depth++] = offset;
}

uint32_t pop(Machine& m)
{
   if (m.depth == 0)
   {
      fail(m, Fault::StackUnderflow);
      return 0;
   }
   return m.stack[--m.depth];
}

void ret(Machine& m)
{
   const uint32_t to = pop(m);
   if (m.status == Status::Running)
   {
      jump(m, to);
   }
}

void end(Machine& m)
{
   m.status = Status::Ended;
}

void yield(Machine& m)
{
   m.yielded = true;
}

bool pause(Machine& m, uint16_t frames)
{
   // the counter counts this call first, then compares (a wait of n frames is over at its n-th call)
   ++m.pause_frames;
   if (m.pause_frames >= frames)
   {
      m.pause_frames = 0;
      return true;
   }
   return false;
}

}   // namespace capsule::battle::vm
