/// @file
/// The engine save image (D-26 decision 1, package G2; versioned sections C6): one image whose sections are sized
/// from the manifests, kept on the save medium (capsule/hal/save.h) in two copies.
///
/// Image (little-endian, P-19):
///   - header (kHeaderBytes): magic "CCSV", format version, section count, sequence (each write counts up; the
///     newest valid copy is the one read), bytes after the header, the header checksum (FNV-1a 32 over the header
///     but its own field, and the section table);
///   - the section table (kSectionEntryBytes each: id, the section's own version, size, checksum over its bytes);
///   - the sections in table order. Each section carries the shape it was written in, so a build whose manifests
///     changed still reads it (C6: per-section migration; a section is refused only when it cannot be migrated):
///     - Bag: the entry count, then per filled slot its item's stable key (capsule/inventory/data.h item_keys) and
///       count, pocket by pocket. Loading adds each entry to an empty bag: an item the build does not have is
///       dropped, a count over its pocket's quantity_max is clamped, an entry its pocket has no room for is dropped
///       (Notes counts each); items may be added, removed or reordered and pockets added, removed or resized. Keys
///       are FNV-1a 32 of the item's name (no item has key 0: a build error, C7); a removed item's key equals a
///       new item's with a chance of about one in 2^32 per pair, and its old entries then load as the new item;
///     - Party: the record hash (C7: monster::Layout::record_hash, the layout's without its party size; images
///       before C7 hold the full layout hash, also read), the record bytes, the member count, then the members'
///       records. A party that grew or shrank loads; it is refused (OtherShape) for another record shape or more
///       members than the build's party holds;
///     - Boxes (G3): the record hash, the record bytes, the box count and size, the current box, a used
///       bit per slot (low bit first), then the used slots' records in slot order. Loading keeps each record's box
///       and place when the build has them, else puts it into the first free slot (Notes::moved_records); refused
///       for another record layout or more records than the build's slots;
///     - Nursery (B2): the record hash, the record bytes, the slot count, the used bits, whether a result waits,
///       then per used slot its level when left, its gained experience and its record, then the waiting result's
///       record. Loading keeps each record's slot when the build has it, else the first free one
///       (Notes::moved_records); refused for another record layout or more records than the build's slots;
///     - Script: per non-empty sub-bank (capsule/script/data.h kSaveSubBanks: flags and variables per lifetime,
///       name slots) its index, shape hash, bytes and storage: copied when the shape is the build's, moved by the
///       build's recorded remap otherwise (tools/save_shape.py), reset for a transient lifetime without one,
///       refused for the permanent lifetime or the name slots without one. A variable's shape holds its range
///       when it is not its width's whole range (C7), so a narrowed range is a new shape and the remap clamps the
///       old values to it; a remap that drops a set flag, a non-zero variable or a non-empty name counts it
///       (Notes::dropped_flags / dropped_vars). F1: the timed field effects' implicit variables
///       (capsule/script/field_effects.h) are saved like any other; after a load the effects that do not
///       persist are stopped (their variables cleared).
///     - Game (N3): the game's version of its block (2 bytes), the block's bytes (4), then the block as the game gave
///       it (capsule::save::GameBlock). Loading copies it when its version and size are the destination's, else the
///       read is refused (OtherShape: the game migrates its own blocks before it asks again);
///   A section the image does not have loads empty (an older build without boxes: no boxed records; N3: a game
///   block of zeros).
///
/// Copies (C7): the medium is split in two slots of whole erase units (slot_bytes(): from the medium alone), copy k
/// at k * slot_bytes(), so a copy an older build wrote with an image of another size is found where it is. An
/// image larger than a slot is kept once, from offset 0 (tools/save_layout.py refuses that at build time unless the
/// game opts in); reading still looks at both slots. Sequences compare by serial-number arithmetic (RFC 1982): a
/// write after 0xFFFFFFFF counts on from 0 and is the newer one.
///
/// Writing goes to the older copy, so a write cut short leaves the newer image readable; the write erases the erase
/// units its image takes, programs it and commits it (hal::save::commit(): durable on the desktop's disk). A section
/// whose source is nullptr is copied from the newest valid copy unchanged (C6: nothing is lost when a caller saves
/// part of the state), or written empty when there is none. N3: with one copy the write erases where those sections
/// are, so they go through the built-in stage first (GBA: 2 KB; kept sections larger than that refuse the write,
/// NoRoom: give their sources).
///
/// Reading is all or nothing (C7): it checks the newest copy whole (header and section checksums), then stages every
/// section it loads, back to back, in the built-in stage (hosts: the whole budget, a static buffer; GBA: 2 KB on
/// the stack, enough for the bag, party and script sections) and, past it, in the caller's Stage (GBA: the storage
/// boxes); a section that fits neither refuses the read (NoRoom). The staged bytes are checked again (a medium that
/// changed since: MediumError), every section is decoded once without storing (OtherShape, OtherVersion) and only
/// then do the staged bytes land. A read that fails anywhere leaves every destination as it was.
///
/// tools/save_layout.py works out the same sizes from the manifests at build time and checks them against the
/// platform's medium (two copies, unless the game opts in to one: C6), and writes the same bytes (image()).
#pragma once

#include "capsule/combine/nursery.h"
#include "capsule/inventory/bag.h"
#include "capsule/monster/monster.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/script.h"
#include "capsule/script/step_effects.h"
#include "capsule/storage/storage.h"

#include <cstdint>

namespace capsule::save
{

/// @name Image layout (tools/save_layout.py mirrors them)
///@{
inline constexpr uint32_t kMagic = 0x56534343u;       ///< "CCSV" read as a little-endian word.
inline constexpr uint16_t kVersion = 2;               ///< Image format version (C6: versioned sections).
inline constexpr uint32_t kHeaderBytes = 32;          ///< Header bytes.
inline constexpr uint32_t kSectionEntryBytes = 12;    ///< Bytes per section table entry.
inline constexpr int kMaxSections = 8;                ///< Section table entries an image may have.
inline constexpr uint32_t kBagHeadBytes = 2;          ///< Bag: the entry count.
inline constexpr uint32_t kBagEntryBytes = 6;         ///< Bag: per entry its item key (4) and count (2).
inline constexpr uint32_t kPartyHeadBytes = 7;        ///< Party: layout hash (4), record bytes (2), count (1).
inline constexpr uint32_t kBoxesHeadBytes = 10;       ///< Boxes: layout hash (4), record bytes (2), box count (1),
                                                      ///< box size (2), current box (1).
inline constexpr uint32_t kScriptHeadBytes = 1;       ///< Script: the sub-bank count.
inline constexpr uint32_t kSubBankHeadBytes = 7;      ///< Script: per sub-bank its index (1), hash (4), bytes (2).
inline constexpr uint32_t kNurseryHeadBytes = 9;      ///< Nursery (B2): record hash (4), record bytes (2), slots (1),
                                                      ///< used bits (1), result waiting (1).
inline constexpr uint32_t kNurserySlotBytes = 5;      ///< Nursery: per used slot its level (1) and gained
                                                      ///< experience (4), before its record.
inline constexpr uint32_t kGameHeadBytes = 6;         ///< Game (N3): the block's version (2) and bytes (4).
///@}

/// The save budget of this platform (build_settings PLATFORM_LIMITS max_save_bytes; tools/save_layout.py media).
#if defined(CAPSULE_PLATFORM_GBA) && defined(CAPSULE_SAVE_SRAM)
inline constexpr uint32_t kSaveBudget = 0x8000u;    ///< The cartridge's 32 KB SRAM (C6: its own medium).
#elif defined(CAPSULE_PLATFORM_GBA)
inline constexpr uint32_t kSaveBudget = 0x10000u;   ///< The cartridge's 64 KB flash.
#else
inline constexpr uint32_t kSaveBudget = 0x40000u;   ///< 256 KB.
#endif

/// The sections (their ids in the table).
enum class Section : uint8_t
{
   Bag,          ///< The bag (inventory).
   Party,        ///< The party's records.
   Boxes,        ///< Storage boxes (G3; absent without boxes).
   Script,       ///< The script bank's flags, variables and name slots.
   Nursery,      ///< The nursery's slots and waiting result (B2; absent without a nursery).
   Game,         ///< N3: the game's own block (absent when the game has none: game_block_bytes() is 0).
   NUM_SECTIONS  ///< Number of sections.
};
/// Number of sections.
inline constexpr int kSectionCount = static_cast<int>(Section::NUM_SECTIONS);

/// N3: the game's own block of the image (a game keeps state the engine's sections do not hold, e.g. its own party
/// and boxes): its bytes as they are, with the game's version of their layout. Its size is game_block_bytes().
struct GameBlock
{
   uint8_t* bytes = nullptr;   ///< The block, or nullptr: none (written: copied from the newest copy).
   uint32_t size = 0;          ///< Its bytes (game_block_bytes()).
   uint16_t version = 0;       ///< The game's layout version: an image of another version or size is refused.
};

/// What the image holds. A nullptr source is not touched: write() copies its section from the newest valid copy
/// (or writes it empty), read() leaves it as it is. Every field must be given (C6: no defaults), but the game's
/// block (N3), which defaults to none.
struct Sources
{
   inventory::Bag* bag;          ///< The bag, or nullptr.
   monster::Party* party;        ///< The party, or nullptr.
   storage::Storage* storage;    ///< The storage boxes (G3), or nullptr.
   combine::nursery::State* nursery;   ///< The nursery (B2; combine::nursery::state()), or nullptr.
   GameBlock game = {};          ///< N3: the game's block, or none.
};

/// N3: the bytes of the game's block in this build (the game defines it; the engine's default: 0, no Game section).
/// @return bytes
uint32_t game_block_bytes();

/// How a write or read went.
enum class Status : uint8_t
{
   Ok,            ///< Done.
   Empty,         ///< Reading: no image on the medium.
   Corrupt,       ///< Reading: no copy passes its checksums.
   OtherVersion,  ///< Reading: an image of another format version, or a section of a version this build does not
                  ///< know.
   OtherShape,    ///< Reading: a section this build cannot migrate (another record layout, more party members or
                  ///< boxed records than the build holds, a permanent script bank of an unrecorded shape).
   NoRoom,        ///< The image does not fit the medium (or the storage view does not fit the build's boxes).
   MediumError,   ///< The medium failed a read, erase or program.
   NUM_STATUSES   ///< Number of statuses.
};

/// What a read changed while migrating (all 0 for an image of this build's shapes).
struct Notes
{
   uint16_t dropped_items;    ///< Bag entries dropped: an unknown item, or no room in its pocket.
   uint16_t clamped_items;    ///< Bag entries clamped to their pocket's quantity_max.
   uint16_t moved_records;    ///< Boxed records put into another slot (their box or place is gone).
   uint8_t migrated_banks;    ///< Script sub-banks moved by a recorded remap.
   uint8_t reset_banks;       ///< Transient script sub-banks reset (no remap).
   uint16_t dropped_flags;    ///< Set flags a remap dropped (renamed or removed since; C7).
   uint16_t dropped_vars;     ///< Non-zero variables and non-empty name slots a remap dropped (C7).
};

/// Room the caller lends a read for the sections past the built-in stage (C7; GBA: the storage boxes, about the
/// boxes' bytes; any RAM free while read() runs, e.g. a scratch buffer). Not needed on hosts.
struct Stage
{
   uint8_t* bytes;   ///< The room, or nullptr: none.
   uint32_t size;    ///< Its bytes.
};

/// Most bytes one section takes in this build.
/// @param s  the section
/// @return its size (0: the build writes no such section)
uint32_t section_size(Section s);

/// Most bytes of the whole image in this build (header, table, sections).
/// @return its size
uint32_t image_size();

/// Copies the medium holds (2 when the image fits a slot, 1 when it fits the medium, or 0 when it does not).
/// @return the count
int copy_count();

/// Bytes of one copy's slot (C7): half the medium in whole erase units; copy k starts at k times them.
/// @return bytes
uint32_t slot_bytes();

/// Writes the image into the older copy.
/// @param src  what it holds (a nullptr source: its section copied from the newest copy)
/// @return Ok, NoRoom or MediumError
Status write(const Sources& src);

/// Reads the newest valid copy into the sources (the bag, the party, the storage boxes, the script bank's storage),
/// migrating sections of older shapes. All or nothing: every section is staged and checked before any lands (see
/// the file comment). A game uses load(), which also tells the script VM.
/// @param dst    where it goes (a nullptr source: its section is skipped)
/// @param notes  what migrating changed, or nullptr
/// @param stage  room for the sections past the built-in stage (GBA: the storage boxes), or none
/// @return Ok or why not (NoRoom: a section fits no stage)
Status read(const Sources& dst, Notes* notes = nullptr, const Stage& stage = Stage{nullptr, 0});

/// read(), then the script VM's state counts as changed (triggers and object pages see the loaded flags and
/// variables: script::bump_state()); C8: the timed field effects' unreported expiries and the step effects' due
/// runs of the game before are forgotten (field_effects::reset_clock(), step_effects::clear_due(): the loaded
/// variables are what counts).
/// @param dst    where it goes
/// @param notes  what migrating changed, or nullptr
/// @param stage  room for the sections past the built-in stage, or none
/// @return Ok or why not
inline Status load(const Sources& dst, Notes* notes = nullptr, const Stage& stage = Stage{nullptr, 0})
{
   const Status st = read(dst, notes, stage);
   if (st == Status::Ok)
   {
      script::bump_state();
      script::field_effects::reset_clock();
      script::step_effects::clear_due();
   }
   return st;
}

/// The sequence number of the newest valid copy (serial-number arithmetic: see the file comment).
/// @param out  its sequence (unchanged when there is none)
/// @return Ok, Empty, Corrupt, OtherVersion or MediumError
Status newest(uint32_t& out);

} // namespace capsule::save
