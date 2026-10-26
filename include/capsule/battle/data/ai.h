/// @file
/// AI profiles (D-26 section 1, G1): how an AI party chooses its actions, as manifest data. A party names a
/// profile (battle.json `ai_profiles`, a party's `ai`); a profile is an ordered list of scoring rules, run for
/// every usable move and target, an ordered list of switching rules, run once the best move is known, and an
/// ordered list of item-use rules (G3).
///
/// Scoring rules add to a move's score or veto it: built-in terms with their numbers as data (a draw, the
/// expected damage, statuses, costs), the ruleset pipeline `ai_score` and the effect hook `ai_score` of the move,
/// the user's owners and the target's owners. Every rule but the draw is pure (no draw, no event, no change).
/// Item-use rules (G3) run after the switching rules: the first that says so uses one of the party's items
/// (PartyData::items) on its active battler as the turn's Item action.
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"

#include <cstdint>

namespace capsule::battle
{

/// @name Caps (G1)
///@{
inline constexpr int kMaxAiProfiles = 32;   ///< Profiles per battle data (8-bit ids; the compiler checks it).
inline constexpr int kMaxAiRules = 16;      ///< Scoring rules and switching rules per profile, each.
///@}

/// What an AI rule does (AiRuleData::kind; tools/battlec/ai.py RULES and SWITCH_RULES mirror the order).
enum class AiRuleKind : uint8_t
{
   Noise,           ///< + a draw below `arg` (the tiebreak; the only rule that draws).
   Damage,          ///< + per hit battler: its expected damage (a knock-out: its HP + `arg2`) x the move's accuracy
                    ///< percent x `arg` (an ally's counts against).
   AvoidUseless,    ///< Veto: the move has no effect (a decisive Zero tier) on a foe it would hit.
   Status,          ///< + `arg` per status the move inflicts, for each foe it would hit that has none (status moves).
   StatusMove,      ///< + `arg` for a move that deals no damage.
   SaveResources,   ///< - the share of each pool the move's costs spend, scaled so a whole pool costs `arg` (a score
                    ///< never goes below 0 through it).
   Pipeline,        ///< + the ruleset pipeline ai_score's `score`; veto when it is below `arg` (kAiNoVeto: never).
   Hook,            ///< The effect hook ai_score: the move's own handler, then the user's owners (holder = user),
                    ///< then the target's (holder = target); value = the score (modify), block = veto.
   SwitchBelow,     ///< Switching: switch out when the best move's score is below `arg`.
   SwitchHook,      ///< Switching: the ai_score hook of the user's owners with move none (value 0 in): switch out
                    ///< when it ends above the best move's score.
   ItemBelowHp,     ///< Item use (G3): use item `arg2` when the battler's HP is below `arg` percent of its maximum.
   ItemHook,        ///< Item use (G3): the ai_score hook of item `arg2`'s own logic (holder = the battler, move none,
                    ///< value 0 in): use it when it ends above the best move's score + `arg` (block: never).
   NUM_KINDS        ///< Number of kinds.
};

/// AiRuleData::arg of a Pipeline rule that never vetoes.
inline constexpr int16_t kAiNoVeto = INT16_MIN;

/// One AI rule (ROM).
struct AiRuleData
{
   AiRuleKind kind;    ///< What it does.
   uint8_t reserved;   ///< Padding; 0.
   int16_t arg;        ///< Its number (see AiRuleKind).
   int16_t arg2;       ///< Its second number (Damage: the knock-out bonus), else 0.
};
CAPSULE_POD(AiRuleData, 6);

/// An AI profile (ROM): its rules in BattleData::ai_rules (its scoring rules, its switching rules from first_switch,
/// then its item-use rules from first_switch + switch_count).
struct AiProfileData
{
   uint16_t first_rule;     ///< First scoring rule.
   uint16_t first_switch;   ///< First switching rule.
   uint8_t rule_count;      ///< Scoring rules (<= kMaxAiRules).
   uint8_t switch_count;    ///< Switching rules (<= kMaxAiRules).
   uint8_t item_count;      ///< Item-use rules (G3; <= kMaxAiRules), right after its switching rules.
   uint8_t reserved;        ///< Padding; 0.
};
CAPSULE_POD(AiProfileData, 8);

} // namespace capsule::battle
