/// @file
/// MonsterCore (C-07a): creature records laid out by the game, read by role.
///
/// The engine never names a game's own record fields. It names **engine
/// roles** from a frozen, append-only registry (Role): each has a fixed ID
/// (its place in the registry), a default element width and a default element
/// count. A game picks which engine roles its record holds in its monster
/// layout manifest and may add **custom roles** (D-22 decision 2: gender, met
/// data, ribbons, friendship, ...): fields the engine stores and scripts read
/// and write, but no engine code interprets. The battle compiler packs both
/// into a bit layout and emits the build's kLayout table. Engine code reads
/// and writes records through get() / set(), so the same engine works with
/// every game's layout.
///
/// **Role IDs** (RoleId): an engine role's ID is its Role value (below
/// kFirstCustomRole); custom role i of the layout has ID kFirstCustomRole + i
/// (its declaration order). battle_ids.h names every role the layout holds
/// ROLE_<NAME>; find_custom() finds a custom role by its name_hash().
///
/// **What a layout chooses** (D-23 decision 4, RoleChoice): an element width up
/// to RoleChoice::max_width for the roles that allow it, the move slots
/// (Layout::move_slots, up to kMaxMoveSlots: the counts of Moves, Pp and
/// PpUps), the stats (Layout::stat_count: the per-stat roles), the nickname's
/// length (up to RoleChoice::max_count) and the party size
/// (Layout::party_size, up to kMaxPartySize).
///
/// **Retired roles** (A10): OwnerSecret, Friendship, Form, Nicknamed,
/// OwnerName and CaptureDevice are custom roles now. Their IDs stay reserved
/// (IDs never move) and no layout holds them as engine roles; a custom role of
/// the same name and shape keeps the layout hash they gave.
///
/// **Unstored custom roles** (E13a): a custom role the layout declares with
/// "stored": false has a RoleSlot (name hash, width, count, initial) but no
/// bits (RoleSlot::offset kAbsent): has() is false, get() reads 0 and set()
/// does nothing. Its value is computed whenever it is read
/// (capsule::battle::record_value(): its generation entry), so it never goes
/// stale. Unstored slots follow the stored ones in Layout::custom (IDs
/// kFirstCustomRole + custom_count + j); custom_count counts the stored ones
/// only, so transfers and the hash see the record's bits alone.
///
/// Records are little-endian bit strings, LSB first (P-19): role element i
/// of width w starts at bit offset(role) + i * w. A role the layout leaves out
/// reads as 0 and ignores writes.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"

#include <cstdint>

namespace capsule::monster
{

/// The role registry (frozen, append-only; P-9 enum, ends with NUM_ROLES).
enum class Role : uint8_t
{
   SpeciesKey,       ///< 32: AssetKey of the species ("species:NAME").
   Seed,             ///< 32 (1-32): per-monster random seed.
   OwnerId,          ///< 16 (1-32): original owner's public ID.
   OwnerSecret,      ///< Retired (a custom role now; was 16).
   Experience,       ///< 24 (1-32): total experience points.
   Level,            ///< 8 (1-8): level.
   Moves,            ///< 16 (1-16) x move slots: move indices in the game's move table (all bits set = empty).
   Pp,               ///< 7 (1-8) x move slots: storage for a move resource's pool (D-25: the battle reads it only
                     ///< as a resource's persist role, e.g. PP).
   PpUps,            ///< 2 (1-8) x move slots: storage for a move resource's raises (a resource's raises role).
   StatGenes,        ///< 5 (1-8) x stats: per-stat genes, in BattleData::stats order.
   StatTraining,     ///< 8 (1-8) x stats: per-stat training values.
   StatAwaken,       ///< 8 (1-8) x stats: per-stat awakening values.
   Trait,            ///< 5 (1-8): index into the game's trait table.
   PassiveSelector,  ///< 4 (1-8): the passive slot in its low bits (as many as the manifest's passive slots need);
                     ///< the other bits are the game's.
   HeldItem,         ///< 16 (1-16): item index (0 = none).
   CurrentHp,        ///< 16 (1-16): storage for a battler resource (the hit points' persist role in party records).
   PrimaryStatus,    ///< 8: status ID (low 4 bits, 0 = none) and counter (high 4 bits).
   Friendship,       ///< Retired (a custom role now; was 8).
   Form,             ///< Retired (a custom role now; was 8).
   Nicknamed,        ///< Retired (a custom role now; was 1).
   Nickname,         ///< 8 x 12 (1-12): the TextLanguage role's code page, kCodeEnd-terminated when shorter.
   OwnerName,        ///< Retired (a custom role now; was 8 x 12).
   CaptureDevice,    ///< Retired (a custom role now; was 32).
   TextLanguage,     ///< 8 (1-8): language id of the nickname's code page (no transcoding).
   NUM_ROLES         ///< Number of roles.
};

/// A role's ID: an engine Role value, or kFirstCustomRole + a custom role's index.
using RoleId = uint8_t;

/// Shape of a role: element width in bits and element count.
struct RoleShape
{
   uint8_t width;   ///< Default bits per element (1-32).
   uint8_t count;   ///< Default elements (per-stat: the default six stats; per move slot: the default four slots).
};
CAPSULE_POD(RoleShape, 2);

/// Registry shapes, indexed by Role (frozen with the registry).
inline constexpr RoleShape kRoleShapes[static_cast<int>(Role::NUM_ROLES)] = {
   {32, 1}, {32, 1}, {16, 1}, {16, 1}, {24, 1}, {8, 1}, {16, 4}, {7, 4}, {2, 4},  {5, 6},  {8, 6},  {8, 6},
   {5, 1},  {4, 1},  {16, 1}, {16, 1}, {8, 1},  {8, 1}, {8, 1},  {1, 1}, {8, 12}, {8, 12}, {32, 1}, {8, 1},
};

/// Where a role's element count comes from (RoleChoice::count).
enum class RoleCount : uint8_t
{
   One,          ///< One element.
   PerStat,      ///< Layout::stat_count elements.
   PerMoveSlot,  ///< Layout::move_slots elements.
   Chosen,       ///< The layout's choice, 1 to RoleChoice::max_count (default RoleShape::count).
   Retired,      ///< A retired role (A10): no layout holds it; a custom role of its name takes its place.
   NUM_ROLE_COUNTS   ///< Number of rules.
};

/// What a layout may change about a role (D-20 C, D-23 decision 4).
struct RoleChoice
{
   RoleCount count;     ///< Where its element count comes from.
   uint8_t max_width;   ///< Widest element a layout may choose (RoleShape::width when it cannot choose).
   uint8_t max_count;   ///< RoleCount::Chosen: most elements; else RoleShape::count.
   uint8_t reserved;    ///< Padding; 0.
};
CAPSULE_POD(RoleChoice, 4);

/// Stats of the layout the registry's per-stat counts describe.
inline constexpr uint8_t kDefaultStatCount = 6;
/// Widest element of a per-stat role (battlers keep them in 8 bits).
inline constexpr uint8_t kMaxPerStatBits = 8;
/// Widest element of a role battlers keep in 8 bits (level, PP, trait, ...).
inline constexpr uint8_t kMaxByteRoleBits = 8;
/// Widest element of a role battlers keep in 16 bits (moves, held item, current HP).
inline constexpr uint8_t kMaxHalfRoleBits = 16;
/// Widest role element: a whole 32-bit value.
inline constexpr int kMaxRoleBits = 32;
/// Most move slots a layout may choose (battlers hold this many).
inline constexpr int kMaxMoveSlots = 8;
/// Move slots of a layout that names none (the registry's counts of Moves, Pp and PpUps).
inline constexpr uint8_t kDefaultMoveSlots = 4;
/// Most codes of the Nickname role (capsule::battle::kNameCodes; the registry's count).
inline constexpr uint8_t kMaxNicknameCodes = 12;
/// Most custom roles of a layout.
inline constexpr int kMaxCustomRoles = 32;
/// First custom role ID (custom role i has ID kFirstCustomRole + i).
inline constexpr RoleId kFirstCustomRole = 0x80;
/// "No role" (find_custom() of an unknown name).
inline constexpr RoleId kNoRole = 0xFF;

/// Registry choices, indexed by Role (frozen with the registry; ROLES, ROLE_MAX_WIDTHS and ROLE_MAX_COUNTS in
/// tools/battlec/layout.py).
inline constexpr RoleChoice kRoleChoices[static_cast<int>(Role::NUM_ROLES)] = {
   {RoleCount::One, kMaxRoleBits, 1, 0},             // SpeciesKey (fixed)
   {RoleCount::One, kMaxRoleBits, 1, 0},             // Seed
   {RoleCount::One, kMaxRoleBits, 1, 0},             // OwnerId
   {RoleCount::Retired, 16, 1, 0},                   // OwnerSecret
   {RoleCount::One, kMaxRoleBits, 1, 0},             // Experience
   {RoleCount::One, kMaxByteRoleBits, 1, 0},         // Level
   {RoleCount::PerMoveSlot, kMaxHalfRoleBits, 4, 0}, // Moves
   {RoleCount::PerMoveSlot, kMaxByteRoleBits, 4, 0}, // Pp
   {RoleCount::PerMoveSlot, kMaxByteRoleBits, 4, 0}, // PpUps
   {RoleCount::PerStat, kMaxPerStatBits, 6, 0},      // StatGenes
   {RoleCount::PerStat, kMaxPerStatBits, 6, 0},      // StatTraining
   {RoleCount::PerStat, kMaxPerStatBits, 6, 0},      // StatAwaken
   {RoleCount::One, kMaxByteRoleBits, 1, 0},         // Trait
   {RoleCount::One, kMaxByteRoleBits, 1, 0},         // PassiveSelector
   {RoleCount::One, kMaxHalfRoleBits, 1, 0},         // HeldItem
   {RoleCount::One, kMaxHalfRoleBits, 1, 0},         // CurrentHp
   {RoleCount::One, 8, 1, 0},                        // PrimaryStatus (fixed)
   {RoleCount::Retired, 8, 1, 0},                    // Friendship
   {RoleCount::Retired, 8, 1, 0},                    // Form
   {RoleCount::Retired, 1, 1, 0},                    // Nicknamed
   {RoleCount::Chosen, 8, kMaxNicknameCodes, 0},     // Nickname (width fixed)
   {RoleCount::Retired, 8, 12, 0},                   // OwnerName
   {RoleCount::Retired, kMaxRoleBits, 1, 0},         // CaptureDevice
   {RoleCount::One, kMaxByteRoleBits, 1, 0},         // TextLanguage
};

#if defined(CAPSULE_PLATFORM_GBA)
/// Largest record the engine stores (party slots are this size; build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxRecordBytes = 100;
/// Most members a party holds (party storage is this size; build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxPartySize = 6;
#else
/// Largest record the engine stores (party slots are this size; build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxRecordBytes = 256;
/// Most members a party holds (party storage is this size; build_settings.PLATFORM_LIMITS).
inline constexpr int kMaxPartySize = 8;
#endif
/// Party size of a layout that names none.
inline constexpr uint8_t kDefaultPartySize = 6;
/// Bit offset marking a role the layout does not hold.
inline constexpr uint16_t kAbsent = 0xFFFF;
/// "No move" in the Moves role (written truncated to the role's width: all bits set).
inline constexpr uint16_t kNoMove = 0xFFFF;
/// max_value() of a role kMaxRoleBits wide (every bit set).
inline constexpr uint32_t kMaxRoleValue = 0xFFFFFFFFu;

/// One custom role of the layout (ROM; D-23 §2 RoleSlot).
struct RoleSlot
{
   uint32_t name_hash;   ///< name_hash() of its name.
   uint32_t initial;     ///< Value of every element in a new record (reset(); the manifest's "default").
   uint16_t offset;      ///< First bit.
   uint8_t width;        ///< Bits per element (1-32).
   uint8_t count;        ///< Elements (1-255).
};
CAPSULE_POD(RoleSlot, 12);

/// The build's record layout (generated by the battle compiler).
struct Layout
{
   const uint16_t* offsets;   ///< Bit offset per engine role (NUM_ROLES entries), kAbsent if not held.
   const uint8_t* widths;     ///< Element width per engine role (NUM_ROLES entries), or nullptr for the registry's.
   const uint8_t* counts;     ///< Element count per engine role (NUM_ROLES entries), or nullptr for the
                              ///< registry's (per-stat roles: stat_count).
   const RoleSlot* custom;    ///< custom_count stored custom roles, then unstored_count unstored ones, or nullptr.
   uint32_t hash;             ///< Layout hash (FNV-1a 32; tools/battlec/layout.py compile_layout()): the held
                              ///< engine role IDs in order, their chosen widths and counts only where they differ
                              ///< from the registry, each custom role as (name hash, width, count), and the party
                              ///< size where it differs from kDefaultPartySize.
   uint16_t record_bytes;     ///< Bytes per record (<= kMaxRecordBytes).
   uint8_t stat_count;        ///< Elements of each per-stat role (the game's stats).
   uint8_t move_slots;        ///< Elements of each per-move-slot role (1 to kMaxMoveSlots).
   uint8_t party_size;        ///< Members a party holds (1 to kMaxPartySize).
   uint8_t custom_count;      ///< Stored custom roles (0 to kMaxCustomRoles).
   uint8_t unstored_count;    ///< Unstored custom roles after them (E13a; custom_count + unstored_count <=
                              ///< kMaxCustomRoles).
   uint8_t reserved[kPadAfterThreeBytes];   ///< Padding; 0.
   uint32_t record_hash;      ///< The record's shape alone: hash without the party size (C7; what the save image's
                              ///< party and boxes sections check, so a party that grows or shrinks keeps its saves).
};

/// The build's layout (generated; the engine's default holds no roles).
extern const Layout kLayout;

/// One record (party slot). Only the first kLayout.record_bytes bytes are used.
struct Record
{
   uint8_t bytes[kMaxRecordBytes];   ///< Packed role bits.
};
CAPSULE_POD(Record, kMaxRecordBytes);

/// A party: up to kLayout.party_size records (storage for kMaxPartySize).
struct Party
{
   Record members[kMaxPartySize];   ///< Records 0..count-1 are used.
   uint8_t count;                   ///< Members.
   uint8_t reserved[kPadAfterOneByte];   ///< Padding; 0.
};
CAPSULE_POD(Party, kMaxPartySize* kMaxRecordBytes + 4);

/// @name FNV-1a 32 (name_hash(); tools/battlec/layout.py FNV_OFFSET_BASIS, FNV_PRIME)
///@{
inline constexpr uint32_t kFnvOffsetBasis = 0x811C9DC5u;   ///< Offset basis.
inline constexpr uint32_t kFnvPrime = 0x01000193u;         ///< Prime.
///@}

/// The hash a custom role's name is found by (FNV-1a 32 over its bytes).
/// @param name  the role's name as the manifest declares it (NUL-terminated)
/// @return the hash (RoleSlot::name_hash)
constexpr uint32_t name_hash(const char* name)
{
   uint32_t h = kFnvOffsetBasis;
   for (const char* c = name; *c != '\0'; ++c)
   {
      h = (h ^ static_cast<uint8_t>(*c)) * kFnvPrime;
   }
   return h;
}

/// The ID of an engine role.
/// @param r  the role
/// @return its RoleId
constexpr RoleId id_of(Role r)
{
   return static_cast<RoleId>(r);
}

/// Bits of each RoleSet word.
inline constexpr int kRoleSetBits = 32;

/// A set of roles (E13a: the roles a transfer import filled, transfer.h import_record()): one bit per engine
/// role and one per custom role.
struct RoleSet
{
   uint32_t engine;   ///< Bit i: engine role i.
   uint32_t custom;   ///< Bit i: custom role kFirstCustomRole + i.
};
CAPSULE_POD(RoleSet, 8);
static_assert(static_cast<int>(Role::NUM_ROLES) <= kRoleSetBits && kMaxCustomRoles <= kRoleSetBits,
              "one RoleSet word per kind of role");

/// The bit of a role in its RoleSet word.
/// @param id  the role
/// @return the bit, or 0 for an ID no layout gives (kNoRole)
constexpr uint32_t role_bit(RoleId id)
{
   const int at = id >= kFirstCustomRole ? id - kFirstCustomRole : id;
   return at < kRoleSetBits ? uint32_t{1} << at : 0u;
}

/// Adds a role to a set.
/// @param set  the set
/// @param id   the role (an engine role or a custom role)
constexpr void add_role(RoleSet& set, RoleId id)
{
   (id >= kFirstCustomRole ? set.custom : set.engine) |= role_bit(id);
}

/// Whether a set holds a role.
/// @param set  the set
/// @param id   the role
/// @return true if it does
constexpr bool has_role(const RoleSet& set, RoleId id)
{
   return ((id >= kFirstCustomRole ? set.custom : set.engine) & role_bit(id)) != 0;
}

/// The custom role a layout declares under a name.
/// @param hash  name_hash() of its name
/// @return its ID, or kNoRole
RoleId find_custom(uint32_t hash);

/// Whether the layout holds a role.
/// @param id  the role
/// @return true if get()/set() reach real bits
bool has(RoleId id);

/// Reads one element of a role.
/// @param rec      the record
/// @param id       the role
/// @param element  element index (0 for single-element roles)
/// @return the value, or 0 if the layout does not hold the role (or the element)
uint32_t get(const Record& rec, RoleId id, int element = 0);

/// Writes one element of a role; the value is truncated to the role's width.
/// Does nothing if the layout does not hold the role (or the element).
/// @param rec      the record
/// @param id       the role
/// @param element  element index
/// @param value    new value
void set(Record& rec, RoleId id, int element, uint32_t value);

/// Element width of a role in the build's layout.
/// @param id  the role
/// @return bits per element (0 for an unknown custom role)
uint8_t width(RoleId id);

/// Elements of a role in the build's layout.
/// @param id  the role
/// @return element count (0 for an unknown custom role)
int count(RoleId id);

/// Highest value an element of a role can hold in the build's layout.
/// @param id  the role
/// @return 2^width - 1
uint32_t max_value(RoleId id);

/// The value a role's elements hold in a new record (a custom role's "default"; 0 for engine roles).
/// @param id  the role
/// @return the value
uint32_t initial(RoleId id);

/// Whether the layout holds an engine role.
/// @param r  the role
/// @return true if get()/set() reach real bits
inline bool has(Role r)
{
   return has(id_of(r));
}

/// Reads one element of an engine role.
/// @param rec      the record
/// @param r        the role
/// @param element  element index (0 for single-element roles)
/// @return the value, or 0 if the layout does not hold the role
inline uint32_t get(const Record& rec, Role r, int element = 0)
{
   return get(rec, id_of(r), element);
}

/// Writes one element of an engine role; the value is truncated to the role's width.
/// @param rec      the record
/// @param r        the role
/// @param element  element index
/// @param value    new value
inline void set(Record& rec, Role r, int element, uint32_t value)
{
   set(rec, id_of(r), element, value);
}

/// Writes a single-element engine role.
/// @param rec    the record
/// @param r      the role
/// @param value  new value
inline void set(Record& rec, Role r, uint32_t value)
{
   set(rec, id_of(r), 0, value);
}

/// Element width of an engine role in the build's layout.
/// @param r  the role
/// @return bits per element
inline uint8_t width(Role r)
{
   return width(id_of(r));
}

/// Elements of an engine role in the build's layout.
/// @param r  the role
/// @return element count
inline int count(Role r)
{
   return count(id_of(r));
}

/// Highest value an element of an engine role can hold in the build's layout.
/// @param r  the role
/// @return 2^width - 1
inline uint32_t max_value(Role r)
{
   return max_value(id_of(r));
}

/// The value an engine role's elements hold in a new record (always 0).
/// @param r  the role
/// @return the value
inline uint32_t initial(Role r)
{
   return initial(id_of(r));
}

/// @name Records of any layout
/// The functions above read the build's kLayout. These take the layout and the record's bytes, so a program can
/// read and write records of another game's layout (record transfers, capsule/monster/transfer.h, and tools);
/// the build's functions are these with kLayout.
///@{

/// Reads a bit field of a packed record (LSB first, P-19).
/// @param bytes  the record's bytes
/// @param bit    first bit
/// @param bits   width (1 to kMaxRoleBits)
/// @return the value
uint32_t read_bits(const uint8_t* bytes, uint32_t bit, uint8_t bits);

/// Writes a bit field of a packed record (LSB first); bits of @p value above @p bits are dropped.
/// @param bytes  the record's bytes
/// @param bit    first bit
/// @param bits   width (1 to kMaxRoleBits)
/// @param value  new value
void write_bits(uint8_t* bytes, uint32_t bit, uint8_t bits, uint32_t value);

/// The custom role a layout declares under a name.
/// @param layout  the layout
/// @param hash    name_hash() of its name
/// @return its ID, or kNoRole
RoleId find_custom(const Layout& layout, uint32_t hash);

/// Whether a layout holds a role.
/// @param layout  the layout
/// @param id      the role
/// @return true if get()/set() reach real bits
bool has(const Layout& layout, RoleId id);

/// Element width of a role in a layout.
/// @param layout  the layout
/// @param id      the role
/// @return bits per element (0 for an unknown custom role)
uint8_t width(const Layout& layout, RoleId id);

/// Elements of a role in a layout.
/// @param layout  the layout
/// @param id      the role
/// @return element count (0 for an unknown custom role)
int count(const Layout& layout, RoleId id);

/// Highest value an element of a role can hold in a layout.
/// @param layout  the layout
/// @param id      the role
/// @return 2^width - 1
uint32_t max_value(const Layout& layout, RoleId id);

/// The value a role's elements hold in a new record of a layout (a custom role's "default"; 0 for engine roles).
/// @param layout  the layout
/// @param id      the role
/// @return the value
uint32_t initial(const Layout& layout, RoleId id);

/// Reads one element of a role of a record of a layout.
/// @param layout   the layout
/// @param bytes    the record's bytes (layout.record_bytes)
/// @param id       the role
/// @param element  element index
/// @return the value, or 0 if the layout does not hold the role (or the element)
uint32_t get(const Layout& layout, const uint8_t* bytes, RoleId id, int element);

/// Writes one element of a role of a record of a layout; the value is truncated to the role's width.
/// @param layout   the layout
/// @param bytes    the record's bytes (layout.record_bytes)
/// @param id       the role
/// @param element  element index
/// @param value    new value
void set(const Layout& layout, uint8_t* bytes, RoleId id, int element, uint32_t value);

///@}

/// Clears a record (every role reads 0).
/// @param rec  the record
void clear(Record& rec);

/// Clears a record and gives every custom role element its initial() value (a new creature's record).
/// @param rec  the record
void reset(Record& rec);

} // namespace capsule::monster
