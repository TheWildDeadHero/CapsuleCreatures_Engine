/// @file
/// Door animations on the shown map (see capsule/field/doors.h).
#include "capsule/field/doors.h"

#include "capsule/core/memory.h"
#include "capsule/map/map.h"

namespace capsule::field::doors
{
namespace
{

/// Words of one 4bpp tile.
constexpr uint32_t kTileWords = 8;

/// One running door.
struct Run
{
   const asset::Door* door = nullptr;     ///< Its asset (nullptr: the entry is free).
   const asset::DoorStep* steps = nullptr;   ///< Its opening or closing.
   uint8_t count = 0;                     ///< Steps.
   uint8_t step = 0;                      ///< The step shown.
   uint8_t counter = 0;                   ///< Display frames into it.
   int16_t x = 0;                         ///< Anchor column.
   int16_t y = 0;                         ///< Anchor row.
};

Run g_runs[kMaxRunning];   ///< The running doors.

/// Draws one frame of a door on its cells, or the closed door.
/// @param d      the door
/// @param frame  the frame, or asset::kDoorClosed
/// @param x      anchor column (bottom-left cell)
/// @param y      anchor row
void draw(const asset::Door& d, uint8_t frame, int x, int y)
{
   const map::Map* m = map::current();
   if (m == nullptr)
   {
      return;
   }
   const int top = y - (d.height - 1);   // the anchor is the bottom-left cell
   if (frame == asset::kDoorClosed || frame >= d.frame_count)
   {
      for (int cy = 0; cy < d.height; ++cy)
      {
         for (int cx = 0; cx < d.width; ++cx)
         {
            map::clear_cell_override(x + cx, top + cy);
         }
      }
      return;
   }
   if (d.show == asset::DoorShow::Tiles && d.tiles != nullptr)
   {
      map::load_tiles(d.load_at, d.tiles + static_cast<uint32_t>(frame) * d.frame_tiles * kTileWords, d.frame_tiles);
   }
   const uint32_t cell_entries = static_cast<uint32_t>(d.layers) * map::kMetatileEntries;
   const uint32_t set_entries = cell_entries * d.width * d.height;
   const uint16_t* set = d.entries + (d.entry_frames > 1 ? frame * set_entries : 0);
   for (int cy = 0; cy < d.height; ++cy)
   {
      for (int cx = 0; cx < d.width; ++cx)
      {
         const uint16_t* cell = set + static_cast<uint32_t>(cy * d.width + cx) * cell_entries;
         uint16_t entries[map::kMaxLayers * map::kMetatileEntries] = {};
         for (int l = 0; l < m->layer_count; ++l)
         {
            // the door's layers, then the cell's own metatiles on the map's other layers
            const map::MetatileId own = map::metatile_at(*m, l, x + cx, top + cy);
            const uint16_t* from = l < d.layers ? cell + l * map::kMetatileEntries
                                   : (own < m->metatile_count ? m->metatiles[own].tiles : nullptr);
            for (int k = 0; k < map::kMetatileEntries; ++k)
            {
               entries[l * map::kMetatileEntries + k] = from != nullptr ? from[k] : uint16_t{0};
            }
         }
         map::set_cell_override(x + cx, top + cy, entries);
      }
   }
}

/// The entry for a door on a cell: the one running there, else a free one.
/// @param x  anchor column
/// @param y  anchor row
/// @return the entry, or nullptr (every entry runs another door)
Run* entry_for(int x, int y)
{
   Run* free = nullptr;
   for (Run& r : g_runs)
   {
      if (r.door != nullptr && r.x == x && r.y == y)
      {
         return &r;
      }
      if (r.door == nullptr && free == nullptr)
      {
         free = &r;
      }
   }
   return free;
}

/// Starts a door's opening or closing.
/// @param x        anchor column
/// @param y        anchor row
/// @param opening  open (else close)
/// @return false when nothing started
bool start(int x, int y, bool opening)
{
   const asset::Door* d = at(x, y);
   Run* r = d != nullptr ? entry_for(x, y) : nullptr;
   if (r == nullptr)
   {
      return false;
   }
   const asset::DoorStep* steps = opening ? d->open : d->close;
   const uint8_t count = opening ? d->open_count : d->close_count;
   *r = Run{};
   if (count == 0)
   {
      return true;   // nothing to show
   }
   r->door = d;
   r->steps = steps;
   r->count = count;
   r->x = static_cast<int16_t>(x);
   r->y = static_cast<int16_t>(y);
   const AssetKey sound = opening ? d->open_sound : d->close_sound;
   if (raw(sound) != 0)
   {
      game::door_sound(sound);
   }
   return true;
}

/// Stops the door running on a cell, if one does.
/// @param x  anchor column
/// @param y  anchor row
void stop(int x, int y)
{
   for (Run& r : g_runs)
   {
      if (r.door != nullptr && r.x == x && r.y == y)
      {
         r = Run{};
      }
   }
}

}   // namespace

const asset::Door* at(int x, int y)
{
   const map::DoorBinding* b = map::door_at(x, y);
   return b != nullptr ? asset::door(b->door) : nullptr;
}

bool open(int x, int y)
{
   return start(x, y, true);
}

bool close(int x, int y)
{
   return start(x, y, false);
}

bool set_open(int x, int y)
{
   const asset::Door* d = at(x, y);
   if (d == nullptr)
   {
      return false;
   }
   stop(x, y);
   draw(*d, d->open_count > 0 ? d->open[d->open_count - 1].frame : asset::kDoorClosed, x, y);
   return true;
}

bool set_closed(int x, int y)
{
   const asset::Door* d = at(x, y);
   if (d == nullptr)
   {
      return false;
   }
   stop(x, y);
   draw(*d, asset::kDoorClosed, x, y);
   return true;
}

void update()
{
   for (Run& r : g_runs)
   {
      if (r.door == nullptr)
      {
         continue;
      }
      const asset::DoorStep& s = r.steps[r.step];
      if (r.counter == 0)
      {
         draw(*r.door, s.frame, r.x, r.y);   // a step's frame shows from its first display frame
      }
      ++r.counter;
      if (r.counter < s.ticks)
      {
         continue;
      }
      r.counter = 0;
      ++r.step;
      if (r.step >= r.count)
      {
         r = Run{};   // its last frame stays drawn
      }
   }
}

bool busy()
{
   for (const Run& r : g_runs)
   {
      if (r.door != nullptr)
      {
         return true;
      }
   }
   return false;
}

void reset()
{
   for (Run& r : g_runs)
   {
      r = Run{};
   }
}

}   // namespace capsule::field::doors

namespace capsule::field::game
{

/// The default: no sound (CAPSULE_WEAK: a game with audio defines its own; see doors.h).
CAPSULE_WEAK void door_sound(AssetKey sound)
{
   static_cast<void>(sound);
}

}   // namespace capsule::field::game
