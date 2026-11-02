/// @file
/// Animation script banks: reading and checking (see capsule/anim/bank.h).
#include "capsule/anim/bank.h"

#include "capsule/core/endian.h"

namespace capsule::anim
{
namespace
{

constexpr uint32_t kBitsPerWord = 32;   ///< Boundary map: bits per word.
constexpr uint32_t kBitShift = 5;       ///< Boundary map: byte to word, b >> 5.
constexpr uint32_t kBitMask = 31;       ///< Boundary map: byte to bit, b & 31.

/// Whether a section fits in the bank and is aligned.
/// @param at     its offset
/// @param bytes  its size
/// @param size   the bank's size
/// @return true when it does
bool section_ok(uint32_t at, uint32_t bytes, uint32_t size)
{
   return at % kSectionAlign == 0 && at <= size && bytes <= size - at;
}

/// A string by offset.
/// @param b       the bank
/// @param offset  its offset, or kNoName
/// @return the string, or "" when it is none or out of the section
const char* string_at(const Bank& b, uint32_t offset)
{
   if (offset == kNoName || offset >= b.strings_size)
   {
      return "";
   }
   return b.strings + offset;
}

/// Whether a string offset names a NUL-terminated string inside the section (or nothing).
/// @param b       the bank
/// @param offset  the offset
/// @return true when it does
bool string_ok(const Bank& b, uint32_t offset)
{
   if (offset == kNoName)
   {
      return true;
   }
   for (uint32_t i = offset; i < b.strings_size; ++i)
   {
      if (b.strings[i] == '\0')
      {
         return true;
      }
   }
   return false;
}

/// Two strings equal.
/// @param a  one
/// @param c  the other
/// @return true when equal
bool same(const char* a, const char* c)
{
   while (*a != '\0' && *a == *c)
   {
      ++a;
      ++c;
   }
   return *a == *c;
}

/// The opcode entry of a game opcode.
/// @param b       the bank
/// @param opcode  the opcode
/// @return its entry, or nullptr
const uint8_t* opcode_entry(const Bank& b, uint8_t opcode)
{
   for (uint16_t i = 0; i < b.opcode_count; ++i)
   {
      const uint8_t* e = b.opcodes + i * kOpcodeBytes;
      if (e[0] == opcode)
      {
         return e;
      }
   }
   return nullptr;
}

/// Marks a code byte as a command's first byte.
/// @param bits  the map
/// @param at    the byte
void mark(uint32_t* bits, uint32_t at)
{
   bits[at >> kBitShift] |= uint32_t{1} << (at & kBitMask);
}

/// Whether a code byte is a command's first byte.
/// @param bits  the map
/// @param at    the byte
/// @return true when it is
bool marked(const uint32_t* bits, uint32_t at)
{
   return (bits[at >> kBitShift] >> (at & kBitMask) & 1u) != 0;
}

/// Checks one command's operands, and lists its address operands to a visitor.
/// @param b          the bank
/// @param at         its address
/// @param bytes      its size (command_bytes())
/// @param bits       the boundary map, or nullptr
/// @param[out] bad   the bad operand's address
/// @return true when good
bool check_operands(const Bank& b, uint32_t at, uint32_t bytes, const uint32_t* bits, uint32_t& bad)
{
   const char* sig = signature(b, b.code[at]);
   uint32_t p = at + 1;
   for (; *sig != '\0'; ++sig)
   {
      const char letter = *sig;
      if (letter == kSigArgs)
      {
         if (b.code[p] > kMaxArgs)
         {
            bad = p;
            return false;
         }
         p += 1 + b.code[p] * kArgBytes;
         continue;
      }
      if (letter == kSigAddress && bits != nullptr)
      {
         const uint32_t target = load_le32(b.code + p);
         if (target >= b.code_size || !marked(bits, target))
         {
            bad = p;
            return false;
         }
      }
      const SymbolKind kind = symbol_kind_of(letter);
      if (kind != SymbolKind::NUM_KINDS)
      {
         const uint16_t sym = load_le16(b.code + p);
         if (sym >= b.symbol_count || symbol_kind(b, sym) != kind)
         {
            bad = p;
            return false;
         }
      }
      p += fixed_operand_bytes(letter);
   }
   return p == at + bytes;
}

}   // namespace

BankError open(const uint8_t* data, uint32_t size, Bank& out)
{
   out = Bank{};
   if (data == nullptr || size < kHeaderBytes)
   {
      return BankError::TooSmall;
   }
   if (load_le32(data + kHeaderMagic) != kMagic)
   {
      return BankError::BadMagic;
   }
   if (load_le16(data + kHeaderVersion) != kVersion)
   {
      return BankError::BadVersion;
   }
   Bank b{};
   b.data = data;
   b.size = size;
   b.flags = load_le16(data + kHeaderFlags);
   b.script_count = load_le16(data + kHeaderScriptCount);
   b.symbol_count = load_le16(data + kHeaderSymbolCount);
   b.opcode_count = load_le16(data + kHeaderOpcodeCount);
   const uint32_t scripts = load_le32(data + kHeaderScripts);
   const uint32_t symbols = load_le32(data + kHeaderSymbols);
   const uint32_t opcodes = load_le32(data + kHeaderOpcodes);
   const uint32_t code = load_le32(data + kHeaderCode);
   b.code_size = load_le32(data + kHeaderCodeSize);
   const uint32_t strings = load_le32(data + kHeaderStrings);
   b.strings_size = load_le32(data + kHeaderStringsSize);
   if (!section_ok(scripts, b.script_count * kScriptEntryBytes, size) ||
       !section_ok(symbols, b.symbol_count * kSymbolBytes, size) ||
       !section_ok(opcodes, b.opcode_count * kOpcodeBytes, size) || !section_ok(code, b.code_size, size) ||
       !section_ok(strings, b.strings_size, size))
   {
      return BankError::BadSection;
   }
   b.scripts = data + scripts;
   b.symbols = data + symbols;
   b.opcodes = data + opcodes;
   b.code = data + code;
   b.strings = reinterpret_cast<const char*>(data + strings);

   for (uint16_t i = 0; i < b.script_count; ++i)
   {
      const uint8_t* e = b.scripts + i * kScriptEntryBytes;
      if (load_le32(e) >= b.code_size || !string_ok(b, load_le32(e + kScriptName)))
      {
         return BankError::BadEntry;
      }
   }
   for (uint16_t i = 0; i < b.symbol_count; ++i)
   {
      const uint8_t* e = b.symbols + i * kSymbolBytes;
      if (e[kSymbolKind] >= static_cast<uint8_t>(SymbolKind::NUM_KINDS) || !string_ok(b, load_le32(e + kSymbolName)))
      {
         return BankError::BadEntry;
      }
   }
   for (uint16_t i = 0; i < b.opcode_count; ++i)
   {
      const uint8_t* e = b.opcodes + i * kOpcodeBytes;
      const uint32_t sig = load_le32(e + kOpcodeSignature);
      if (e[0] < kFirstGameOpcode || sig == kNoName || !string_ok(b, sig) ||
          !string_ok(b, load_le32(e + kOpcodeName)))
      {
         return BankError::BadEntry;
      }
   }
   out = b;
   return BankError::None;
}

uint32_t script_address(const Bank& b, uint16_t index)
{
   return index < b.script_count ? load_le32(b.scripts + index * kScriptEntryBytes) : b.code_size;
}

const char* script_name(const Bank& b, uint16_t index)
{
   return index < b.script_count ? string_at(b, load_le32(b.scripts + index * kScriptEntryBytes + kScriptName)) : "";
}

uint16_t find_script(const Bank& b, const char* name)
{
   for (uint16_t i = 0; i < b.script_count; ++i)
   {
      if (same(script_name(b, i), name))
      {
         return i;
      }
   }
   return b.script_count;
}

SymbolKind symbol_kind(const Bank& b, uint16_t index)
{
   return index < b.symbol_count ? static_cast<SymbolKind>(b.symbols[index * kSymbolBytes + kSymbolKind])
                                 : SymbolKind::NUM_KINDS;
}

uint16_t symbol_index(const Bank& b, uint16_t index)
{
   return index < b.symbol_count ? load_le16(b.symbols + index * kSymbolBytes + kSymbolIndex) : kUnbound;
}

const char* symbol_name(const Bank& b, uint16_t index)
{
   return index < b.symbol_count ? string_at(b, load_le32(b.symbols + index * kSymbolBytes + kSymbolName)) : "";
}

const char* game_signature(const Bank& b, uint8_t opcode)
{
   const uint8_t* e = opcode_entry(b, opcode);
   return e != nullptr ? string_at(b, load_le32(e + kOpcodeSignature)) : nullptr;
}

const char* game_opcode_name(const Bank& b, uint8_t opcode)
{
   const uint8_t* e = opcode_entry(b, opcode);
   return e != nullptr ? string_at(b, load_le32(e + kOpcodeName)) : nullptr;
}

const char* signature(const Bank& b, uint8_t opcode)
{
   return opcode <= kLastEngineOpcode ? op_signature(opcode) : game_signature(b, opcode);
}

uint32_t command_bytes(const Bank& b, uint32_t address)
{
   if (address >= b.code_size)
   {
      return 0;
   }
   const char* sig = signature(b, b.code[address]);
   if (sig == nullptr)
   {
      return 0;
   }
   uint32_t p = address + 1;
   for (; *sig != '\0'; ++sig)
   {
      if (*sig == kSigArgs)
      {
         if (p >= b.code_size)
         {
            return 0;
         }
         p += 1 + b.code[p] * kArgBytes;
      }
      else
      {
         const uint32_t n = fixed_operand_bytes(*sig);
         if (n == 0)
         {
            return 0;
         }
         p += n;
      }
      if (p > b.code_size)
      {
         return 0;
      }
   }
   return p - address;
}

bool check_code(const Bank& b, uint32_t* boundary_bits, uint32_t& bad_address)
{
   if (boundary_bits != nullptr)
   {
      for (uint32_t i = 0; i < boundary_words(b.code_size); ++i)
      {
         boundary_bits[i] = 0;
      }
   }
   // first pass: every command's size (and its first byte in the map)
   for (uint32_t at = 0; at < b.code_size;)
   {
      const uint32_t n = command_bytes(b, at);
      if (n == 0)
      {
         bad_address = at;
         return false;
      }
      if (boundary_bits != nullptr)
      {
         mark(boundary_bits, at);
      }
      at += n;
   }
   // second pass: operands, with the map complete
   for (uint32_t at = 0; at < b.code_size;)
   {
      const uint32_t n = command_bytes(b, at);
      if (!check_operands(b, at, n, boundary_bits, bad_address))
      {
         return false;
      }
      at += n;
   }
   if (boundary_bits != nullptr)
   {
      for (uint16_t i = 0; i < b.script_count; ++i)
      {
         const uint32_t entry = script_address(b, i);
         if (!marked(boundary_bits, entry))
         {
            bad_address = entry;
            return false;
         }
      }
   }
   static_assert(kBitsPerWord == uint32_t{1} << kBitShift);
   return true;
}

}   // namespace capsule::anim
