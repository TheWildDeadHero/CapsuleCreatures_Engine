/// @file
/// A screen's sprites (see capsule/screen/sprites.h).
#include "capsule/screen/sprites.h"

#include "capsule/core/assert.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::screen::sprites
{
namespace
{

namespace video = hal::video;

/// One tagged range of tiles.
struct Sheet
{
   uint16_t tag;      ///< Its tag, or kNoTag (free).
   uint16_t start;    ///< First tile.
   uint16_t count;    ///< Tiles.
};

constexpr int kBitsPerWord = 32;                      ///< Tiles per word of the allocation map.
constexpr int kMapWords = kTiles / kBitsPerWord;      ///< Words of the allocation map.
constexpr int kWordShift = 5;                         ///< Tile to map word: t >> 5.
constexpr int kBitMask = kBitsPerWord - 1;            ///< Tile to bit: t & 31.
constexpr int kTileWords = asset::kTileWords4bpp;     ///< Words of a 4bpp tile.
constexpr int16_t kWrapY = 256;                       ///< OAM y wraps here.
constexpr int16_t kLowY = video::kScreenHeight;       ///< OAM y from here counts as above the screen.
constexpr int16_t kDoubleLowY = 128;                  ///< ... from here for a size-3 2x-size sprite.
constexpr uint8_t kSizeLargest = 3;                   ///< OBJ size 3.
constexpr int kPriorityShift = 8;                     ///< Priority above the subpriority in a sort key.
constexpr uint8_t kBankMask = 15;                     ///< A bank number's bits (a missing palette's 0xFF -> 15).
constexpr int kHalf = 2;                              ///< Halves.
constexpr uint8_t kLastSubpriority = 0xFF;            ///< A free entry's subpriority (behind all).
constexpr int16_t kOffRight = 64;                     ///< A free entry's x past the screen's right edge.
/// Words of one queued piece (the queue's largest must-land request).
constexpr uint32_t kPieceWords = hal::vram::kMaxMustLandBytes / video::kWordBytes;

Sprite g_sprites[kMaxSprites];        ///< The sprites.
uint8_t g_order[kMaxSprites];         ///< The draw order (kept from frame to frame).
uint32_t g_tiles[kMapWords];          ///< Allocated tiles (bit per tile).
Sheet g_sheets[kSheets];              ///< Tagged ranges.
uint16_t g_palette_tags[kBanks];      ///< Each bank's tag, or kNoTag.
int16_t g_offset_x = 0;               ///< set_offset() x.
int16_t g_offset_y = 0;               ///< set_offset() y.
uint8_t g_drawn[kOamLimit];           ///< The sprite each OAM entry shows (kNoSprite: hidden).

/// The reference games' animation of nothing (a sprite without animations shows frame 0 and ends).
constexpr AnimCmd kStill[] = {{AnimOp::Frame, 0, 0, 0, 0}, {AnimOp::End, 0, 0, 0, 0}};
/// Its table.
constexpr const AnimCmd* kStillTable[] = {kStill};

/// A free sprite entry, as reset() leaves it (the reference games' dummy sprite: off the screen, last in order).
/// @return the entry
Sprite dummy()
{
   Sprite s{};
   s.x = video::kScreenWidth + kOffRight;   // off the right edge
   s.y = video::kScreenHeight;
   s.subpriority = kLastSubpriority;
   s.priority = video::kNumPriorities - 1;
   s.oam_y = static_cast<uint8_t>(video::kScreenHeight);
   return s;
}

/// Whether a tile is allocated.
/// @param t  tile 0 to kTiles - 1
/// @return true when it is
bool allocated(int t)
{
   return (g_tiles[t >> kWordShift] >> (t & kBitMask) & 1u) != 0;
}

/// Marks a tile allocated or free.
/// @param t   the tile
/// @param on  allocated
void mark(int t, bool on)
{
   const uint32_t bit = uint32_t{1} << (t & kBitMask);
   g_tiles[t >> kWordShift] = on ? (g_tiles[t >> kWordShift] | bit) : (g_tiles[t >> kWordShift] & ~bit);
}

/// First fit over the tiles (the reference games' allocator).
/// @param count  tiles
/// @return the first tile, or -1
int alloc_tiles(int count)
{
   if (count <= 0)
   {
      return -1;
   }
   int i = 0;
   for (;;)
   {
      while (allocated(i))
      {
         if (++i == kTiles)
         {
            return -1;
         }
      }
      const int start = i;
      int found = 1;
      while (found != count)
      {
         if (++i == kTiles)
         {
            return -1;
         }
         if (allocated(i))
         {
            break;
         }
         ++found;
      }
      if (found == count)
      {
         for (int t = start; t < start + count; ++t)
         {
            mark(t, true);
         }
         return start;
      }
   }
}

/// Queues raw tile words into sprite memory (must-land, split into pieces the queue takes).
/// @param first  first tile
/// @param words  the words
/// @param count  tiles
void queue_words(int first, const uint32_t* words, int count)
{
   const auto total = static_cast<uint32_t>(count * kTileWords);
   const auto base = static_cast<uint32_t>(first) * video::kTileBytes4bpp;
   for (uint32_t done = 0; done < total; done += kPieceWords)
   {
      const uint32_t n = total - done < kPieceWords ? total - done : kPieceWords;
      hal::vram::copy32(video::Region::ObjVram, base + done * video::kWordBytes, words + done, n,
                        hal::vram::Priority::MustLand, video::Source::Rom);
   }
}

/// A sheet entry by tag.
/// @param tag  the tag (kNoTag: a free entry)
/// @return its index, or -1
int sheet_of(uint16_t tag)
{
   for (int i = 0; i < kSheets; ++i)
   {
      if (g_sheets[i].tag == tag)
      {
         return i;
      }
   }
   return -1;
}

/// A sheet's tiles allocated and recorded.
/// @param tag    its tag
/// @param count  tiles
/// @return the first tile, or -1
int add_sheet(uint16_t tag, int count)
{
   const int start = alloc_tiles(count);
   const int free_entry = sheet_of(kNoTag);
   if (start < 0 || free_entry < 0)
   {
      return -1;
   }
   g_sheets[free_entry] = Sheet{tag, static_cast<uint16_t>(start), static_cast<uint16_t>(count)};
   return start;
}

/// One of a sprite's animation commands.
/// @param s      the sprite
/// @param index  command index in its animation
/// @return the command
const AnimCmd& cmd_of(const Sprite& s, int index)
{
   const AnimCmd* const* anims = s.tmpl->anims != nullptr ? s.tmpl->anims : kStillTable;
   return anims[s.anim][index];
}

/// Shows an animation frame: the tile (a sheet's offset, or the images' frame copied into the sprite's tiles).
/// @param s  the sprite
/// @param c  the frame command
void show_frame(Sprite& s, const AnimCmd& c)
{
   s.delay = c.duration > 0 ? static_cast<uint8_t>(c.duration - 1) : 0;
   if (s.tmpl->affine == kAffineOff)
   {
      s.flips = c.flips;
   }
   if (s.tmpl->tile_tag != kNoTag)
   {
      s.tile = static_cast<uint16_t>(s.sheet_start + c.value);
      return;
   }
   const Images* im = s.tmpl->images;
   if (im != nullptr && c.value >= 0 && c.value < im->frames)
   {
      queue_words(s.sheet_start, im->words + static_cast<uint32_t>(c.value) * im->frame_tiles * kTileWords,
                  im->frame_tiles);
   }
}

/// Goes back to the command after the previous loop marker (the reference games' JumpToTopOfAnimLoop).
/// @param s  the sprite
void jump_to_loop_top(Sprite& s)
{
   if (s.loop_left == 0)
   {
      return;
   }
   --s.cmd;
   while (cmd_of(s, s.cmd - 1).op != AnimOp::Loop)
   {
      if (s.cmd == 0)
      {
         break;
      }
      --s.cmd;
   }
   --s.cmd;
}

/// Continues a sprite's animation one frame (see continue_anim() below).
/// @param s  the sprite
void continue_anim(Sprite& s);

/// A loop command (begin or repeat).
/// @param s  the sprite
void loop_cmd(Sprite& s)
{
   if (s.loop_left != 0)
   {
      --s.loop_left;
   }
   else
   {
      s.loop_left = static_cast<uint8_t>(cmd_of(s, s.cmd).value);
   }
   jump_to_loop_top(s);
   continue_anim(s);
}

/// Starts a sprite's animation at its first command.
/// @param s  the sprite
void begin_anim(Sprite& s)
{
   s.cmd = 0;
   s.anim_ended = false;
   s.loop_left = 0;
   const AnimCmd& c = cmd_of(s, 0);
   if (c.op == AnimOp::End)
   {
      return;   // as the reference games: it begins again next frame
   }
   s.anim_beginning = false;
   show_frame(s, c);
}

/// Counts a frame of the command shown, or moves to the next command.
/// @param s  the sprite
void continue_anim(Sprite& s)
{
   if (s.delay != 0)
   {
      if (!s.anim_paused)
      {
         --s.delay;
      }
      if (s.tmpl->affine == kAffineOff)
      {
         s.flips = cmd_of(s, s.cmd).flips;
      }
      return;
   }
   if (s.anim_paused)
   {
      return;
   }
   ++s.cmd;
   const AnimCmd& c = cmd_of(s, s.cmd);
   switch (c.op)
   {
   case AnimOp::Frame:
      show_frame(s, c);
      break;
   case AnimOp::End:
      --s.cmd;
      s.anim_ended = true;
      break;
   case AnimOp::Jump:
      s.cmd = static_cast<uint8_t>(c.value);
      show_frame(s, cmd_of(s, s.cmd));
      break;
   case AnimOp::Loop:
      loop_cmd(s);
      break;
   default:
      break;
   }
}

/// The sort key's y of a sprite: its OAM y, from 160 up counted as negative (and from 128 for size-3 2x-size
/// square or tall sprites).
/// @param s  the sprite
/// @return y
int16_t sort_y(const Sprite& s)
{
   int16_t y = s.oam_y;
   if (y >= kLowY)
   {
      y = static_cast<int16_t>(y - kWrapY);
   }
   if (s.tmpl != nullptr && s.tmpl->affine == kAffineDouble && s.tmpl->size == kSizeLargest &&
       (s.tmpl->shape == video::OBJ_SQUARE || s.tmpl->shape == video::OBJ_TALL) && y > kDoubleLowY)
   {
      y = static_cast<int16_t>(y - kWrapY);
   }
   return y;
}

/// A sprite's sort key: priority, then subpriority.
/// @param s  the sprite
/// @return the key
uint16_t sort_key(const Sprite& s)
{
   return static_cast<uint16_t>(s.priority << kPriorityShift | s.subpriority);
}

/// Whether the sprite before belongs after the one behind it.
/// @param a  the one now first
/// @param b  the one after it
/// @return true when they swap
bool swaps(const Sprite& a, const Sprite& b)
{
   const uint16_t ka = sort_key(a);
   const uint16_t kb = sort_key(b);
   return ka > kb || (ka == kb && sort_y(a) < sort_y(b));
}

/// Creates a sprite in an entry.
/// @param index        the entry
/// @param t            its template
/// @param x            centre x
/// @param y            centre y
/// @param subpriority  lower in front
/// @return the entry, or kNoSprite when its tiles do not fit
uint8_t create_in(int index, const Template& t, int16_t x, int16_t y, uint8_t subpriority)
{
   Sprite& s = g_sprites[index];
   s = dummy();
   s.in_use = true;
   s.anim_beginning = true;
   s.tmpl = &t;
   s.callback = t.callback;
   s.subpriority = subpriority;
   s.priority = t.priority;
   s.x = x;
   s.y = y;
   const int dbl = t.affine == kAffineDouble ? kHalf : 1;
   s.center_x = static_cast<int8_t>(-video::obj_width(t.shape, t.size) / kHalf * dbl);
   s.center_y = static_cast<int8_t>(-video::obj_height(t.shape, t.size) / kHalf * dbl);
   if (t.tile_tag == kNoTag)
   {
      const int frame_tiles = t.images != nullptr ? t.images->frame_tiles : 0;
      const int start = alloc_tiles(frame_tiles);
      if (start < 0)
      {
         s = dummy();
         return kNoSprite;
      }
      s.sheet_start = static_cast<uint16_t>(start);
      s.tile = s.sheet_start;
   }
   else
   {
      s.sheet_start = sheet_start(t.tile_tag);
      const int16_t first = cmd_of(s, 0).op == AnimOp::Frame ? cmd_of(s, 0).value : int16_t{0};
      s.tile = static_cast<uint16_t>(s.sheet_start + (first < 0 ? 0 : first));
   }
   s.bank = t.palette_tag != kNoTag ? static_cast<uint8_t>(palette_bank(t.palette_tag) & kBankMask) : t.bank;
   return static_cast<uint8_t>(index);
}

}   // namespace

void reset()
{
   for (int i = 0; i < kMaxSprites; ++i)
   {
      g_sprites[i] = dummy();
      g_order[i] = static_cast<uint8_t>(i);
   }
   for (uint32_t& w : g_tiles)
   {
      w = 0;
   }
   for (Sheet& sh : g_sheets)
   {
      sh = Sheet{kNoTag, 0, 0};
   }
   for (uint16_t& tag : g_palette_tags)
   {
      tag = kNoTag;
   }
   g_offset_x = 0;
   g_offset_y = 0;
   for (uint8_t& d : g_drawn)
   {
      d = kNoSprite;
   }
   for (int n = 0; n < video::kNumObjs; ++n)
   {
      video::set_obj(static_cast<uint8_t>(n), video::kObjHidden);
   }
   video::set_obj_layer(true);
}

uint16_t load_sheet(uint16_t tag, const asset::Tiles& tiles)
{
   ENGINE_ASSERT(tiles.bpp == asset::kTiles4bpp);
   const int start = add_sheet(tag, tiles.count);
   if (start < 0)
   {
      return kNoTiles;
   }
   video::load_obj_tiles(static_cast<video::TileIndex>(start), tiles.words, tiles.count);
   return static_cast<uint16_t>(start);
}

uint16_t load_sheet_words(uint16_t tag, const uint32_t* words, uint16_t count)
{
   const int start = add_sheet(tag, count);
   if (start < 0)
   {
      return kNoTiles;
   }
   queue_words(start, words, count);
   return static_cast<uint16_t>(start);
}

void copy_tiles(uint16_t first_tile, const uint32_t* words, uint16_t count)
{
   if (static_cast<int>(first_tile) + count > kTiles)
   {
      return;   // past sprite memory: nothing (the reference games' copy would write past OBJ VRAM)
   }
   queue_words(first_tile, words, count);
}

uint16_t reserve_sheet(uint16_t tag, uint16_t count)
{
   const int start = add_sheet(tag, count);
   return start < 0 ? kNoTiles : static_cast<uint16_t>(start);
}

void free_sheet(uint16_t tag)
{
   const int i = sheet_of(tag);
   if (i < 0 || tag == kNoTag)
   {
      return;
   }
   for (int t = g_sheets[i].start; t < g_sheets[i].start + g_sheets[i].count; ++t)
   {
      mark(t, false);
   }
   g_sheets[i] = Sheet{kNoTag, 0, 0};
}

uint16_t sheet_start(uint16_t tag)
{
   const int i = sheet_of(tag);
   return i < 0 || tag == kNoTag ? kNoTiles : g_sheets[i].start;
}

uint8_t palette_bank(uint16_t tag)
{
   for (int b = 0; b < kBanks; ++b)
   {
      if (g_palette_tags[b] == tag)
      {
         return static_cast<uint8_t>(b);
      }
   }
   return kNoBank;
}

uint8_t alloc_palette(uint16_t tag)
{
   const uint8_t bank = palette_bank(kNoTag);
   if (bank != kNoBank)
   {
      g_palette_tags[bank] = tag;
   }
   return bank;
}

uint8_t load_palette(uint16_t tag, const video::Color* colors)
{
   const uint8_t known = palette_bank(tag);
   if (known != kNoBank)
   {
      return known;
   }
   const uint8_t bank = alloc_palette(tag);
   if (bank != kNoBank)
   {
      gfx::palette::load_obj(static_cast<uint8_t>(bank * gfx::palette::kBankColors), colors,
                             gfx::palette::kBankColors);
   }
   return bank;
}

void free_palette(uint16_t tag)
{
   const uint8_t bank = palette_bank(tag);
   if (bank != kNoBank && tag != kNoTag)
   {
      g_palette_tags[bank] = kNoTag;
   }
}

uint8_t create(const Template& t, int16_t x, int16_t y, uint8_t subpriority)
{
   for (int i = 0; i < kMaxSprites; ++i)
   {
      if (!g_sprites[i].in_use)
      {
         return create_in(i, t, x, y, subpriority);
      }
   }
   return kNoSprite;
}

uint8_t create_at_end(const Template& t, int16_t x, int16_t y, uint8_t subpriority)
{
   for (int i = kMaxSprites - 1; i >= 0; --i)
   {
      if (!g_sprites[i].in_use)
      {
         return create_in(i, t, x, y, subpriority);
      }
   }
   return kNoSprite;
}

void destroy(uint8_t id)
{
   ENGINE_ASSERT(id < kMaxSprites);
   Sprite& s = g_sprites[id];
   if (!s.in_use)
   {
      return;
   }
   if (s.tmpl->tile_tag == kNoTag && s.tmpl->images != nullptr)
   {
      for (int t = s.sheet_start; t < s.sheet_start + s.tmpl->images->frame_tiles; ++t)
      {
         mark(t, false);
      }
   }
   s = dummy();
}

Sprite& sprite(uint8_t id)
{
   ENGINE_ASSERT(id < kMaxSprites);
   return g_sprites[id];
}

void start_anim(uint8_t id, uint8_t anim)
{
   Sprite& s = sprite(id);
   s.anim = anim;
   s.anim_beginning = true;
   s.anim_ended = false;
}

void animate()
{
   for (Sprite& s : g_sprites)
   {
      if (!s.in_use)
      {
         continue;
      }
      if (s.callback != nullptr)
      {
         s.callback(s);
      }
      if (!s.in_use)
      {
         continue;   // its callback destroyed it
      }
      if (s.anim_beginning)
      {
         begin_anim(s);
      }
      else
      {
         continue_anim(s);
      }
   }
}

void build_oam()
{
   for (Sprite& s : g_sprites)
   {
      if (s.in_use && !s.invisible)
      {
         const int16_t oy = s.coord_offset ? g_offset_y : int16_t{0};
         s.oam_y = static_cast<uint8_t>(s.y + s.y2 + s.center_y + oy);   // OAM keeps 8 bits of y
      }
   }
   // the reference games' stable insertion sort over the last order (every entry, free ones too)
   for (int i = 1; i < kMaxSprites; ++i)
   {
      int j = i;
      while (j > 0 && swaps(g_sprites[g_order[j - 1]], g_sprites[g_order[j]]))
      {
         const uint8_t t = g_order[j];
         g_order[j] = g_order[j - 1];
         g_order[j - 1] = t;
         --j;
      }
   }
   int entry = 0;
   for (int i = 0; i < kMaxSprites && entry < kOamLimit; ++i)
   {
      const Sprite& s = g_sprites[g_order[i]];
      if (!s.in_use || s.invisible)
      {
         continue;
      }
      const int16_t ox = s.coord_offset ? g_offset_x : int16_t{0};
      const int16_t oy = s.coord_offset ? g_offset_y : int16_t{0};
      const uint8_t flips = static_cast<uint8_t>(s.flips ^ s.mirror);
      video::ObjDesc d{};
      d.x = static_cast<int16_t>(s.x + s.x2 + s.center_x + ox);
      d.y = static_cast<int16_t>(s.y + s.y2 + s.center_y + oy);
      d.tile = s.tile;
      d.shape = s.tmpl->shape;
      d.size = s.tmpl->size;
      d.palette_bank = s.bank;
      d.priority = s.priority;
      d.mode = s.tmpl->mode;
      d.affine = s.tmpl->affine != kAffineOff;
      d.double_size = s.tmpl->affine == kAffineDouble;
      d.affine_group = s.tmpl->matrix;
      d.hflip = (flips & kFlipH) != 0;
      d.vflip = (flips & kFlipV) != 0;
      g_drawn[entry] = g_order[i];
      video::set_obj(static_cast<uint8_t>(entry++), video::make_obj(d));
   }
   for (; entry < kOamLimit; ++entry)
   {
      g_drawn[entry] = kNoSprite;
      video::set_obj(static_cast<uint8_t>(entry), video::kObjHidden);
   }
}

void set_offset(int16_t x, int16_t y)
{
   g_offset_x = x;
   g_offset_y = y;
}

void set_matrix(uint8_t matrix, int16_t pa, int16_t pb, int16_t pc, int16_t pd)
{
   ENGINE_ASSERT(matrix < kMatrices);
   video::set_obj_affine(matrix, pa, pb, pc, pd);
}

uint8_t drawn(int entry)
{
   ENGINE_ASSERT(entry >= 0 && entry < kOamLimit);
   return g_drawn[entry];
}

int count()
{
   int n = 0;
   for (const Sprite& s : g_sprites)
   {
      n += s.in_use ? 1 : 0;
   }
   return n;
}

int tiles_used()
{
   int n = 0;
   for (int t = 0; t < kTiles; ++t)
   {
      n += allocated(t) ? 1 : 0;
   }
   return n;
}

}   // namespace capsule::screen::sprites
