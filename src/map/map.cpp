/// @file
/// Overworld maps (see capsule/map/map.h).
#include "capsule/map/map.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::map
{
namespace
{

namespace video = hal::video;

/// @name Map geometry
///@{
constexpr int kMetatileShift = 4;                       ///< Pixel coordinate to metatile (/ kMetatilePixels).
constexpr int kChunkShift = 3;                          ///< Metatile coordinate to chunk (/ kChunkMetatiles).
constexpr int kChunkMask = kChunkMetatiles - 1;         ///< Metatile coordinate within its chunk.
constexpr int kMetatileTiles = 2;                       ///< Tiles per metatile side.
constexpr uint16_t kNoChunk = 0xFFFF;                   ///< Map::grid: no chunk here.
constexpr int kEventLocalYShift = 4;                    ///< Event::local packs x | y << 4.
static_assert((1 << kMetatileShift) == kMetatilePixels && (1 << kChunkShift) == kChunkMetatiles);
///@}

/// @name Metatile::tiles order
///@{
constexpr int kEntryTopLeft = 0;       ///< Top-left map entry.
constexpr int kEntryTopRight = 1;      ///< Top-right map entry.
constexpr int kEntryBottomLeft = 2;    ///< Bottom-left map entry.
constexpr int kEntryBottomRight = 3;   ///< Bottom-right map entry.
///@}

/// @name VRAM layout
///@{
constexpr unsigned kScreenBlockBytes = 2048u;   ///< Bytes per screen block.
constexpr unsigned kCharBlockBytes = 0x4000u;   ///< Bytes per character block.
constexpr unsigned kMapEntryBytes = 2u;         ///< Bytes per regular map entry.
constexpr unsigned kTileWords4bpp = 8u;         ///< 32-bit words in one 4bpp tile.
///@}

/// Colors stored per map palette bank: color 0 is the backdrop, loaded once.
constexpr int kStoredBankColors = gfx::palette::kBankColors - 1;

/// Floor division by 16 (metatile of a pixel), also for negative pixels.
/// @param px  pixel coordinate
/// @return metatile coordinate
constexpr int32_t metatile_of(int32_t px)
{
   return px >> kMetatileShift;   // an arithmetic shift rounds negative pixels down
}

static_assert(metatile_of(-1) == -1 && metatile_of(15) == 0 && metatile_of(16) == 1);

/// I3: one cell override (a door's frame shown on a cell).
struct CellOverride
{
   int16_t x;                                         ///< Metatile column.
   int16_t y;                                         ///< Metatile row.
   bool used;                                         ///< The entry holds an override.
   uint16_t entries[kMaxLayers][kMetatileEntries];    ///< Per tile layer, the 2x2 map entries shown.
};

/// N2: one edited cell of the shown map (set_cell()).
struct CellEdit
{
   int16_t x;                             ///< Metatile column (kNoEdit: the entry is free).
   int16_t y;                             ///< Metatile row.
   MapCell cell;                          ///< Its attributes.
   MetatileId layers[kMaxLayers];         ///< Its metatile per tile layer.
};

constexpr int16_t kNoEdit = INT16_MIN;   ///< CellEdit::x of a free entry.
/// The edit table is a power of two (its slots found by a hash of the cell and linear probing).
static_assert((kMaxCellEdits & (kMaxCellEdits - 1)) == 0, "kMaxCellEdits must be a power of two");
constexpr uint32_t kEditMask = kMaxCellEdits - 1;   ///< Slot bits of a hash.
constexpr uint32_t kEditHashY = 31u;                ///< Multiplier of the row in the hash (spreads rows apart).

/// Shown-map state.
struct State
{
   const Map* map;                        ///< Shown map, or nullptr.
   const Map* terrain;                    ///< N2: the map whose terrain shows (map, or set_layout()'s).
   uint16_t edit_count;                   ///< N2: edited cells (g_edits).
   int32_t cam_x;                         ///< Camera left edge, pixels.
   int32_t cam_y;                         ///< Camera top edge, pixels.
   int32_t drawn_x;                       ///< Metatile column of the drawn window's left edge.
   int32_t drawn_y;                       ///< Metatile row of the drawn window's top edge.
   uint32_t clock;                        ///< I3: frames since the map was entered (a seamless shift keeps it).
   uint8_t anim_timer[kMaxAnimations];    ///< Frames into each animation's period.
   uint8_t anim_frame[kMaxAnimations];    ///< Each animation's current frame.
   uint8_t target_timer[kMaxAnimTargets];   ///< DS1: frames into the period of each more target (Map::anim_targets).
   uint8_t target_frame[kMaxAnimTargets];   ///< DS1: each more target's frame.
   CellOverride overrides[kMaxCellOverrides];   ///< I3: the overridden cells.
   uint16_t sets_first;                   ///< DS1: the shown map's first override set (World::override_sets).
   uint8_t sets_count;                    ///< DS1: its override sets.
   uint32_t sets_on;                      ///< DS1: the applied ones (bit k = set sets_first + k).
   bool suspended;                        ///< UC: suspend() took the map's video memory (resume() gives it back).
};
State g{};   ///< The shown map.

/// Tiles per ring side (32: one screen block).
constexpr int kRingTiles = kRingMetatiles * kMetatileTiles;
/// Camera bits the scroll registers take: the ring repeats every 256 pixels.
constexpr int32_t kRingPixelMask = kRingTiles * kTilePixels - 1;
/// Ring row bits of one metatile (its two tile rows).
constexpr uint32_t kMetatileRowBits = 3;
static_assert(kRingTiles == 32, "one 32x32 screen block per layer");

/// The layers' screen rings as they should look; rows land in VRAM through
/// the upload queue (A-5), so the screen never shows a half-drawn edge.
CAPSULE_EWRAM_BSS uint16_t g_ring[kMaxLayers][kRingTiles * kRingTiles];
/// Rows of each layer's ring changed since they were last queued (bit = tile row).
uint32_t g_dirty[kMaxLayers];

/// N2: the shown map's edited cells (set_cell()), slots by a hash of the cell.
CAPSULE_EWRAM_BSS CellEdit g_edits[kMaxCellEdits];

/// N2: the first slot to look at for a cell's edit.
/// @param x  metatile column
/// @param y  metatile row
/// @return 0 to kMaxCellEdits - 1
constexpr uint32_t edit_hash(int x, int y)
{
   return (static_cast<uint32_t>(x) + static_cast<uint32_t>(y) * kEditHashY) & kEditMask;
}

/// N2: a cell's edit on the shown map.
/// @param m  the map asked about (only the shown map has edits)
/// @param x  metatile column
/// @param y  metatile row
/// @return the edit, or nullptr
const CellEdit* find_edit(const Map& m, int x, int y)
{
   if (g.edit_count == 0 || &m != g.map)
   {
      return nullptr;
   }
   uint32_t i = edit_hash(x, y);
   for (int n = 0; n < kMaxCellEdits; ++n, i = (i + 1) & kEditMask)
   {
      const CellEdit& e = g_edits[i];
      if (e.x == kNoEdit)
      {
         return nullptr;   // linear probing: the cell's run of slots ends at a free one
      }
      if (e.x == x && e.y == y)
      {
         return &e;
      }
   }
   return nullptr;
}

/// N2: frees every slot of the edit table (at a map's first edit: the table may hold an earlier map's).
void clear_edits()
{
   for (CellEdit& e : g_edits)
   {
      e.x = kNoEdit;
   }
   g.edit_count = 0;
}

/// N2: the terrain a query of a map reads: the shown terrain for the shown map, else the map itself.
/// @param m  the map
/// @return its terrain
const Map& terrain_of(const Map& m)
{
   return &m == g.map && g.terrain != nullptr ? *g.terrain : m;
}

/// Metatile columns drawn around the camera (15 visible + 1 partial).
constexpr int kViewCols = 16;
/// Metatile rows drawn (10 visible + 1 partial).
constexpr int kViewRows = 11;
static_assert(kViewCols <= kRingMetatiles && kViewRows <= kRingMetatiles);

/// The chunk holding a position.
/// @param m  the map
/// @param x  metatile column
/// @param y  metatile row
/// @return the chunk, or nullptr outside every chunk
const ChunkDirectoryEntry* chunk_of(const Map& m, int x, int y)
{
   const int cx = (x >> kChunkShift) - m.grid_x0;
   const int cy = (y >> kChunkShift) - m.grid_y0;
   if (cx < 0 || cy < 0 || cx >= m.grid_w || cy >= m.grid_h)
   {
      return nullptr;
   }
   const uint16_t i = m.grid[cy * m.grid_w + cx];
   return i == kNoChunk ? nullptr : &m.chunks[i];
}

/// Index of a position within its chunk.
/// @param x  metatile column
/// @param y  metatile row
/// @return 0-63
constexpr uint32_t cell_index(int x, int y)
{
   return static_cast<uint32_t>((y & kChunkMask) * kChunkMetatiles + (x & kChunkMask));
}

/// Index of a position in the border pattern (world-anchored, C-03b).
/// @param m  the map
/// @param x  metatile column
/// @param y  metatile row
/// @return 0 to border_w * border_h - 1
int border_index(const Map& m, int x, int y)
{
   int bx = (x + m.border_phase_x) % m.border_w;
   int by = (y + m.border_phase_y) % m.border_h;
   if (bx < 0)
   {
      bx += m.border_w;
   }
   if (by < 0)
   {
      by += m.border_h;
   }
   return by * m.border_w + bx;
}

/// DS1: the entries an applied override set shows on one layer of a cell (the last applied set listing the cell
/// wins), or the cell's own.
/// @param x     metatile column
/// @param y     metatile row
/// @param l     tile layer
/// @param own   the cell's own entries on that layer
/// @return the entries to draw
const uint16_t* set_entries(int x, int y, int l, const uint16_t* own)
{
   static constexpr uint16_t kNothing[kMetatileEntries] = {0, 0, 0, 0};   // tile 0: transparent
   const Map& m = *g.terrain;   // N2: the shown terrain's dictionary
   const uint16_t* e = own;
   for (uint8_t k = 0; k < g.sets_count; ++k)
   {
      if ((g.sets_on & (1u << k)) == 0)
      {
         continue;
      }
      const OverrideSet& s = kWorld.override_sets[g.sets_first + k];
      for (uint16_t n = 0; n < s.cell_count; ++n)
      {
         const OverrideCell& c = kWorld.override_cells[s.first_cell + n];
         if (c.x != x || c.y != y || c.layer != l)
         {
            continue;
         }
         if ((c.flags & kOverrideEntries) != 0)
         {
            e = kWorld.override_entries + static_cast<uint32_t>(c.value) * kMetatileEntries;
         }
         else
         {
            e = c.value < m.metatile_count ? m.metatiles[c.value].tiles : kNothing;
         }
      }
   }
   return e;
}

/// Draws one metatile position of every tile layer into the rings.
/// @param mx  metatile column
/// @param my  metatile row
void draw_metatile(int32_t mx, int32_t my)
{
   const Map& m = *g.terrain;   // N2: the shown terrain (the shown map's own, or set_layout()'s)
   const int x = static_cast<int>(mx);
   const int y = static_cast<int>(my);
   // The ring wraps: metatile (mx, my) lands at its position modulo the ring.
   const int tx = static_cast<int>(mx & (kRingMetatiles - 1)) * kMetatileTiles;
   const int ty = static_cast<int>(my & (kRingMetatiles - 1)) * kMetatileTiles;
   static constexpr uint16_t kBlank[4] = {0, 0, 0, 0};
   // One lookup for every layer: the chunk's cells, or the border's.
   const ChunkDirectoryEntry* c = chunk_of(m, x, y);
   const uint32_t i = cell_index(x, y);
   const bool painted = c != nullptr && m.cells[c->cells_offset + i] != kCellUndefined;
   const int b = painted ? 0 : border_index(m, x, y);
   const int border_cells = m.border_w * m.border_h;
   const CellEdit* edit = find_edit(*g.map, x, y);   // N2: an edited cell shows its edit's metatiles
   // I3: an overridden cell shows its override's entries instead
   const CellOverride* over = nullptr;
   for (const CellOverride& o : g.overrides)
   {
      if (o.used && o.x == x && o.y == y)
      {
         over = &o;
      }
   }
   for (int l = 0; l < m.layer_count; ++l)
   {
      MetatileId id = painted ? m.layer_cells[c->layers_offset + static_cast<uint32_t>(l) * kChunkCells + i]
                              : m.border[l * border_cells + b];
      if (edit != nullptr)
      {
         id = edit->layers[l];
      }
      const uint16_t* e = id < m.metatile_count ? m.metatiles[id].tiles : kBlank;
      if (g.sets_on != 0)
      {
         e = set_entries(x, y, l, e);   // DS1: an applied override set's cell
      }
      if (over != nullptr)
      {
         e = over->entries[l];
      }
      uint16_t* ring = g_ring[l];
      ring[ty * kRingTiles + tx] = e[kEntryTopLeft];
      ring[ty * kRingTiles + tx + 1] = e[kEntryTopRight];
      ring[(ty + 1) * kRingTiles + tx] = e[kEntryBottomLeft];
      ring[(ty + 1) * kRingTiles + tx + 1] = e[kEntryBottomRight];
      g_dirty[l] |= kMetatileRowBits << ty;
   }
}

/// Draws a column of the view window.
/// @param mx  metatile column
void draw_column(int32_t mx)
{
   for (int r = 0; r < kViewRows; ++r)
   {
      draw_metatile(mx, g.drawn_y + r);
   }
}

/// Draws a row of the view window.
/// @param my  metatile row
void draw_row(int32_t my)
{
   for (int c = 0; c < kViewCols; ++c)
   {
      draw_metatile(g.drawn_x + c, my);
   }
}

/// Draws the whole view window at the current camera.
void draw_all()
{
   g.drawn_x = metatile_of(g.cam_x);
   g.drawn_y = metatile_of(g.cam_y);
   for (int r = 0; r < kViewRows; ++r)
   {
      for (int c = 0; c < kViewCols; ++c)
      {
         draw_metatile(g.drawn_x + c, g.drawn_y + r);
      }
   }
}

/// Queues every changed ring row (must-land), runs of rows as one request.
void queue_rings()
{
   const Map& m = *g.terrain;
   for (int l = 0; l < m.layer_count; ++l)
   {
      uint32_t dirty = g_dirty[l];
      g_dirty[l] = 0;
      const uint32_t base = uint32_t{screen_block_of(m.layers[l].bg)} * kScreenBlockBytes;
      int row = 0;
      while (dirty != 0)
      {
         while ((dirty & 1u) == 0)
         {
            dirty >>= 1;
            ++row;
         }
         int run = 0;
         while ((dirty & 1u) != 0)
         {
            dirty >>= 1;
            ++run;
         }
         hal::vram::copy16(video::Region::BgVram, base + static_cast<uint32_t>(row) * kRingTiles * kMapEntryBytes,
                           g_ring[l] + row * kRingTiles, static_cast<uint32_t>(run) * kRingTiles,
                           hal::vram::Priority::MustLand, video::Source::Ewram);
         row += run;
      }
   }
}

/// Points the map layers' scroll registers at the camera.
void apply_scroll()
{
   const auto sx = static_cast<int16_t>(g.cam_x & kRingPixelMask);
   const auto sy = static_cast<int16_t>(g.cam_y & kRingPixelMask);
   for (int l = 0; l < g.terrain->layer_count; ++l)
   {
      video::set_bg_scroll(g.terrain->layers[l].bg, sx, sy);
   }
}

/// Queues one frame of an animation into one of its targets (streaming, from ROM).
/// @param a      the animation
/// @param first  the target's first map tile (DS1: the animation's own, or a more target's)
/// @param frame  frame index
void upload_frame(const TileAnimation& a, uint16_t first, uint8_t frame)
{
   hal::vram::copy32(video::Region::BgVram,
                     uint32_t{kTileCharBlock} * kCharBlockBytes + uint32_t{first} * video::kTileBytes4bpp,
                     a.frames + static_cast<uint32_t>(frame) * a.tile_count * kTileWords4bpp,
                     uint32_t{a.tile_count} * kTileWords4bpp, hal::vram::Priority::Streaming, video::Source::Rom);
}

/// DS1: the index of an animation's first more target in Map::anim_targets (those of the animations before it).
/// @param m  the map
/// @param i  the animation
/// @return the index
uint16_t first_target(const Map& m, uint8_t i)
{
   uint16_t n = 0;
   for (uint8_t j = 0; j < i; ++j)
   {
      n = static_cast<uint16_t>(n + m.animations[j].targets);
   }
   return n;
}

/// DS1: one frame's worth of one target's timer: on its period's end the next frame shows.
/// @param a      the animation
/// @param first  the target's first map tile
/// @param timer  [in, out] its timer
/// @param frame  [in, out] its frame
void tick_target(const TileAnimation& a, uint16_t first, uint8_t& timer, uint8_t& frame)
{
   if (++timer < a.period)
   {
      return;
   }
   timer = 0;
   frame = static_cast<uint8_t>(frame + 1 < a.frame_count ? frame + 1 : 0);
   upload_frame(a, first, frame);
}

} // namespace

const Map* find(AssetKey key)
{
   int lo = 0;
   int hi = static_cast<int>(kWorld.map_count) - 1;
   while (lo <= hi)
   {
      const int mid = (lo + hi) / kHalf;
      const auto k = static_cast<uint32_t>(kWorld.keys[mid].key);
      if (k == static_cast<uint32_t>(key))
      {
         return &kWorld.maps[kWorld.keys[mid].index];
      }
      if (k < static_cast<uint32_t>(key))
      {
         lo = mid + 1;
      }
      else
      {
         hi = mid - 1;
      }
   }
   return nullptr;
}

MapCell cell_at(const Map& map, int x, int y)
{
   const CellEdit* edit = find_edit(map, x, y);
   if (edit != nullptr)
   {
      return edit->cell;   // N2: the shown map's edited cell
   }
   const Map& m = terrain_of(map);
   const ChunkDirectoryEntry* c = chunk_of(m, x, y);
   if (c != nullptr)
   {
      const MapCell v = m.cells[c->cells_offset + cell_index(x, y)];
      if (v != kCellUndefined)
      {
         return v;
      }
   }
   return kCellBorder;
}

MetatileId metatile_at(const Map& map, int layer, int x, int y)
{
   const Map& m = terrain_of(map);
   if (layer < 0 || layer >= m.layer_count)
   {
      return kNoMetatile;
   }
   const CellEdit* edit = find_edit(map, x, y);
   if (edit != nullptr)
   {
      return edit->layers[layer];   // N2: the shown map's edited cell
   }
   const ChunkDirectoryEntry* c = chunk_of(m, x, y);
   const uint32_t i = cell_index(x, y);
   if (c != nullptr && m.cells[c->cells_offset + i] != kCellUndefined)
   {
      return m.layer_cells[c->layers_offset + static_cast<uint32_t>(layer) * kChunkCells + i];
   }
   return m.border[layer * m.border_w * m.border_h + border_index(m, x, y)];
}

bool is_painted(const Map& map, int x, int y)
{
   if (find_edit(map, x, y) != nullptr)
   {
      return true;   // N2: an edited cell is drawn as painted
   }
   const Map& m = terrain_of(map);
   const ChunkDirectoryEntry* c = chunk_of(m, x, y);
   return c != nullptr && m.cells[c->cells_offset + cell_index(x, y)] != kCellUndefined;
}

const Event* find_event(const Map& m, int x, int y, EventKind kind, uint8_t elevation, const Event* after)
{
   const ChunkDirectoryEntry* c = chunk_of(m, x, y);
   if (c == nullptr || c->event_count == 0 || m.events == nullptr)
   {
      return nullptr;
   }
   const auto local = static_cast<uint8_t>((x & kChunkMask) | ((y & kChunkMask) << kEventLocalYShift));
   const Event* e = m.events + c->event_first;
   const Event* end = e + c->event_count;
   if (after != nullptr)
   {
      ENGINE_ASSERT(after >= e && after < end);
      e = after + 1;
   }
   for (; e < end; ++e)
   {
      if (e->local == local && e->kind == kind && (e->elevation == 0 || e->elevation == elevation))
      {
         return e;
      }
   }
   return nullptr;
}

uint16_t index_of(const Map& m)
{
   ENGINE_ASSERT(&m >= kWorld.maps && &m < kWorld.maps + kWorld.map_count);
   return static_cast<uint16_t>(&m - kWorld.maps);
}

AssetKey key_of(uint16_t index)
{
   // the keys are sorted by key, not by index: a scan (rare callers, e.g. the escape point on a warp)
   for (uint16_t k = 0; k < kWorld.map_count; ++k)
   {
      if (kWorld.keys[k].index == index)
      {
         return kWorld.keys[k].key;
      }
   }
   return AssetKey{};
}

namespace
{

/// The default rule's 16 levels (World::elevations is nullptr).
struct DefaultElevations
{
   Elevation levels[kElevationLevels];   ///< By level.
};

/// Builds the default rule's table.
/// @return every level's default_elevation()
constexpr DefaultElevations make_default_elevations()
{
   DefaultElevations t{};
   for (int i = 0; i < kElevationLevels; ++i)
   {
      t.levels[i] = default_elevation(static_cast<uint8_t>(i));
   }
   return t;
}

constexpr DefaultElevations kDefaultElevations = make_default_elevations();   ///< The default rule.

/// Whether a level's meet mask holds another level.
/// @param e      the level's entry
/// @param level  the other level
/// @return true when it does
constexpr bool mask_has(const Elevation& e, uint8_t level)
{
   return ((e.meets >> (level & kElevationMask)) & 1u) != 0;
}

}   // namespace

const Elevation& elevation(uint8_t level)
{
   const uint8_t i = level & kElevationMask;
   return kWorld.elevations != nullptr ? kWorld.elevations[i] : kDefaultElevations.levels[i];
}

bool levels_meet(uint8_t a, uint8_t b)
{
   return mask_has(elevation(a), b) || mask_has(elevation(b), a);
}

bool may_enter(uint8_t mover, uint8_t cell)
{
   const Elevation& from = elevation(mover);
   // A mover on a keep level (a bridge level it was placed on) has no level of its own to offer.
   return mask_has(elevation(cell), mover) || ((from.flags & kElevationKeep) == 0 && mask_has(from, cell));
}

uint8_t arrival_level(uint8_t mover, uint8_t cell)
{
   return (elevation(cell).flags & kElevationKeep) != 0 ? mover : static_cast<uint8_t>(cell & kElevationMask);
}

uint8_t placed_level(uint8_t cell)
{
   return (elevation(cell).flags & kElevationKeep) != 0 ? kElevationAny : static_cast<uint8_t>(cell & kElevationMask);
}

const VehicleClass& vehicle_class(uint8_t index)
{
   if (kWorld.vehicle_classes == nullptr || index >= kWorld.vehicle_class_count)
   {
      return kDefaultVehicle;
   }
   return kWorld.vehicle_classes[index];
}

const Behavior& behavior(MapCell c)
{
   static constexpr Behavior kNormal{0xFF, BehaviorKind::None, 0, 0};
   if (kWorld.behaviors == nullptr || kWorld.behavior_count == 0)
   {
      return kNormal;
   }
   const uint8_t b = cell_behavior(c);
   return kWorld.behaviors[b < kWorld.behavior_count ? b : 0];
}

namespace
{

/// The timer and frame an animation has after running for some frames from its phase and frame 0 (I3: the map's
/// animation clock).
/// @param a      the animation
/// @param clock  frames it has run
/// @param timer  [out] its timer
/// @param frame  [out] its frame
/// @param phase   the target's phase (DS1: the animation's own, or a more target's)
/// @param offset  the target's frame offset (DS1: 0 for the animation's own target)
void clocked_frame(const TileAnimation& a, uint32_t clock, uint8_t phase, uint8_t offset, uint8_t& timer,
                   uint8_t& frame)
{
   const uint32_t t = clock + phase;
   timer = static_cast<uint8_t>(t % a.period);
   frame = static_cast<uint8_t>((t / a.period + offset) % a.frame_count);
}

/// I2: the timer and frame an animation takes at enter(): those of the same animation on the map shown before when
/// it is kept, else (I3) those the map's animation clock gives it (its phase and frame 0 at a plain entry) or, for a
/// kAnimRestartOnShift animation on a shift, its phase and frame 0. DS1: its more targets too (theirs: their phases
/// and offsets, or the same animation's targets before when it has as many).
/// @param a        the new map's animation
/// @param targets  the new map's Map::anim_targets (nullptr without targets)
/// @param target0  the index of its first more target in them (first_target())
/// @param before   the state before enter() (its map may be nullptr)
/// @param keep     whether running animations are kept
/// @param timer    [out] its timer
/// @param frame    [out] its frame
/// @return true when it was kept running (its current frame belongs on screen)
bool starting_frame(const TileAnimation& a, const AnimTarget* targets, uint16_t target0, const State& before,
                    bool keep, uint8_t& timer, uint8_t& frame)
{
   timer = a.phase;
   frame = 0;
   for (uint8_t k = 0; k < a.targets; ++k)
   {
      g.target_timer[target0 + k] = targets[target0 + k].phase;
      g.target_frame[target0 + k] = targets[target0 + k].offset;
   }
   if (!keep || before.map == nullptr || (a.flags & kAnimRestartOnShift) != 0)
   {
      return false;
   }
   for (uint8_t j = 0; j < before.map->animation_count; ++j)
   {
      const TileAnimation& b = before.map->animations[j];
      if (b.frames == a.frames && b.first_tile == a.first_tile && b.period == a.period &&
          b.frame_count == a.frame_count && b.tile_count == a.tile_count)
      {
         timer = before.anim_timer[j];
         frame = before.anim_frame[j];
         const uint16_t from = first_target(*before.map, j);
         for (uint8_t k = 0; k < a.targets; ++k)
         {
            if (b.targets == a.targets)
            {
               g.target_timer[target0 + k] = before.target_timer[from + k];
               g.target_frame[target0 + k] = before.target_frame[from + k];
            }
            else
            {
               clocked_frame(a, before.clock, targets[target0 + k].phase, targets[target0 + k].offset,
                             g.target_timer[target0 + k], g.target_frame[target0 + k]);
            }
         }
         return true;
      }
   }
   // as if it had run since the map's entry
   clocked_frame(a, before.clock, a.phase, 0, timer, frame);
   for (uint8_t k = 0; k < a.targets; ++k)
   {
      clocked_frame(a, before.clock, targets[target0 + k].phase, targets[target0 + k].offset,
                    g.target_timer[target0 + k], g.target_frame[target0 + k]);
   }
   return true;
}

/// DS1: finds the shown map's override sets in the world's list (sorted by map); none is applied yet.
/// @param m  the map entered
void find_override_sets(const Map& m)
{
   g.sets_first = 0;
   g.sets_count = 0;
   g.sets_on = 0;
   const uint16_t me = index_of(m);
   uint16_t i = 0;
   while (i < kWorld.override_set_count && kWorld.override_sets[i].map < me)
   {
      ++i;
   }
   g.sets_first = i;
   while (i < kWorld.override_set_count && kWorld.override_sets[i].map == me && g.sets_count < kMaxMapOverrideSets)
   {
      ++i;
      ++g.sets_count;
   }
}

/// UC: loads a terrain's tiles and palettes (map::enter(), resume()).
/// @param m  the terrain shown
void load_graphics(const Map& m)
{
   video::set_mode(0);
   video::load_bg_tiles(kTileCharBlock, 0, m.tiles, m.tile_count);
   gfx::palette::load_bg(0, &m.backdrop, 1);
   for (uint8_t p = 0; p < m.palette_count; ++p)
   {
      gfx::palette::load_bg(static_cast<uint8_t>(p * gfx::palette::kBankColors + 1), m.palettes + p * kStoredBankColors,
                            kStoredBankColors);
   }
}

/// UC: starts (or keeps) a terrain's tile animations and uploads the frames they show (map::enter(), resume()).
/// @param m                the terrain shown
/// @param before           the state before (the animations it ran)
/// @param keep_animations  see enter()
void start_animations(const Map& m, const State& before, bool keep_animations)
{
   g.clock = keep_animations ? before.clock : 0;
   uint16_t target0 = 0;
   for (uint8_t i = 0; i < m.animation_count; ++i)
   {
      const TileAnimation& a = m.animations[i];
      ENGINE_ASSERT(target0 + a.targets <= kMaxAnimTargets && (a.targets == 0 || m.anim_targets != nullptr));
      const bool kept = starting_frame(a, m.anim_targets, target0, before, keep_animations, g.anim_timer[i],
                                       g.anim_frame[i]);
      // I3: the map's own tiles (just loaded) show until the first frame change: at a plain entry for kAnimFromSheet,
      // on a shift for kAnimReloadOnShift (and a restarted animation is entered afresh)
      const bool sheet = kept ? (a.flags & kAnimReloadOnShift) != 0 : (a.flags & kAnimFromSheet) != 0;
      if (!sheet)
      {
         upload_frame(a, a.first_tile, g.anim_frame[i]);
         for (uint8_t k = 0; k < a.targets; ++k)
         {
            upload_frame(a, m.anim_targets[target0 + k].first_tile, g.target_frame[target0 + k]);
         }
      }
      target0 = static_cast<uint16_t>(target0 + a.targets);
   }
}

/// UC: configures a terrain's layers under the map plan and draws the view (map::enter(), resume()).
/// @param m  the terrain shown
void show_layers(const Map& m)
{
   uint8_t map_bgs = 0;
   for (int l = 0; l < m.layer_count; ++l)
   {
      map_bgs = static_cast<uint8_t>(map_bgs | (1 << m.layers[l].bg));
   }
   gfx::background::begin_map_plan(map_bgs, m.palette_count);   // BG0 (if free) moves out of the way, for text
   for (int l = 0; l < m.layer_count; ++l)
   {
      const TileLayer& t = m.layers[l];
      video::configure_bg(t.bg, true, t.priority, kTileCharBlock, screen_block_of(t.bg), false, 0);
   }
   draw_all();
   queue_rings();
   apply_scroll();
}

/// Shows a map with a terrain (enter(): its own; set_layout(): another map's).
/// @param map              the map
/// @param m                the terrain shown
/// @param cx               camera left edge, pixels
/// @param cy               camera top edge, pixels
/// @param keep_animations  see enter()
void show(const Map& map, const Map& m, int32_t cx, int32_t cy, bool keep_animations)
{
   ENGINE_ASSERT(m.tile_count <= kMaxTiles && m.palette_count <= kMaxPalettes && m.animation_count <= kMaxAnimations &&
                 m.border_w > 0 && m.border_h > 0 && m.layer_count <= kMaxLayers);
   const State before = g;   // the running animations, for keep_animations
   leave();   // the previous map's layers off (this map may use other BGs)
   g.map = &map;
   g.terrain = &m;   // N2: the terrain shown (leave() dropped the cell edits)
   g.cam_x = cx;
   g.cam_y = cy;
   load_graphics(m);
   start_animations(m, before, keep_animations);
   find_override_sets(map);
   show_layers(m);
}

} // namespace

void enter(const Map& m, int32_t cx, int32_t cy, bool keep_animations)
{
   show(m, m, cx, cy, keep_animations);
}

void suspend()
{
   if (g.map == nullptr || g.suspended)
   {
      return;
   }
   for (int l = 0; l < g.terrain->layer_count; ++l)
   {
      const TileLayer& t = g.terrain->layers[l];
      video::configure_bg(t.bg, false, t.priority, kTileCharBlock, screen_block_of(t.bg), false, 0);
   }
   for (uint32_t& d : g_dirty)
   {
      d = 0;   // the rings are drawn again at resume()
   }
   gfx::background::end_map_plan();
   g.suspended = true;
}

void resume(bool restart_animations)
{
   if (g.map == nullptr || !g.suspended)
   {
      return;
   }
   g.suspended = false;
   const Map& m = *g.terrain;
   const State before = g;   // the same map: every animation is "kept" unless restarted
   load_graphics(m);
   start_animations(m, before, !restart_animations);
   for (uint32_t& d : g_dirty)
   {
      d = ~uint32_t{0};   // every ring row goes up again (the scene wrote over the rows outside the view too)
   }
   show_layers(m);
}

bool suspended()
{
   return g.suspended;
}

void leave()
{
   if (g.map != nullptr && !g.suspended)
   {
      for (int l = 0; l < g.terrain->layer_count; ++l)
      {
         const TileLayer& t = g.terrain->layers[l];
         video::configure_bg(t.bg, false, t.priority, kTileCharBlock, screen_block_of(t.bg), false, 0);
      }
   }
   g = State{};   // N2: no cell edits (the table is cleared at the next map's first edit)
   for (uint32_t& d : g_dirty)
   {
      d = 0;
   }
   gfx::background::end_map_plan();
}

const Map* current()
{
   return g.map;
}

void set_camera(int32_t x, int32_t y)
{
   if (g.map == nullptr)
   {
      return;
   }
   g.cam_x = x;
   g.cam_y = y;
   const int32_t want_x = metatile_of(x);
   const int32_t want_y = metatile_of(y);
   // Big jumps (teleports): redraw everything.
   if (want_x - g.drawn_x >= kViewCols || g.drawn_x - want_x >= kViewCols || want_y - g.drawn_y >= kViewRows ||
       g.drawn_y - want_y >= kViewRows)
   {
      draw_all();
      queue_rings();
      apply_scroll();
      return;
   }
   while (g.drawn_x < want_x)
   {   // scrolled right: the new right column
      ++g.drawn_x;
      draw_column(g.drawn_x + kViewCols - 1);
   }
   while (g.drawn_x > want_x)
   {
      --g.drawn_x;
      draw_column(g.drawn_x);
   }
   while (g.drawn_y < want_y)
   {
      ++g.drawn_y;
      draw_row(g.drawn_y + kViewRows - 1);
   }
   while (g.drawn_y > want_y)
   {
      --g.drawn_y;
      draw_row(g.drawn_y);
   }
   queue_rings();
   apply_scroll();
}

int32_t camera_x()
{
   return g.cam_x;
}

int32_t camera_y()
{
   return g.cam_y;
}

void update()
{
   if (g.map == nullptr || g.suspended)
   {
      return;   // UC: a suspended map's animations wait for resume()
   }
   const Map& m = *g.terrain;   // N2: the shown terrain's animations
   ++g.clock;
   uint16_t target = 0;
   for (uint8_t i = 0; i < m.animation_count; ++i)
   {
      const TileAnimation& a = m.animations[i];
      tick_target(a, a.first_tile, g.anim_timer[i], g.anim_frame[i]);
      for (uint8_t k = 0; k < a.targets; ++k, ++target)
      {   // DS1: its more targets, each on its own timer
         tick_target(a, m.anim_targets[target].first_tile, g.target_timer[target], g.target_frame[target]);
      }
   }
}

void redraw()
{
   if (g.map == nullptr || g.suspended)
   {
      return;
   }
   draw_all();
   queue_rings();
   apply_scroll();
}

namespace
{

/// Redraws one cell when it is in the drawn window (an override set or cleared).
/// @param x  metatile column
/// @param y  metatile row
void redraw_cell(int x, int y)
{
   if (x >= g.drawn_x && x < g.drawn_x + kViewCols && y >= g.drawn_y && y < g.drawn_y + kViewRows)
   {
      draw_metatile(x, y);
      queue_rings();
   }
}

}   // namespace

bool set_cell_override(int x, int y, const uint16_t* entries)
{
   if (g.map == nullptr)
   {
      return false;
   }
   CellOverride* slot = nullptr;
   for (CellOverride& o : g.overrides)
   {
      if (o.used && o.x == x && o.y == y)
      {
         slot = &o;   // the cell's own override is replaced
         break;
      }
      if (!o.used && slot == nullptr)
      {
         slot = &o;
      }
   }
   if (slot == nullptr)
   {
      return false;
   }
   slot->used = true;
   slot->x = static_cast<int16_t>(x);
   slot->y = static_cast<int16_t>(y);
   for (int l = 0; l < g.terrain->layer_count; ++l)
   {
      for (int k = 0; k < kMetatileEntries; ++k)
      {
         slot->entries[l][k] = entries[l * kMetatileEntries + k];
      }
   }
   redraw_cell(x, y);
   return true;
}

void clear_cell_override(int x, int y)
{
   for (CellOverride& o : g.overrides)
   {
      if (o.used && o.x == x && o.y == y)
      {
         o.used = false;
         if (g.map != nullptr)
         {
            redraw_cell(x, y);
         }
      }
   }
}

void load_tiles(uint16_t first, const uint32_t* tiles, uint16_t count)
{
   ENGINE_ASSERT(first + count <= kMaxTiles);
   if (g.map == nullptr || count == 0)
   {
      return;
   }
   hal::vram::copy32(video::Region::BgVram, uint32_t{kTileCharBlock} * kCharBlockBytes + uint32_t{first} *
                     video::kTileBytes4bpp, tiles, uint32_t{count} * kTileWords4bpp, hal::vram::Priority::MustLand,
                     video::Source::Rom);
}

const DoorBinding* door_at(int x, int y)
{
   if (g.map == nullptr || g.map->doors == nullptr)
   {
      return nullptr;
   }
   const Map& m = *g.map;
   // cell bindings come first (a warp's door wins over its metatile's)
   for (uint8_t i = 0; i < m.door_count; ++i)
   {
      const DoorBinding& d = m.doors[i];
      if (d.x != kDoorAnyCell && d.x == x && d.y == y)
      {
         return &d;
      }
   }
   for (uint8_t i = 0; i < m.door_count; ++i)
   {
      const DoorBinding& d = m.doors[i];
      if (d.x != kDoorAnyCell)
      {
         continue;
      }
      for (int l = 0; l < m.layer_count; ++l)
      {
         if (metatile_at(m, l, x, y) == d.metatile)
         {
            return &d;
         }
      }
   }
   return nullptr;
}

bool set_override_set(uint16_t set, bool on)
{
   if (g.map == nullptr || set < g.sets_first || set >= g.sets_first + g.sets_count)
   {
      return false;
   }
   const uint32_t bit = 1u << (set - g.sets_first);
   g.sets_on = on ? (g.sets_on | bit) : (g.sets_on & ~bit);
   const OverrideSet& s = kWorld.override_sets[set];
   for (uint16_t n = 0; n < s.cell_count; ++n)
   {
      const OverrideCell& c = kWorld.override_cells[s.first_cell + n];
      redraw_cell(c.x, c.y);
   }
   return true;
}

bool override_set_on(uint16_t set)
{
   return g.map != nullptr && set >= g.sets_first && set < g.sets_first + g.sets_count &&
          (g.sets_on & (1u << (set - g.sets_first))) != 0;
}

uint8_t sub_priority(int32_t bottom, uint8_t elevation)
{
   return draw_order_of(kWorld.draw_order != nullptr ? *kWorld.draw_order : kDefaultDrawOrder, bottom, elevation);
}

bool set_cell(int x, int y, MapCell cell, const MetatileId* layers)
{
   if (g.map == nullptr)
   {
      return false;
   }
   if (g.edit_count == 0)
   {
      clear_edits();   // the shown map's first edit: earlier maps' entries (or zeros at start-up) go
   }
   uint32_t i = edit_hash(x, y);
   for (int n = 0; n < kMaxCellEdits; ++n, i = (i + 1) & kEditMask)
   {
      CellEdit& e = g_edits[i];
      const bool same = e.x == x && e.y == y;
      if (e.x != kNoEdit && !same)
      {
         continue;
      }
      if (!same)
      {
         if (g.edit_count == kMaxCellEdits)
         {
            return false;
         }
         ++g.edit_count;
         e.x = static_cast<int16_t>(x);
         e.y = static_cast<int16_t>(y);
      }
      e.cell = cell;
      for (int l = 0; l < kMaxLayers; ++l)
      {
         e.layers[l] = l < g.terrain->layer_count ? layers[l] : kNoMetatile;
      }
      return true;
   }
   return false;   // every slot holds another cell's edit
}

void draw_cell(int x, int y)
{
   if (g.map != nullptr)
   {
      redraw_cell(x, y);
   }
}

uint16_t cell_edit_count()
{
   return g.edit_count;
}

bool is_edited(int x, int y)
{
   return g.map != nullptr && find_edit(*g.map, x, y) != nullptr;
}

void set_layout(const Map* layout)
{
   if (g.map == nullptr)
   {
      return;
   }
   const Map& t = layout != nullptr ? *layout : *g.map;
   // the terrain's graphics load as at enter(); the camera stays (the reference games swap a layout as the map loads)
   show(*g.map, t, g.cam_x, g.cam_y, false);
}

const Map* terrain()
{
   return g.terrain;
}

} // namespace capsule::map
