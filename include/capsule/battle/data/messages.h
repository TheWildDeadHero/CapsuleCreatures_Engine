/// @file
/// Engine messages: the texts the battle itself shows (BattleData::messages, indexed by message id).
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include <cstdint>

namespace capsule::battle
{

/// The messages the engine says itself, by their ids in BattleData::messages (the battle compiler's
/// battlec.texts.MESSAGES order; tests/test_battle_compiler.py checks the values). The ids between them belong to
/// optional messages only data names (effect logic's `say`, the chart's and tiers' messages, condition texts:
/// "super_effective", "confused", ...) and the battle's own messages follow NUM_MSGS (BattleData::message_count
/// holds them all); the engine never names those (C4).
/// Placeholders: see text::kCodeArgUser ...
enum class Msg : uint8_t
{
   Appeared = 0,
   PartyChallenge = 1,
   SendOut = 2,
   PartySends = 3,
   ComeBack = 4,
   UsedMove = 5,
   Missed = 6,
   NoEffect = 7,
   Critical = 10,
   Fainted = 12,
   StatUp = 13,
   StatUp2 = 14,
   StatDown = 15,
   StatDown2 = 16,
   StatMax = 17,
   StatMin = 18,
   Failed = 31,
   NoTarget = 32,
   Passive = 33,
   ItemUsed = 34,
   Absorbed = 35,
   ExpGained = 36,
   LevelUp = 37,
   Learned = 38,
   NotLearned = 39,
   GotAway = 40,
   CantEscape = 41,
   EscapeBlocked = 42,
   PartyDefeated = 43,
   Reward = 44,
   OutOfCreatures = 45,
   NoUsableMove = 46,   ///< No move's cost can be paid and there is no fallback move (D-25).
   Blocked = 47,
   NUM_MSGS = 48        ///< The engine's message ids (the optional ones among them); a battle's own follow.
};

} // namespace capsule::battle
