/// @file
/// Animation script banks (BF, D-38): the binary format the animation-script player (capsule/anim/player.h) runs.
///
/// A bank holds scripts (a byte-coded command stream with named entry points), the names of the game objects the
/// scripts refer to (sprite templates, visual tasks, sound tasks, sprite callbacks, data), and the operand layouts
/// ("signatures") of the game's own commands, so any tool can read and disassemble a bank without the game's code.
/// The full description, with examples, is docs/ANIM_SCRIPT.md; tools/anim_bank.py writes and reads banks.
///
/// Layout (every number little-endian; sections start on a multiple of 4 bytes, offsets count from the bank's
/// first byte):
/// - Header (kHeaderBytes): magic, version, flags, counts, section offsets (the kHeader* offsets below).
/// - Scripts: script_count entries of kScriptEntryBytes: the entry's code address (u32), its name (u32 string
///   offset, or kNoName).
/// - Symbols: symbol_count entries of kSymbolBytes: kind (u8, SymbolKind), reserved (u8, 0), the index in the game's
///   table of that kind (u16, or kUnbound), name (u32 string offset).
/// - Opcodes: opcode_count entries of kOpcodeBytes: the game opcode (u8, kFirstGameOpcode-255), three reserved
///   bytes (0), its name (u32), its signature (u32 string offset).
/// - Code: code_size bytes of commands only, one after the other. A command is its opcode byte, then its operands as
///   its signature says, with no padding. Code addresses (operand 'a', script entries) are offsets from the code
///   section's first byte and land on a command's opcode.
/// - Strings: NUL-terminated ASCII, strings_size bytes; string offsets count from the section's first byte.
///
/// Operand letters of a signature (kSig* below): b u8, c s8, h u16, s s16, w u32, a code address (u32), T / K / S /
/// C / D a symbol (u16 index into the bank's symbol table, of kind template / visual task / sound task / callback /
/// data), v an argument list (u8 count, at most kMaxArgs, then count s16 values).
///
/// Opcodes 0 to kLastEngineOpcode are the engine's generic commands (Op, with fixed signatures: op_signature());
/// kFirstGameOpcode to 255 are the game's, described by the bank's opcode entries and run by the game's command table.
#pragma once

#include <cstdint>

/// Animation scripts: banks and their player.
namespace capsule::anim
{

/// @name Header
///@{
inline constexpr uint32_t kMagic = 0x4D4E4143u;     ///< "CANM" read as a little-endian u32.
inline constexpr uint16_t kVersion = 1;             ///< Format version this engine reads.
inline constexpr uint32_t kHeaderBytes = 44;        ///< Bytes of the header.
inline constexpr uint32_t kHeaderMagic = 0;         ///< u32 magic.
inline constexpr uint32_t kHeaderVersion = 4;       ///< u16 version.
inline constexpr uint32_t kHeaderFlags = 6;         ///< u16 flags (kFlag*).
inline constexpr uint32_t kHeaderScriptCount = 8;   ///< u16 scripts.
inline constexpr uint32_t kHeaderSymbolCount = 10;  ///< u16 symbols.
inline constexpr uint32_t kHeaderOpcodeCount = 12;  ///< u16 game opcode entries.
inline constexpr uint32_t kHeaderReserved = 14;     ///< u16, 0.
inline constexpr uint32_t kHeaderScripts = 16;      ///< u32 offset of the script entries.
inline constexpr uint32_t kHeaderSymbols = 20;      ///< u32 offset of the symbols.
inline constexpr uint32_t kHeaderOpcodes = 24;      ///< u32 offset of the opcode entries.
inline constexpr uint32_t kHeaderCode = 28;         ///< u32 offset of the code.
inline constexpr uint32_t kHeaderCodeSize = 32;     ///< u32 bytes of code.
inline constexpr uint32_t kHeaderStrings = 36;      ///< u32 offset of the strings.
inline constexpr uint32_t kHeaderStringsSize = 40;  ///< u32 bytes of strings.
inline constexpr uint16_t kFlagIndexed = 1;         ///< Every symbol carries its game table index (no binding needed).
inline constexpr uint32_t kSectionAlign = 4;        ///< Sections start on a multiple of this.
///@}

/// @name Entries
///@{
inline constexpr uint32_t kScriptEntryBytes = 8;    ///< Script entry: u32 address, u32 name.
inline constexpr uint32_t kScriptName = 4;          ///< Script entry: offset of the name.
inline constexpr uint32_t kSymbolBytes = 8;         ///< Symbol: u8 kind, u8 reserved, u16 index, u32 name.
inline constexpr uint32_t kSymbolKind = 0;          ///< Symbol: offset of the kind.
inline constexpr uint32_t kSymbolIndex = 2;         ///< Symbol: offset of the game index.
inline constexpr uint32_t kSymbolName = 4;          ///< Symbol: offset of the name.
inline constexpr uint32_t kOpcodeBytes = 12;        ///< Opcode entry: u8 opcode, 3 reserved, u32 name, u32 signature.
inline constexpr uint32_t kOpcodeName = 4;          ///< Opcode entry: offset of the name.
inline constexpr uint32_t kOpcodeSignature = 8;     ///< Opcode entry: offset of the signature.
inline constexpr uint32_t kNoName = 0xFFFFFFFFu;    ///< A string offset that names nothing.
inline constexpr uint16_t kUnbound = 0xFFFF;        ///< A symbol's index before binding.
///@}

/// @name Commands and operands
///@{
inline constexpr int kMaxArgs = 8;                  ///< Arguments a command passes (Player::args).
inline constexpr uint8_t kLastEngineOpcode = 0x3F;  ///< Opcodes 0-63 are the engine's (unused ones are reserved).
inline constexpr uint8_t kFirstGameOpcode = 0x40;   ///< Opcodes 64-255 are the game's.
inline constexpr int kOpcodes = 256;                ///< Opcodes (a command table's entries).
inline constexpr char kSigU8 = 'b';                 ///< Operand: u8.
inline constexpr char kSigS8 = 'c';                 ///< Operand: s8.
inline constexpr char kSigU16 = 'h';                ///< Operand: u16.
inline constexpr char kSigS16 = 's';                ///< Operand: s16.
inline constexpr char kSigU32 = 'w';                ///< Operand: u32.
inline constexpr char kSigAddress = 'a';            ///< Operand: code address (u32).
inline constexpr char kSigTemplate = 'T';           ///< Operand: symbol of kind Template (u16).
inline constexpr char kSigTask = 'K';               ///< Operand: symbol of kind Task (u16).
inline constexpr char kSigSoundTask = 'S';          ///< Operand: symbol of kind SoundTask (u16).
inline constexpr char kSigCallback = 'C';           ///< Operand: symbol of kind Callback (u16).
inline constexpr char kSigData = 'D';               ///< Operand: symbol of kind Data (u16).
inline constexpr char kSigArgs = 'v';               ///< Operand: argument list (u8 count, count x s16).
///@}

/// What a symbol names (a game table per kind: capsule/anim/player.h Tables).
enum class SymbolKind : uint8_t
{
   Template,    ///< A sprite template (Tables::templates).
   Task,        ///< A visual task (Tables::tasks).
   SoundTask,   ///< A sound task (Tables::sound_tasks).
   Callback,    ///< A sprite callback (Tables::callbacks).
   Data,        ///< Game data (Tables::data).
   NUM_KINDS    ///< Number of kinds.
};

/// The engine's generic commands (their signatures: op_signature(); their behaviour: docs/ANIM_SCRIPT.md and the
/// cmd_* handlers of capsule/anim/player.h).
enum class Op : uint8_t
{
   End,               ///< "": ends the script once no visual or sound task runs (and the game's finish hook agrees).
   Delay,             ///< "b": waits (n frames; 0 waits as -1: the wait mode's one-frame step).
   WaitVisual,        ///< "": waits until no visual task or sprite runs.
   WaitSound,         ///< "": waits until no sound task runs and the game's sound hook says quiet.
   Jump,              ///< "a": goes to an address.
   Call,              ///< "a": goes to an address, Return comes back.
   Return,            ///< "": back after the last Call.
   SetArg,            ///< "bs": sets argument n.
   JumpIfArgEq,       ///< "bsa": goes to an address when argument n equals the value.
   CreateSprite,      ///< "Tbv": the arguments, then a sprite of a template (the game places it; flags are its own).
   CreateTask,        ///< "Kbv": the arguments, then a visual task with a priority, run once at once.
   CreateSoundTask,   ///< "Sbv": the arguments, then a sound task with a priority, run once at once.
   LoadGfx,           ///< "h": the game loads a tag's graphics; waits one frame.
   UnloadGfx,         ///< "h": the game frees a tag's graphics.
   PlaySe,            ///< "hc": the game plays a sound effect with a pan.
   SetBlend,          ///< "bbbbb": colour effect mode, first and second target masks, alpha weights.
   BlendOff,          ///< "": colour effects off, weights 0.
   BgShow,            ///< "bb": shows (1) or hides (0) a background layer.
   BgScroll,          ///< "bss": sets a background layer's scroll.
   Nop,               ///< "": nothing (advances).
   NUM_OPS            ///< Number of generic commands.
};

/// A generic command's signature.
/// @param op  the command
/// @return its operand letters ("" for none), or nullptr for an opcode that is not one
constexpr const char* op_signature(uint8_t op)
{
   constexpr const char* kSignatures[] = {"",    "b",    "",   "",  "a", "a",     "", "bs", "bsa", "Tbv",
                                          "Kbv", "Sbv",  "h",  "h", "hc", "bbbbb", "", "bb", "bss", ""};
   static_assert(sizeof(kSignatures) / sizeof(kSignatures[0]) == static_cast<int>(Op::NUM_OPS));
   return op < static_cast<uint8_t>(Op::NUM_OPS) ? kSignatures[op] : nullptr;
}

/// A generic command's name (as tools/anim_bank.py writes it).
/// @param op  the command
/// @return its name, or nullptr
constexpr const char* op_name(uint8_t op)
{
   constexpr const char* kNames[] = {"end",       "delay",      "wait_visual", "wait_sound",       "jump",
                                     "call",      "return",     "set_arg",     "jump_if_arg_eq",   "create_sprite",
                                     "create_task", "create_sound_task", "load_gfx", "unload_gfx",  "play_se",
                                     "set_blend", "blend_off",  "bg_show",     "bg_scroll",        "nop"};
   static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<int>(Op::NUM_OPS));
   return op < static_cast<uint8_t>(Op::NUM_OPS) ? kNames[op] : nullptr;
}

/// The symbol kind an operand letter refers to.
/// @param letter  a signature letter
/// @return the kind, or SymbolKind::NUM_KINDS when the letter is not a symbol
constexpr SymbolKind symbol_kind_of(char letter)
{
   return letter == kSigTemplate    ? SymbolKind::Template
          : letter == kSigTask      ? SymbolKind::Task
          : letter == kSigSoundTask ? SymbolKind::SoundTask
          : letter == kSigCallback  ? SymbolKind::Callback
          : letter == kSigData      ? SymbolKind::Data
                                    : SymbolKind::NUM_KINDS;
}

/// Bytes one fixed-size operand takes (an argument list: see operand_bytes()).
/// @param letter  a signature letter
/// @return bytes, or 0 for an unknown letter or 'v'
constexpr uint32_t fixed_operand_bytes(char letter)
{
   switch (letter)
   {
   case kSigU8:
   case kSigS8:
      return 1;
   case kSigU16:
   case kSigS16:
   case kSigTemplate:
   case kSigTask:
   case kSigSoundTask:
   case kSigCallback:
   case kSigData:
      return sizeof(uint16_t);
   case kSigU32:
   case kSigAddress:
      return sizeof(uint32_t);
   default:
      return 0;
   }
}

/// Bytes of one argument value in an argument list.
inline constexpr uint32_t kArgBytes = 2;

/// A bank in memory (ROM or RAM), checked by open(). The fields are the header's, read once.
struct Bank
{
   const uint8_t* data;        ///< The bank's first byte.
   uint32_t size;              ///< Bytes of the bank.
   uint16_t flags;             ///< kFlag* bits.
   uint16_t script_count;      ///< Scripts.
   uint16_t symbol_count;      ///< Symbols.
   uint16_t opcode_count;      ///< Game opcode entries.
   const uint8_t* scripts;     ///< First script entry.
   const uint8_t* symbols;     ///< First symbol.
   const uint8_t* opcodes;     ///< First opcode entry.
   const uint8_t* code;        ///< First code byte.
   uint32_t code_size;         ///< Bytes of code.
   const char* strings;        ///< First string byte.
   uint32_t strings_size;      ///< Bytes of strings.
};

/// Why open() refused a bank.
enum class BankError : uint8_t
{
   None,          ///< The bank is good.
   TooSmall,      ///< Shorter than its header.
   BadMagic,      ///< Not a bank.
   BadVersion,    ///< A version this engine does not read.
   BadSection,    ///< A section is misaligned or runs past the bank.
   BadEntry,      ///< A script address, symbol kind or opcode entry is out of range.
   NUM_ERRORS     ///< Number of errors.
};

/// Reads and checks a bank's header and tables (not its code: see check_code()).
/// @param data  the bank (4-byte aligned)
/// @param size  its bytes
/// @param[out] out  the bank, valid when the result is BankError::None
/// @return why it was refused, or BankError::None
BankError open(const uint8_t* data, uint32_t size, Bank& out);

/// A script's code address.
/// @param b      the bank
/// @param index  0 to script_count - 1
/// @return its address (an offset into the code)
uint32_t script_address(const Bank& b, uint16_t index);

/// A script's name.
/// @param b      the bank
/// @param index  0 to script_count - 1
/// @return the name, or "" when it has none
const char* script_name(const Bank& b, uint16_t index);

/// The first script of a name.
/// @param b     the bank
/// @param name  its name
/// @return its index, or script_count when none has it
uint16_t find_script(const Bank& b, const char* name);

/// A symbol's kind.
/// @param b      the bank
/// @param index  0 to symbol_count - 1
/// @return its kind
SymbolKind symbol_kind(const Bank& b, uint16_t index);

/// A symbol's game index as the bank stores it.
/// @param b      the bank
/// @param index  0 to symbol_count - 1
/// @return the index, or kUnbound
uint16_t symbol_index(const Bank& b, uint16_t index);

/// A symbol's name.
/// @param b      the bank
/// @param index  0 to symbol_count - 1
/// @return the name, or ""
const char* symbol_name(const Bank& b, uint16_t index);

/// A game opcode's signature as the bank declares it.
/// @param b       the bank
/// @param opcode  kFirstGameOpcode to 255
/// @return its letters, or nullptr when the bank does not declare it
const char* game_signature(const Bank& b, uint8_t opcode);

/// A game opcode's name as the bank declares it.
/// @param b       the bank
/// @param opcode  kFirstGameOpcode to 255
/// @return its name, or nullptr
const char* game_opcode_name(const Bank& b, uint8_t opcode);

/// Any opcode's signature: the engine's for a generic command, the bank's for a game one.
/// @param b       the bank
/// @param opcode  0-255
/// @return its letters, or nullptr when the opcode is unknown
const char* signature(const Bank& b, uint8_t opcode);

/// Bytes of the command at an address (opcode and operands).
/// @param b        the bank
/// @param address  a code address
/// @return its bytes, or 0 when the opcode is unknown or the command runs past the code
uint32_t command_bytes(const Bank& b, uint32_t address);

/// Words of the boundary map check_code() needs for a code section.
/// @param code_size  bytes of code
/// @return u32 words (one bit per code byte)
constexpr uint32_t boundary_words(uint32_t code_size)
{
   constexpr uint32_t kBitsPerWord = 32;
   return (code_size + kBitsPerWord - 1) / kBitsPerWord;
}

/// Checks the code section, which holds commands only, one after the other from its first byte: each command is
/// known, ends inside the code, names symbols of the right kind and passes at most kMaxArgs arguments; with a
/// boundary map, every script entry and address operand also lands on the first byte of a command.
/// @param b              the bank
/// @param boundary_bits  boundary_words(code_size) words of scratch, or nullptr (no landing check)
/// @param[out] bad_address  the first bad command's (or operand's) address when the result is false
/// @return true when the code is good
bool check_code(const Bank& b, uint32_t* boundary_bits, uint32_t& bad_address);

}   // namespace capsule::anim
