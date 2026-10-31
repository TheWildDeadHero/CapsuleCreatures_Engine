/// @file
/// Battle data (C-08, P-1): the ROM tables the battle compiler generates.
///
/// Everything a battle does that is content lives here as data: types and
/// the type chart with its tiers, the stages, move categories, statuses, weather, moves, passives and
/// held items with their effect logic (one bank, capsule/battle/effect.h), creatures
/// (species), the stats, growth tables, traits, formats, parties with their presenters
/// and message texts. The rules behind stats, experience, rewards and
/// escape are ruleset formulas (capsule/battle/ruleset.h); damage, speed,
/// accuracy and critical hits are ruleset pipelines whose modifier points
/// (data/points.h) run point handlers (capsule/battle/pipeline.h). The engine library carries an empty default
/// (battle::kData) that a build's generated table replaces at link time.
///
/// Fixed point is Q8 (P-4); chances are integer odds (P-5).
///
/// The tables are declared in parts (capsule/battle/data/: common, chart,
/// creatures, moves, conditions, formats, parties, messages, rules, points, resources, ai, recruit); this
/// header includes every part and adds BattleData, which holds them all.
#pragma once

#include "capsule/battle/data/ai.h"
#include "capsule/battle/data/chart.h"
#include "capsule/battle/data/common.h"
#include "capsule/battle/data/conditions.h"
#include "capsule/battle/data/creatures.h"
#include "capsule/battle/data/formats.h"
#include "capsule/battle/data/messages.h"
#include "capsule/battle/data/moves.h"
#include "capsule/battle/data/parties.h"
#include "capsule/battle/data/points.h"
#include "capsule/battle/data/recruit.h"
#include "capsule/battle/data/resources.h"
#include "capsule/battle/data/rules.h"
#include "capsule/battle/effect.h"

#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::battle
{

/// The whole battle data table (ROM).
struct BattleData
{
   const TypeData* types;       ///< Types.
   const uint8_t* chart;       ///< type_count x type_count tier indices (BattleData::tiers),
                               ///< [attacker * n + defender].
   const TierData* tiers;      ///< The chart's tiers (tier_count).
   const StatusData* statuses;    ///< Statuses; index 0 is "none" (unused entry).
   const WeatherData* weathers;    ///< Weathers.
   const VolatileData* volatiles;  ///< Volatiles (Battler::vol bit order).
   const SideCondData* side_conds; ///< Side conditions (SideState::cond bit order).
   const FieldCondData* field_conds;   ///< Field conditions (FieldState::cond bit order).
   const MoveData* moves;       ///< Moves.
   const HolderData* passives;    ///< Passives.
   const HolderData* items;       ///< Held items; index 0 is "none".
   const SpeciesData* species;     ///< Species, sorted by key (binary search).
   const uint32_t* const* growth;  ///< Experience tables: growth[g][level] (level_max + 1 entries).
   const TraitData* traits;       ///< Traits.
   const FormatData* formats;     ///< Formats.
   const PartyData* parties;      ///< Parties.
   const PresenterData* presenters;   ///< Presenters.
   const CategoryData* categories;    ///< Move categories.
   /// Per type: the category of a damaging move that hits with this type though it is not its own (the move_type
   /// point changed it; structure hit_category_by_type or category_by_type, C5), or nullptr: the move's own.
   const uint8_t* hit_categories;
   const StatInfo* stats;             ///< Permanent stats (stat_count).
   const text::Code* const* messages;   ///< message_count texts by message id (Msg names the engine's).
   const StageData* stages;     ///< Stages (stage_count; Battler::stages order).
   const PointData* points;     ///< Modifier points (point_count; ids are the ruleset's declaration order).
   const text::Code* const* texts;   ///< Owner texts (effect `text`; text_count).
   const ResourceData* resources;    ///< Resources (resource_count; D-25), in the order costs are paid.
   const MoveCost* move_costs;       ///< Every move's costs (MoveData::cost_first / cost_count).
   effect::Bank effects;        ///< Effect logic: one owner per move, passive, item, status, weather, volatile,
                                ///< side condition and field condition.
   Rules rules;                 ///< Non-formula rules.
   uint16_t move_count;            ///< Moves.
   uint16_t species_count;         ///< Species.
   uint16_t item_count;            ///< Items (including "none").
   uint16_t party_count;           ///< Parties.
   uint16_t text_count;            ///< Owner texts.
   uint8_t type_count;            ///< Types.
   uint8_t status_count;          ///< Statuses (including "none").
   uint8_t weather_count;         ///< Weathers.
   uint8_t volatile_count;        ///< Volatiles (<= kMaxVolatiles).
   uint8_t side_cond_count;       ///< Side conditions (<= kMaxSideConds).
   uint8_t field_cond_count;      ///< Field conditions (<= kMaxFieldConds).
   uint8_t status_bits;           ///< Bits of the PrimaryStatus role that hold the status id (the layout's
                                  ///< status_bits; the rest hold Battler::status_counter).
   uint8_t passive_count;         ///< Passives.
   uint8_t growth_count;          ///< Growth tables.
   uint8_t trait_count;           ///< Traits.
   uint8_t format_count;          ///< Formats.
   uint8_t presenter_count;       ///< Presenters.
   uint8_t category_count;        ///< Move categories.
   uint8_t stat_count;            ///< Stats (<= kMaxStats).
   uint8_t pool_stat;             ///< The stat that is hit points (max HP).
   uint8_t order_stat;            ///< The stat that orders turns (speed).
   uint8_t stage_count;           ///< Stages (<= kMaxStages).
   uint8_t point_count;           ///< Modifier points (<= kMaxPoints).
   uint8_t tier_count;            ///< Chart tiers (<= kMaxTiers).
   uint8_t combine;               ///< Combine: how the scaling tiers of a defender's types combine.
   uint8_t species_types;         ///< Types a species has (SpeciesData::types used; <= kMaxTypesPer).
   uint8_t added_types;           ///< Types effects may add on top (species_types + added_types <= kMaxTypesPer).
   uint8_t above_message;         ///< Msg said when the combined factor is above x1 (chart.messages.above), or
                                  ///< kNone8.
   uint8_t below_message;         ///< Msg said when it is below x1 (chart.messages.below), or kNone8.
   uint8_t accuracy_stage;        ///< The Accuracy stage, or kNone8 (the accuracy pipeline then sees 0).
   uint8_t evasion_stage;         ///< The Evasion stage, or kNone8.
   uint8_t crit_stage;            ///< The Crit stage, or kNone8.
   uint8_t stat_stage;            ///< The stat stage whose table every stat stage shares (pipeline staged()), or
                                  ///< kNone8 when they differ.
   uint8_t resource_count;        ///< Resources (<= kMaxResources).
   uint8_t faint_resource;        ///< The resource that faints when empty (the hit points), or kNone8.
   uint8_t move_resource_count;   ///< Move resources (Battler::move_pool used; <= kMaxMoveResources).
   uint8_t odds_stat;             ///< The stat rules::effect_odds() reads for both battlers (structure stats[].odds,
                                  ///< E9), or kNone8.
   uint8_t passive_slots;         ///< Passive slots per species (structure passive_slots; <= kMaxPassiveSlots).
   uint8_t passive_rolled;        ///< Bit per passive slot a new record may roll (generation passive_slot).
   uint8_t passive_selector_mask; ///< PassiveSelector bits that hold the slot (enough for passive_slots).
   uint8_t message_count;         ///< Messages: the engine's Msg::NUM_MSGS ids, then the battle's own (C4).
   const AiProfileData* ai_profiles;     ///< AI profiles (ai_profile_count; battle.json ai_profiles, G1).
   const AiRuleData* ai_rules;           ///< Their scoring and switching rules (AiProfileData::first_rule ...).
   const DelayedHitData* delayed_hits;   ///< Delayed hits (delayed_hit_count; structure delayed_hits, G1).
   uint8_t ai_profile_count;      ///< AI profiles (>= 1: a manifest without them gets the default one).
   uint8_t ai_default;            ///< The profile of a party that names none (a party of records, an unowned one).
   uint8_t delayed_hit_count;     ///< Delayed hits.
   /// Tag bits (structure weather_suppress_tag) whose holders suppress the weather while they are on the field
   /// (C6: a weather-calming passive): the weather stays and counts its turns, but its handlers, points and residual do
   /// not act and field.weather reads none. 0: nothing suppresses it.
   uint16_t weather_suppress_tags;
   /// The moves `teach` may give each species (C6, review4 finding 23; species `teachable`): species k's are
   /// teach_moves[teach_first[k]] to teach_moves[teach_first[k + 1] - 1], sorted. nullptr: no species lists them
   /// and any move can be taught.
   const uint16_t* teach_first;
   const uint16_t* teach_moves;   ///< The listed moves, species by species (teach_first).
};

/// The build's battle data (generated; the engine's default is empty).
extern const BattleData kData;

/// Finds a species by its key.
/// @param key  "species:NAME" key
/// @return species index, or kNone16
uint16_t find_species(AssetKey key);

/// Stage of a stat.
/// @param stat  stat index
/// @return its Battler::stages index, or kNone8 if it has none
inline uint8_t stage_of(uint8_t stat)
{
   return stat < kData.stat_count ? kData.stats[stat].stage : kNone8;
}

/// Type chart tier of one attacking type against one defending type.
/// @param attacker  type index (< type_count)
/// @param defender  type index (< type_count)
/// @return the tier's index (BattleData::tiers)
inline uint8_t chart(uint8_t attacker, uint8_t defender)
{
   return kData.chart[attacker * kData.type_count + defender];
}

} // namespace capsule::battle
