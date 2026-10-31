/// @file
/// Storage boxes (D-26 section 3, package G3): creature records kept outside the party, in boxes. The box count,
/// the slots per box and the box names are manifest data (structure `storage`, tools/battlec/storage.py, compiled
/// into kStorage with the battle data; src/storage/storage_empty.cpp is the empty default of a build without it).
///
/// Storage is plain data a game keeps where it likes (EWRAM on the GBA), sized from its manifest (C6): the generated
/// battle_ids.h gives STORAGE_BYTES (every slot's record bytes) and STORAGE_USED_BYTES (a bit per slot), the game
/// declares a Buffer of those sizes (no heap) and the engine works on a Storage view of it (view()), which carries
/// its capacity so every operation checks the build's boxes against it at run time (fits()). The slots are laid out
/// box by box, slot k's record at k x the layout's record bytes (monster::kLayout), a bit per slot says it is used,
/// and the view keeps the box new records go to first. The save image keeps it as its Boxes section
/// (capsule/save/save.h). A recruited creature goes here when the party is full (capsule/battle/data/recruit.h);
/// scripts count, deposit and withdraw through the script port (capsule/script/port.h namespace boxes).
#pragma once

#include "capsule/core/pod.h"
#include "capsule/monster/monster.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::storage
{

/// @name Engine maxima per platform (tools/build_settings.py PLATFORM_LIMITS max_boxes / max_box_slots /
/// max_storage_bytes): what a manifest may ask for; a game's storage is the size its manifest needs
///@{
#if defined(CAPSULE_PLATFORM_GBA)
inline constexpr int kMaxBoxes = 16;                ///< Boxes.
inline constexpr int kMaxBoxSlots = 512;            ///< Slots of every box together.
inline constexpr int kMaxStorageBytes = 0x8000;     ///< Record bytes of every slot together (32 KB).
#else
inline constexpr int kMaxBoxes = 32;                ///< Boxes.
inline constexpr int kMaxBoxSlots = 2048;           ///< Slots of every box together.
inline constexpr int kMaxStorageBytes = 0x20000;    ///< Record bytes of every slot together (128 KB).
#endif
///@}

/// Bits per byte of the used bits.
inline constexpr int kUsedBitsPerByte = 8;
/// Bytes of the used bits of the most slots a platform holds (a bit per slot).
inline constexpr int kUsedBytes = (kMaxBoxSlots + kUsedBitsPerByte - 1) / kUsedBitsPerByte;

/// Bytes of the used bits of a number of slots.
/// @param slots  slots
/// @return bytes (a bit per slot)
constexpr uint32_t used_bytes_for(uint32_t slots)
{
   return (slots + kUsedBitsPerByte - 1) / kUsedBitsPerByte;
}
/// A slot that is not one (deposit(): no room).
inline constexpr uint16_t kNoSlot = 0xFFFF;

/// The game's storage rules (ROM; structure `storage`).
struct StorageData
{
   const text::Code* const* names;   ///< box_count display names.
   uint16_t box_size;                ///< Slots per box.
   uint8_t box_count;                ///< Boxes (0: none).
   uint8_t reserved;                 ///< 0.
};

/// The build's storage rules (generated with the battle data; the default has no boxes).
extern const StorageData kStorage;

/// The boxes' contents (RAM): a view over a game's Buffer (view()).
struct Storage
{
   uint8_t* bytes;        ///< capacity bytes: slot k's record at k * monster::kLayout.record_bytes.
   uint8_t* used;         ///< used_capacity bytes: a bit per slot, it holds a record.
   uint32_t capacity;     ///< Bytes of bytes.
   uint16_t used_capacity;   ///< Bytes of used.
   uint8_t current;       ///< The box deposit() tries first.
   uint8_t reserved;      ///< 0.
};

/// The bytes a game's storage lives in (a static it declares, e.g. CAPSULE_EWRAM_BSS), sized from its manifest:
/// Buffer<battle_ids::STORAGE_BYTES, battle_ids::STORAGE_USED_BYTES>.
/// @tparam kBytes  record bytes of every slot (at least 1)
/// @tparam kUsed   bytes of the used bits (at least 1)
template <uint32_t kBytes, uint32_t kUsed>
struct Buffer
{
   static_assert(kBytes <= static_cast<uint32_t>(kMaxStorageBytes) && kUsed <= static_cast<uint32_t>(kUsedBytes),
                 "more storage than this platform's engine holds");
   uint8_t bytes[kBytes > 0 ? kBytes : 1];   ///< The records.
   uint8_t used[kUsed > 0 ? kUsed : 1];      ///< The used bits.
};

/// The Storage view of a Buffer (every box free once clear() ran; a static Buffer starts zeroed: all free).
/// @tparam kBytes  the buffer's record bytes
/// @tparam kUsed   its used bits' bytes
/// @param b        the buffer (outlives the view)
/// @return the view (current box 0)
template <uint32_t kBytes, uint32_t kUsed>
constexpr Storage view(Buffer<kBytes, kUsed>& b)
{
   return Storage{b.bytes, b.used, sizeof(b.bytes), static_cast<uint16_t>(sizeof(b.used)), 0, 0};
}

/// Slots of every box (box_count x box_size).
/// @return the count
inline int slot_count()
{
   return kStorage.box_count * kStorage.box_size;
}

/// The box a slot is in.
/// @param slot  slot index
/// @return box index
inline int box_of(int slot)
{
   return kStorage.box_size == 0 ? 0 : slot / kStorage.box_size;
}

/// A box's display name.
/// @param box  box index
/// @return its name, or nullptr past the boxes
inline const text::Code* box_name(int box)
{
   return box >= 0 && box < kStorage.box_count ? kStorage.names[box] : nullptr;
}

/// Whether the build's boxes fit a storage (every slot's record and used bit; a view too small for them takes no
/// records past its end: put() and deposit() refuse those slots).
/// @param st  the storage
/// @return true if every slot fits
bool fits(const Storage& st);

/// Empties every box (the current box back to the first).
/// @param st  the storage
void clear(Storage& st);

/// Whether a slot holds a record.
/// @param st    the storage
/// @param slot  slot index
/// @return true if used (false past the slots)
bool used(const Storage& st, int slot);

/// Records held in all boxes.
/// @param st  the storage
/// @return the count
int count(const Storage& st);

/// Records held in one box.
/// @param st   the storage
/// @param box  box index
/// @return the count (0 past the boxes)
int box_count(const Storage& st, int box);

/// Copies a slot's record out (the slot stays used).
/// @param st    the storage
/// @param slot  slot index
/// @param out   the record (written only when the slot is used)
/// @return false for a free slot or one past the slots
bool read(const Storage& st, int slot, monster::Record& out);

/// Puts a record into a free slot.
/// @param st    the storage
/// @param slot  slot index
/// @param rec   the record
/// @return false when the slot is used or past the slots
bool put(Storage& st, int slot, const monster::Record& rec);

/// Puts a record into the first free slot, from the current box on (then from the first box).
/// @param st   the storage
/// @param rec  the record
/// @return its slot, or kNoSlot when every box is full
uint16_t deposit(Storage& st, const monster::Record& rec);

/// Takes a record out of a slot (the slot is free afterwards).
/// @param st    the storage
/// @param slot  slot index
/// @param out   the record
/// @return false for a free slot or one past the slots
bool withdraw(Storage& st, int slot, monster::Record& out);

/// Frees a slot without reading it (a released creature).
/// @param st    the storage
/// @param slot  slot index
/// @return false for a free slot or one past the slots
bool release(Storage& st, int slot);

/// Moves a record to a free slot.
/// @param st    the storage
/// @param from  its slot
/// @param to    a free slot
/// @return false when @p from is free, @p to is used, or either is past the slots
bool move(Storage& st, int from, int to);

/// Sets the box deposit() tries first.
/// @param st   the storage
/// @param box  box index
/// @return false past the boxes
bool set_current(Storage& st, int box);

/// Whether a loaded storage fits the build: the current box is a box and no used bit lies past the slots.
/// @param st  the storage
/// @return true if it is consistent
bool valid(const Storage& st);

} // namespace capsule::storage
