/// @file
/// Record transfer v2 (C-07a, D-23 Q9): moving a creature record between two
/// games whose record layouts differ.
///
/// A record's bytes only mean something to a game with the same layout, so a
/// transfer (trade, link, a save moved to another game) sends the
/// **canonical transfer record**: a deterministic byte string keyed by role
/// and stat, not by bit offset. Export writes it from a record of one layout;
/// import reads it into a record of another. tools/record_transfer.py is the
/// same format in Python (byte for byte), for the studio and the tools.
///
/// **Keys.** Engine roles are keyed by their role ID (frozen). A custom role
/// is keyed by transfer_key("role:" + its name), a stat by
/// transfer_key("stat:" + its transfer key) (the manifest's `transfer`,
/// default its name; FNV-1a 32). The compiler rejects layouts whose keys
/// collide (tools/battlec/layout.py and tools/battlec/transfer.py).
///
/// **Canonical form** (every multi-byte number little-endian, P-19):
///
/// | Bytes          | Field                                                                    |
/// |----------------|--------------------------------------------------------------------------|
/// | 1              | kTransferVersion                                                         |
/// | 1              | E: engine role IDs the presence mask covers (the exporter's NUM_ROLES)   |
/// | ceil(E / 8)    | presence mask: bit i (LSB first) set when engine role i is present       |
/// | 1              | C: custom roles, then C x 4: their keys, ascending                       |
/// | 1              | N: stats, then N x 4: their keys, ascending (the stat table)             |
/// | 2 per role     | width (1-32) and element count of each present role, in canonical order  |
/// | rest           | the values, bit-packed LSB first at each role's width, in canonical order |
///
/// Canonical order: the present engine roles by ID, then the custom roles by
/// key; a per-stat role's elements follow the stat table, other roles' their
/// element order (move slots by slot). The last byte is zero-padded. Two
/// layouts holding the same values in another order or at other offsets give
/// the same bytes.
///
/// **Raw form** (the fast path): when both games have the same transfer hash
/// (TransferInfo::hash: the layout hash and the stat keys), the record's own
/// bytes are sent: kTransferRawVersion, the 4-byte hash, then
/// Layout::record_bytes bytes. Importing a raw record of another hash fails
/// (TransferStatus::OtherLayout).
///
/// **Import rule** (into the receiver's layout):
/// - every role and element starts at its default: a custom role's initial
///   value; a per-stat role's TransferInfo::stat_defaults entry; the Moves and
///   Nickname roles all bits set ("no move", kCodeEnd); 0 otherwise;
/// - an incoming role the receiver holds (same engine ID or custom key) is
///   copied element by element: per-stat elements by stat key, others by
///   index;
/// - a value wider than the receiver's element is clamped to the receiver's
///   highest value (kTransferNarrowed); for Moves and Nickname the sender's
///   all-bits-set value becomes the receiver's (so "no move" stays "no move");
/// - incoming roles and stats the receiver does not hold are dropped
///   (kTransferDropped), and so are elements past the receiver's count unless
///   they hold the receiver's default;
/// - receiver roles, stats or elements without an incoming value keep their
///   default (kTransferDefaulted).
/// An import may also report which receiver roles the incoming record held
/// whole (a RoleSet; every held role for the raw form; C5: not a role an element
/// of was defaulted or narrowed): a battle game re-derives the derived roles the
/// sender did not give whole (capsule::battle::import_record(), E13a), since a
/// role computed from others is only as current as they are.
/// Move indices, species keys, statuses and the other values are copied as
/// numbers: a transfer between games with different move tables needs the
/// games to agree on them (or remap after import).
#pragma once

#include "capsule/monster/monster.h"

#include <cstdint>

namespace capsule::monster
{

/// Format version of the canonical form (first byte).
inline constexpr uint8_t kTransferVersion = 2;
/// First byte of the raw form (the version with the top bit set).
inline constexpr uint8_t kTransferRawVersion = 0x82;
/// Bytes of a key (custom role, stat, transfer hash).
inline constexpr int kTransferKeyBytes = 4;
/// Bytes of a present role's shape (width, count).
inline constexpr int kTransferShapeBytes = 2;
/// Bytes before the presence mask (version, engine role count).
inline constexpr int kTransferHeaderBytes = 2;
/// Bytes before a raw record's bytes (version, hash).
inline constexpr int kTransferRawHeaderBytes = 1 + kTransferKeyBytes;
/// Most stats a canonical record names that the build's bound counts (capsule::battle::kMaxStats is at most this).
inline constexpr int kMaxTransferStats = 32;
/// Engine roles (the presence mask's bits).
inline constexpr int kTransferEngineRoles = static_cast<int>(Role::NUM_ROLES);
/// Bytes of the presence mask this engine writes.
inline constexpr int kTransferMaskBytes =
   (kTransferEngineRoles + static_cast<int>(kBitsPerByte) - 1) / static_cast<int>(kBitsPerByte);
/// The first per-stat role (the per-stat roles are consecutive: genes, training, awakening).
inline constexpr Role kFirstPerStatRole = Role::StatGenes;
/// Per-stat roles (TransferInfo::stat_defaults holds this many per stat).
inline constexpr int kPerStatRoles = 3;
static_assert(kRoleChoices[static_cast<int>(kFirstPerStatRole)].count == RoleCount::PerStat &&
                 kRoleChoices[static_cast<int>(kFirstPerStatRole) + 1].count == RoleCount::PerStat &&
                 kRoleChoices[static_cast<int>(kFirstPerStatRole) + 2].count == RoleCount::PerStat,
              "the per-stat roles are consecutive");

/// @name Import flags (TransferResult::flags)
///@{
inline constexpr uint8_t kTransferNarrowed = 0x01;    ///< A value was clamped to a narrower element.
inline constexpr uint8_t kTransferDropped = 0x02;     ///< Incoming data had no place in the receiver's layout.
inline constexpr uint8_t kTransferDefaulted = 0x04;   ///< A receiver role, stat or element got no incoming value.
inline constexpr uint8_t kTransferRaw = 0x08;         ///< The raw form (record bytes copied as they are).
/// C9: the record is an egg (its role keyed kEggTransferKey is non-zero) and the receiver holds no role of that key:
/// a battle game refuses it or receives it hatched (capsule::battle::import_record()).
inline constexpr uint8_t kTransferEgg = 0x10;
///@}

/// What a game's layout means to a transfer (generated: tools/record_transfer.py emit, cmake
/// capsule_transfer_table(); the build's own is kTransfer).
struct TransferInfo
{
   const Layout* layout;          ///< The record layout.
   const uint32_t* stat_keys;     ///< Key of each stat (layout->stat_count), in the game's stat order.
   const uint8_t* stat_order;     ///< Stat indices by ascending key (the stat table's order).
   const uint32_t* custom_keys;   ///< Key of each custom role (layout->custom_count), in layout order.
   const uint8_t* custom_order;   ///< Custom role indices by ascending key.
   const uint8_t* stat_defaults;  ///< Per stat, kPerStatRoles values (genes, training, awakening) an import gives a
                                  ///< stat the sender does not have; nullptr for all 0.
   uint32_t hash;                 ///< Transfer hash (the raw form's key): the layout hash and the stat keys.
};

/// How a record is sent.
enum class TransferForm : uint8_t
{
   Canonical,   ///< The canonical form (any receiver).
   Raw,         ///< The record's bytes (a receiver with the same TransferInfo::hash).
   NUM_FORMS    ///< Number of forms.
};

/// Outcome of an export or import.
enum class TransferStatus : uint8_t
{
   Ok,            ///< Done.
   ShortBuffer,   ///< The output buffer (or the receiver's record) is too small.
   Truncated,     ///< The input ends early.
   BadVersion,    ///< The input is no form this engine reads.
   BadShape,      ///< The input is malformed (keys out of order, a width of 0 or over 32, a per-stat role whose
                  ///< count is not the stat table's, ...).
   OtherLayout,   ///< A raw record of another transfer hash.
   EggRefused,    ///< C9: an egg the receiving game does not take (capsule::battle::import_record(); kTransferEgg).
   NUM_STATUSES   ///< Number of statuses.
};

/// Result of export_record() / import_record().
struct TransferResult
{
   TransferStatus status;   ///< Outcome.
   uint8_t flags;           ///< Import: kTransferNarrowed, kTransferDropped, kTransferDefaulted, kTransferRaw.
   uint16_t bytes;          ///< Bytes written (export) or read (import).
};
CAPSULE_POD(TransferResult, 4);

/// The 32-bit key of a transfer name (FNV-1a 32 over "role:NAME" or "stat:KEY").
/// @param prefix  "role:" or "stat:" (NUL-terminated)
/// @param name    the name (NUL-terminated)
/// @return the key
constexpr uint32_t transfer_key(const char* prefix, const char* name)
{
   uint32_t h = kFnvOffsetBasis;
   for (const char* c = prefix; *c != '\0'; ++c)
   {
      h = (h ^ static_cast<uint8_t>(*c)) * kFnvPrime;
   }
   for (const char* c = name; *c != '\0'; ++c)
   {
      h = (h ^ static_cast<uint8_t>(*c)) * kFnvPrime;
   }
   return h;
}

/// The transfer name of the structure's egg role (C9, D-28 section 3: whatever the role is called in a layout, the
/// battle compiler keys it as transfer_key("role:", kEggTransferName), so every game knows an incoming egg).
inline constexpr const char* kEggTransferName = "egg";
/// The egg role's transfer key.
inline constexpr uint32_t kEggTransferKey = transfer_key("role:", kEggTransferName);

/// Bytes an export writes (exact).
/// @param info  the sender's layout
/// @param form  the form
/// @return bytes
int transfer_size(const TransferInfo& info, TransferForm form);

/// Most bytes a canonical record of a layout's size can take (any layout with at most kMaxTransferStats stats,
/// kMaxCustomRoles custom roles and records of @p record_bytes).
/// @param record_bytes  the layout's record size
/// @return bytes
constexpr int transfer_bound(int record_bytes)
{
   return kTransferHeaderBytes + kTransferMaskBytes + 1 + kMaxCustomRoles * kTransferKeyBytes + 1 +
          kMaxTransferStats * kTransferKeyBytes + (kTransferEngineRoles + kMaxCustomRoles) * kTransferShapeBytes +
          record_bytes;
}

/// Bytes enough for any record this platform's engine stores, in either form.
inline constexpr int kMaxTransferBytes = transfer_bound(kMaxRecordBytes);

/// The form to send to a receiver.
/// @param info       the sender's layout
/// @param peer_hash  the receiver's TransferInfo::hash
/// @return Raw when the hashes match, else Canonical
constexpr TransferForm form_for(const TransferInfo& info, uint32_t peer_hash)
{
   return peer_hash == info.hash ? TransferForm::Raw : TransferForm::Canonical;
}

/// Writes a record in a transfer form (no allocation).
/// @param info      the record's layout
/// @param record    the record's bytes (info.layout->record_bytes)
/// @param out       output buffer
/// @param capacity  its size in bytes
/// @param form      the form
/// @return Ok and the bytes written, or ShortBuffer (nothing meaningful written)
TransferResult export_record(const TransferInfo& info, const uint8_t* record, uint8_t* out, int capacity,
                             TransferForm form);

/// Reads a transferred record into a record of a layout (see the import rule above; no allocation).
/// The record is written only when the result is Ok.
/// @param info      the receiver's layout
/// @param in        the transferred bytes
/// @param size      their count (bytes past the record are ignored)
/// @param record    receives the record (its first info.layout->record_bytes bytes; the rest untouched)
/// @param capacity  the record buffer's size in bytes
/// @param received  set to the receiver roles the incoming record held whole (E13a; C5: no element defaulted or
///                  narrowed), or nullptr; written only when the result is Ok
/// @return status, import flags and the bytes read
TransferResult import_record(const TransferInfo& info, const uint8_t* in, int size, uint8_t* record, int capacity,
                             RoleSet* received = nullptr);

/// The build's transfer table (generated for kLayout by capsule_transfer_table(); a program that uses the
/// overloads below links one).
extern const TransferInfo kTransfer;

/// Writes a record of the build's layout in a transfer form.
/// @param rec       the record
/// @param out       output buffer
/// @param capacity  its size in bytes
/// @param form      the form
/// @return as export_record()
inline TransferResult export_record(const Record& rec, uint8_t* out, int capacity, TransferForm form)
{
   return export_record(kTransfer, rec.bytes, out, capacity, form);
}

/// Reads a transferred record into a record of the build's layout.
/// @param in    the transferred bytes
/// @param size  their count
/// @param rec   receives the record
/// @return as import_record()
inline TransferResult import_record(const uint8_t* in, int size, Record& rec)
{
   return import_record(kTransfer, in, size, rec.bytes, static_cast<int>(sizeof(rec.bytes)));
}

} // namespace capsule::monster
