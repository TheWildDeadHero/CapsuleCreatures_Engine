/// @file
/// Recruitment (D-26 section 3, package G3): the structure's `recruit` section as data. An item's use_battle logic
/// calls the effect primitive `recruit WHO BONUS`; the engine then makes the manifest's number of checks, each a
/// draw against the ruleset pipeline recruit_odds (capsule/battle/pipeline.h), shows an event per check, and on
/// success ends the battle with Outcome::Recruited: the battler's record (with the battle changes this data keeps)
/// joins the player's party, else the storage boxes (capsule/storage/storage.h).
///
/// Every rule here is the manifest's: the checks, the species field fed to the pipeline as its recruit rate, the
/// bonus of each primary status, whether an owned battler can be recruited, which battle changes the record keeps,
/// the record role that stores the item used, and the battle messages each moment says.
///
/// Not part of BattleData: the table is the build's capsule::battle::kRecruit (generated with the battle data; the
/// engine's default has no checks, so the primitive fails).
#pragma once

#include "capsule/battle/data/common.h"

#include <cstdint>

namespace capsule::battle
{

/// @name Caps (G3)
///@{
inline constexpr int kMaxRecruitChecks = 8;                       ///< Checks a recruit makes at most.
inline constexpr int kMaxBreakFreeMessages = kMaxRecruitChecks;   ///< "Broke free" messages, by checks passed.
///@}

/// The battle changes a recruited record keeps (RecruitData::keep bits; P-9 bit positions). What it does not keep is
/// reset: its pools full (fill_resources()), no primary status.
enum RecruitKeep : uint8_t
{
   RECRUIT_KEEP_HP,          ///< Its hit points (else full).
   RECRUIT_KEEP_STATUS,      ///< Its primary status (else none).
   RECRUIT_KEEP_RESOURCES,   ///< Its other persisted pools and raises (else full).
   NUM_RECRUIT_KEEPS         ///< Number of bits.
};

/// RecruitData::flags bits (P-9 bit positions).
enum RecruitFlag : uint8_t
{
   RECRUIT_OWNED,             ///< A battler of an owned party may be recruited (else the primitive refuses it).
   RECRUIT_CONSUME_REFUSED,   ///< C6: refusing an owned battler still uses the item up (structure refusal_consumes).
   RECRUIT_CONSUME_BLOCKED,   ///< C6: a battle that forbids recruiting still uses the item up.
   RECRUIT_CONSUME_NO_ROOM,   ///< C6: no room in the party or the boxes still uses the item up.
   NUM_RECRUIT_FLAGS          ///< Number of flags.
};

/// The moments a recruit shows a battle message for (RecruitData::messages; EventKind::Recruit extras).
enum class RecruitMoment : uint8_t
{
   Check,       ///< A check passed (value: the checks passed so far).
   Recruited,   ///< Every check passed.
   BrokeFree,   ///< A check failed (value: the checks passed; RecruitData::break_free gives its message).
   Refused,     ///< An owned battler (RECRUIT_OWNED not set).
   Blocked,     ///< The battle forbids recruiting (SETUP_NO_RECRUIT).
   NoRoom,      ///< Neither the party nor the storage boxes have room.
   ToParty,     ///< The record joined the party.
   ToStorage,   ///< The record went to a storage box (value: the box, 1 = the first).
   NUM_MOMENTS  ///< Number of moments.
};
/// Number of recruit moments.
inline constexpr int kRecruitMoments = static_cast<int>(RecruitMoment::NUM_MOMENTS);

/// The recruit rules (ROM; structure `recruit`).
struct RecruitData
{
   const uint8_t* status_bonus;        ///< The bonus of each primary status (status_count entries; 0: none), or
                                       ///< nullptr (0 for all).
   uint8_t checks;                     ///< Checks a recruit makes (0: the battle has no recruit section).
   uint8_t rate_field;                 ///< The species field read as the recruit rate (species_fields), or kNone8.
   uint8_t keep;                       ///< RecruitKeep bits.
   uint8_t flags;                      ///< RecruitFlag bits.
   uint8_t item_role;                  ///< The record role (monster::RoleId) that stores the item used, or kNone8.
   uint8_t break_free_count;           ///< "Broke free" messages given (break_free).
   uint8_t messages[kRecruitMoments];  ///< Message id per moment (BattleData::messages), or kNone8: no text
                                       ///< (BrokeFree's stays kNone8: break_free gives it).
   /// The message a failed recruit says, by the checks it passed (the last one for more), or kNone8.
   uint8_t break_free[kMaxBreakFreeMessages];
};

/// The build's recruit rules (generated with the battle data; the engine's default has no checks).
extern const RecruitData kRecruit;

} // namespace capsule::battle
