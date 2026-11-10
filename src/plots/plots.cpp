/// @file
/// M6a K2: plots (capsule/plots/plots.h; B-5 in CapsuleCreatures_Design.md) over the engine's records and the
/// economy clock (capsule/core/clock.h).
///
/// A record holds the crop and the economy second it was planted; everything else is worked out from the time since
/// (where()): a cycle is the type's stages of crop.stage_hours each, then its ripe_hours ripe; the crop goes through
/// 1 + the type's regrow cycles, then the plot is withered (empty). A type whose ripe_hours is 0 stays ripe until
/// picked (one cycle). A record's start_stage counts as stages already grown (a new game's ripe crops).
///
/// The operations a game with its own storage replaces are weak (CAPSULE_WEAK): see the header.
#include "capsule/plots/plots.h"

#include "capsule/core/clock.h"
#include "capsule/core/memory.h"

namespace capsule::plots
{

namespace
{

constexpr unsigned kLocalBits = 8;   ///< A place's sort key: its map above its local ID's bits.
constexpr uint16_t kHalf = 2;        ///< The binary search halves the range.

/// Where a plot's crop is now.
struct Where
{
   uint8_t stage = kStageEmpty;   ///< Its stage (the type's stages: ripe), or kStageEmpty.
   uint8_t cycle = 0;             ///< Its cycle (0: the first growth).
};

/// A plot's record (engine storage), or nullptr: no such plot, or no engine records.
/// @param id  the plot
/// @return it
PlotRecord* record(uint16_t id)
{
   const Data* d = data();
   if (d == nullptr || d->records == nullptr || id >= d->place_count)
   {
      return nullptr;
   }
   return &d->records[id];
}

/// A record's crop data, or nullptr (empty, or a crop the world does not have).
/// @param r  the record
/// @return it
const CropData* crop_of(const PlotRecord& r)
{
   const Data* d = data();
   if (r.crop == kNoCrop || r.crop > d->crop_count)
   {
      return nullptr;
   }
   return &d->crops[r.crop - 1];
}

/// Where a record's crop is now (see the file comment).
/// @param r  the record
/// @return its stage and cycle
Where where(const PlotRecord& r)
{
   Where w{};
   const CropData* c = crop_of(r);
   if (c == nullptr)
   {
      return w;
   }
   const TypeData& t = data()->types[c->type];
   const uint32_t stage_seconds = uint32_t{c->stage_hours} * clock::kSecondsPerHour;
   const uint32_t grow = t.stages * stage_seconds;
   const uint32_t now = clock::economy_seconds();
   const uint32_t since = now > r.planted ? now - r.planted : 0;
   const uint32_t at = since + r.start_stage * stage_seconds;   // seconds into its growth
   if (t.ripe_hours == 0)
   {
      w.stage = at >= grow ? t.stages : static_cast<uint8_t>(at / stage_seconds);   // ripe until picked
      return w;
   }
   const uint32_t cycle_seconds = grow + uint32_t{t.ripe_hours} * clock::kSecondsPerHour;
   const uint32_t cycle = at / cycle_seconds;
   if (cycle > t.regrow)
   {
      return w;   // withered: the last cycle's ripe time is over
   }
   const uint32_t into = at % cycle_seconds;
   w.cycle = static_cast<uint8_t>(cycle);
   w.stage = into >= grow ? t.stages : static_cast<uint8_t>(into / stage_seconds);
   return w;
}

/// The stages of a record's current cycle that were watered.
/// @param r  the record
/// @param w  where its crop is
/// @param t  its type
/// @return how many
uint8_t watered_stages(const PlotRecord& r, const Where& w, const TypeData& t)
{
   if (r.cycle != w.cycle)
   {
      return 0;   // the bits belong to an earlier cycle
   }
   uint8_t n = 0;
   for (uint8_t s = 0; s < t.stages; ++s)
   {
      n = static_cast<uint8_t>(n + ((r.watered >> s) & 1u));
   }
   return n;
}

/// Plants a record afresh.
/// @param r      the record
/// @param crop   crop index + 1
/// @param ahead  stages already grown
void plant_record(PlotRecord& r, uint16_t crop, uint8_t ahead)
{
   r = PlotRecord{};
   r.crop = crop;
   r.planted = clock::economy_seconds();
   r.start_stage = ahead;
}

} // namespace

void begin_new()
{
   const Data* d = data();
   if (d == nullptr || d->records == nullptr)
   {
      return;
   }
   for (uint16_t i = 0; i < d->place_count; ++i)
   {
      PlotRecord& r = d->records[i];
      r = PlotRecord{};
      const uint16_t start = d->places[i].start_crop;
      if (start != kNoCrop && start <= d->crop_count)
      {
         plant_record(r, start, d->types[d->crops[start - 1].type].stages);   // ripe from the start
      }
   }
}

CAPSULE_WEAK uint8_t state(uint16_t id)
{
   const PlotRecord* r = record(id);
   return r != nullptr ? where(*r).stage : kStageEmpty;
}

CAPSULE_WEAK uint16_t crop(uint16_t id)
{
   const PlotRecord* r = record(id);
   if (r == nullptr || where(*r).stage == kStageEmpty)
   {
      return kNoCrop;
   }
   return r->crop;
}

CAPSULE_WEAK uint8_t yield(uint16_t id)
{
   const PlotRecord* r = record(id);
   if (r == nullptr)
   {
      return 0;
   }
   const Where w = where(*r);
   const CropData* c = crop_of(*r);
   if (c == nullptr)
   {
      return 0;
   }
   const TypeData& t = data()->types[c->type];
   if (w.stage != t.stages)
   {
      return 0;   // not ripe (or withered)
   }
   if (t.water == 0 || c->yield_max <= c->yield_min)
   {
      return c->yield_min;
   }
   // the minimum, plus the range times the share of this cycle's stages watered
   const uint32_t range = c->yield_max - c->yield_min;
   return static_cast<uint8_t>(c->yield_min + range * watered_stages(*r, w, t) / t.stages);
}

CAPSULE_WEAK bool plant(uint16_t id, uint16_t crop)
{
   PlotRecord* r = record(id);
   const Data* d = data();
   if (r == nullptr || crop == kNoCrop || crop > d->crop_count || where(*r).stage != kStageEmpty)
   {
      return false;
   }
   if (d->crops[crop - 1].type != d->places[id].type)
   {
      return false;   // a crop of another plot type
   }
   plant_record(*r, crop, 0);
   return true;
}

CAPSULE_WEAK bool water(uint16_t id)
{
   PlotRecord* r = record(id);
   if (r == nullptr)
   {
      return false;
   }
   const Where w = where(*r);
   const CropData* c = crop_of(*r);
   if (c == nullptr || w.stage == kStageEmpty)
   {
      return false;
   }
   const TypeData& t = data()->types[c->type];
   if (t.water == 0 || w.stage >= t.stages)
   {
      return false;   // it does not count watering, or it is ripe
   }
   if (r->cycle != w.cycle)
   {
      r->cycle = w.cycle;   // a new cycle: the bits start over
      r->watered = 0;
   }
   r->watered = static_cast<uint8_t>(r->watered | (1u << w.stage));
   return true;
}

CAPSULE_WEAK uint8_t pick(uint16_t id)
{
   PlotRecord* r = record(id);
   const uint8_t n = yield(id);
   if (r == nullptr || n == 0)
   {
      return 0;
   }
   const CropData* c = crop_of(*r);
   if (data()->types[c->type].replant != 0)
   {
      plant_record(*r, r->crop, 0);   // a tree grows again from its first stage
   }
   else
   {
      *r = PlotRecord{};
   }
   return n;
}

CAPSULE_WEAK void clear(uint16_t id)
{
   PlotRecord* r = record(id);
   if (r != nullptr)
   {
      *r = PlotRecord{};
   }
}

uint16_t plot_at(uint16_t map, uint8_t local)
{
   const Data* d = data();
   if (d == nullptr)
   {
      return kNoPlot;
   }
   // the places are sorted by map, then local ID: a binary search
   const uint32_t want = (uint32_t{map} << kLocalBits) | local;
   uint16_t lo = 0;
   uint16_t hi = d->place_count;
   while (lo < hi)
   {
      const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / kHalf);
      const Place& p = d->places[mid];
      const uint32_t have = (uint32_t{p.map} << kLocalBits) | p.local;
      if (have == want)
      {
         return mid;
      }
      if (have < want)
      {
         lo = static_cast<uint16_t>(mid + 1);
      }
      else
      {
         hi = mid;
      }
   }
   return kNoPlot;
}

bool look(uint16_t map, uint8_t local, AssetKey& sprite, uint8_t& anim)
{
   const uint16_t id = plot_at(map, local);
   if (id == kNoPlot)
   {
      return false;
   }
   const Data* d = data();
   const uint8_t stage = state(id);
   const uint16_t c = stage == kStageEmpty ? kNoCrop : crop(id);
   if (c == kNoCrop || c > d->crop_count)
   {
      sprite = d->types[d->places[id].type].sprite;   // empty: the type's sprite, or nothing
      anim = 0;
      return true;
   }
   sprite = d->crops[c - 1].sprite;
   anim = stage;
   return true;
}

} // namespace capsule::plots
