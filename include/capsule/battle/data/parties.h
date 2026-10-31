/// @file
/// Parties (D-20 A, B): parties given as data, their members, AI profile (data/ai.h) and battle items (G3), and the
/// presenters that show them on screen.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/battle/data/moves.h"
#include "capsule/text/encode.h"

namespace capsule::seq
{
struct Sequence;
}

namespace capsule::battle
{

/// @name Caps (C-08a, REF-17)
///@{
inline constexpr int kMaxPresenters = 2;   ///< Presenters per party.
inline constexpr int kMaxPartyItems = 4;   ///< Items an AI party may use in battle (G3: PartyData::items).
///@}

/// A party member given as data (ROM).
struct MemberData
{
   uint16_t species;              ///< Species index.
   uint16_t item;                 ///< Held item index (kNone16 = none).
   uint16_t moves[kMaxMoveSlots]; ///< Moves, the first move_slots() used (all kNone16 = the learnset's last
                                  ///< move_slots() by level).
   uint8_t level;                ///< Level.
   uint8_t passive_slot;         ///< Passive slot (BattleData::passive_slots; the manifest names them).
   uint8_t genes;                ///< Gene value for every stat.
   uint8_t trait;                ///< Trait index.
};
CAPSULE_POD(MemberData, 24);

/// Party flag bits (PartyData::flags, Participant::flags; P-9 bit positions).
enum PartyFlag : uint8_t
{
   PARTY_UNOWNED,     ///< Creatures without an owner: each announces itself (Msg::Appeared), takes its own
                      ///< position, is never replaced and is not written back.
   PARTY_NO_ESCAPE,   ///< Foes of this party cannot run (can_escape()).
   NUM_PARTY_FLAGS    ///< Number of flags.
};

/// A party given as data (ROM): its members, presenters and rules.
struct PartyData
{
   const text::Code* name;          ///< Display name ({party}).
   const MemberData* members;       ///< Members, or nullptr (count 0: the participant brings records).
   uint16_t reward;                 ///< Reward base (the ruleset's reward(reward_base, level)).
   uint8_t count;                   ///< Members.
   uint8_t ai_profile;              ///< AI profile (BattleData::ai_profiles; manifest party `ai`, G1).
   uint8_t presenters[kMaxPresenters];   ///< Presenter indices (BattleData::presenters), kNone8 = none.
   uint8_t rank;                    ///< Rank passed to the ruleset as foe_rank (<= Rules::rank_max).
   uint8_t flags;                   ///< PartyFlag bits.
   /// Items it may use in battle, one use each (manifest party `items`; 0: none; its AI profile's item rules choose
   /// them, G3). Default 0, so a party a program writes itself needs no items.
   uint16_t items[kMaxPartyItems] = {};
};

/// How a presenter enters or leaves the screen.
enum class Motion : uint8_t
{
   None,       ///< At once.
   Slide,      ///< Slides from (or to) an edge.
   Fade,       ///< Fades in (or out) with the video blend.
   Sequence,   ///< A sequence plays it (PresenterData::enter_seq / exit_seq).
   Stay,       ///< Exit only: never leaves.
   NUM_MOTIONS ///< Number of motions.
};

/// The screen edge a slide starts from or ends at.
enum class Edge : uint8_t
{
   Side,     ///< The edge on the presenter's side of the screen.
   Left,     ///< Left edge.
   Right,    ///< Right edge.
   Top,      ///< Top edge.
   Bottom,   ///< Bottom edge.
   NUM_EDGES ///< Number of edges.
};

/// One entrance or exit of a presenter.
struct PresenterMotion
{
   Motion style;       ///< How it moves.
   uint8_t frames;     ///< Frames it takes (0 only with Motion::None).
   Edge from;          ///< Slide edge.
   uint8_t reserved;   ///< Padding; 0.
};
CAPSULE_POD(PresenterMotion, 4);

/// Presenter flag bits (PresenterData::flags; P-9 bit positions).
enum PresenterFlag : uint8_t
{
   PRESENTER_EXIT_AFTER_INTRO,   ///< Leaves right after the intro (else at its party's first send-out).
   PRESENTER_STAYS,              ///< Never leaves.
   PRESENTER_RETURNS,            ///< Comes back when its party is defeated or wins.
   NUM_PRESENTER_FLAGS           ///< Number of flags.
};

/// A presenter (ROM): who shows a party on screen and speaks for it. Purely
/// presentation: the battle only queues cues (EventKind::Presenter) and texts
/// (EventKind::PresenterText) for it.
struct PresenterData
{
   const text::Code* intro;            ///< Intro text, or nullptr.
   const text::Code* defeat;           ///< Text when its party is defeated, or nullptr.
   const text::Code* victory;          ///< Text when its party wins, or nullptr.
   const seq::Sequence* enter_seq;     ///< Entrance sequence (Motion::Sequence), or nullptr.
   const seq::Sequence* exit_seq;      ///< Exit sequence (Motion::Sequence), or nullptr.
   AssetKey sprite;                    ///< Sprite asset ("sprite:..."), or 0 (text only).
   PresenterMotion enter;              ///< Entrance.
   PresenterMotion exit;               ///< Exit.
   int8_t dx;                          ///< Offset from its side's stand (FormatData::stand_x).
   int8_t dy;                          ///< ...and stand_y.
   uint8_t flags;                      ///< PresenterFlag bits.
   uint8_t reserved;                   ///< Padding; 0.
};

} // namespace capsule::battle
