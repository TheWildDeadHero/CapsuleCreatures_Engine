/// @file
/// Sprite slots (see capsule/gfx/sprites.h).
#include "capsule/gfx/sprites.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/vram_queue.h"

namespace capsule::gfx::sprites
{
namespace
{

namespace video = hal::video;

/// Q14 sine over Q8 scale gives Q8 matrix entries when multiplied by this: 256 * 256 / 16384.
constexpr int32_t kMatrixFactor = kQ8One * kQ8One / kQ14One;
/// Bits of a hardware draw priority (0 front to 3).
constexpr uint8_t kPriorityMask = 3;
/// Affine sprites draw in a box twice their size, so rotated corners are not clipped.
constexpr int kDoubleSize = 2;
/// Slot::uploaded: no frame in VRAM yet.
constexpr uint8_t kNoFrame = 0xFF;
/// 32-bit words of one 4bpp tile.
constexpr uint32_t kTileWords = static_cast<uint32_t>(video::kTileBytes4bpp) / sizeof(uint32_t);

/// One sprite slot.
struct Slot
{
   const asset::Sprite* sprite;   ///< Loaded sprite, or nullptr.
   int16_t x;                    ///< Left edge, pixels.
   int16_t y;                    ///< Top edge, pixels.
   uint16_t scale;                ///< Q8; 256 = 1.0.
   uint16_t angle;                ///< 16-bit turn.
   uint8_t shown;                ///< 1 while shown.
   video::ObjMode mode;           ///< Drawing mode.
   uint8_t animation;            ///< Playing animation.
   uint8_t step;                 ///< Position in the animation.
   uint8_t timer;                ///< Frames shown of the current step.
   uint8_t priority;             ///< 0 (front) to 3; see set_priority().
   bool paused;                  ///< I3: the animation stays on its step.
   bool half;                    ///< I3: half speed (the timer runs on every other frame).
   bool tick;                    ///< I3: half speed's frame toggle.
   uint8_t uploaded;             ///< I3: streamed sprites: the frame in the slot's tiles, or kNoFrame.
   uint8_t sub;                  ///< DS1: sub-priority among sprites of equal priority (0 front).
   int16_t top;                  ///< DS1: top edge on screen as last written (the sort's tie-break).
   uint8_t set_count;            ///< C2: frames load_frames() loaded (0: the sprite's own, as load()).
   uint8_t set_frames[kMaxSetFrames];   ///< C2: those frames, as the sprite numbers them.
   uint8_t generation;           ///< C2: counts the slot's loads (an instance of an earlier load draws nothing).
};
CAPSULE_EWRAM_BSS Slot g_slots[kSlots];   ///< The slots (D-42: EWRAM, the main loop's).

/// C2: one instance (a further copy of a slot's sprite).
struct Instance
{
   bool used;            ///< Taken (zero-initialised: free).
   uint8_t slot;         ///< Its slot.
   uint8_t generation;   ///< The slot's load it was given a frame of (Slot::generation).
   int16_t x;            ///< Left edge, pixels.
   int16_t y;            ///< Top edge, pixels.
   uint8_t frame;        ///< The frame among the slot's tiles.
   uint8_t flips;        ///< asset::kFlip... bits.
   uint8_t priority;     ///< 0 (front) to 3.
   uint8_t sub;          ///< Sub-priority (0 front).
   bool shown;           ///< Shown.
   int16_t top;          ///< Top edge on screen as last written (the sort's tie-break).
};
CAPSULE_EWRAM_BSS Instance g_instances[kInstances];   ///< The instances (EWRAM).

/// DS1: the sorted draw order (an order of the drawables, front first; kept from frame to frame). Drawable d is
/// slot d below kSlots, else instance d - kSlots (C2).
struct Order
{
   bool sorted;                  ///< Sorting is on.
   uint8_t items[kEntries];      ///< The drawables, front first.
   uint8_t entry[kEntries];      ///< The OAM entry each drawable was written to.
};
CAPSULE_EWRAM_BSS Order g_order;   ///< The draw order (EWRAM).
CAPSULE_EWRAM_BSS video::ObjAttr g_attrs[kEntries];   ///< Each drawable's entry in the update being written.
uint8_t g_first_entry = 0;   ///< T1: the OAM entry of the draw order's first slot (set_first_entry()).

/// Bits the priority is shifted by above the sub-priority in a sort key.
constexpr int kPriorityKeyShift = 8;

/// DS1: a drawable's sort key: priority, then sub-priority (lower in front).
/// @param d  the drawable (a slot below kSlots, else an instance)
/// @return the key
uint16_t sort_key(uint8_t d)
{
   if (d < kSlots)
   {
      return static_cast<uint16_t>(g_slots[d].priority << kPriorityKeyShift | g_slots[d].sub);
   }
   const Instance& i = g_instances[d - kSlots];
   return static_cast<uint16_t>(i.priority << kPriorityKeyShift | i.sub);
}

/// DS1: a drawable's top edge as last written.
/// @param d  the drawable
/// @return it
int16_t top_of(uint8_t d)
{
   return d < kSlots ? g_slots[d].top : g_instances[d - kSlots].top;
}

/// DS1: whether the drawable before belongs behind the drawable after (lower key in front; equal keys: the lower
/// sprite on screen in front).
/// @param before  the drawable now in front
/// @param after   the drawable now behind it
/// @return true when they swap
bool behind(uint8_t before, uint8_t after)
{
   const uint16_t a = sort_key(before);
   const uint16_t b = sort_key(after);
   return a > b || (a == b && top_of(before) < top_of(after));
}

/// DS1: sorts the draw order: an insertion sort over the last frame's order, so equal sprites keep it.
void sort_order()
{
   for (int i = 1; i < kEntries; ++i)
   {
      int j = i;
      while (j > 0 && behind(g_order.items[j - 1], g_order.items[j]))
      {
         const uint8_t t = g_order.items[j];
         g_order.items[j] = g_order.items[j - 1];
         g_order.items[j - 1] = t;
         --j;
      }
   }
}

/// C2: uploads a slot's frame set (load_frames()), one frame after another.
/// @param n  the slot
void upload_set(int n)
{
   const Slot& s = g_slots[n];
   const auto tiles = static_cast<uint32_t>(asset::tiles_per_frame(*s.sprite));
   for (int k = 0; k < s.set_count; ++k)
   {
      hal::vram::copy32(video::Region::ObjVram,
                        static_cast<uint32_t>((n * kTilesPerSlot + k * static_cast<int>(tiles)) * video::kTileBytes4bpp),
                        s.sprite->tiles + static_cast<uint32_t>(s.set_frames[k]) * tiles * kTileWords, tiles * kTileWords,
                        hal::vram::Priority::MustLand, video::Source::Rom);
   }
}

/// Writes a slot's OBJ matrix: the inverse of scale and rotation.
/// @param n  slot (= matrix)
void write_matrix(int n)
{
   const Slot& s = g_slots[n];
   const int32_t c = cos_q14(s.angle);
   const int32_t sn = sin_q14(s.angle);
   // Q14 / Q8 scale -> Q8: v * 256 / (16384 * scale / 256) = v * 4 / scale.
   const int32_t scale = s.scale == 0 ? 1 : s.scale;
   const auto pa = static_cast<int16_t>(c * kMatrixFactor / scale);
   const auto pb = static_cast<int16_t>(sn * kMatrixFactor / scale);
   video::set_obj_affine(static_cast<uint8_t>(n), pa, pb, static_cast<int16_t>(-pb), pa);
}

/// Frame the slot shows now.
/// @param s  the slot
/// @return frame index
int current_frame(const Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   return a.frames[s.step];
}

/// The flip bits of the entry the slot shows now (I3).
/// @param s  the slot
/// @return asset::kFlip... bits
uint8_t current_flips(const Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   return a.flips != nullptr ? a.flips[s.step] : uint8_t{0};
}

/// Whether a sprite keeps only its shown frame in VRAM (I3).
/// @param sprite  the sprite
/// @return true for a streamed sprite
bool streamed(const asset::Sprite& sprite)
{
   return (sprite.flags & asset::kSpriteStreamed) != 0;
}

/// Queues a streamed slot's frame into its tiles when it shows another one (I3).
/// @param n  the slot
/// @param frame  the frame it shows
void stream_frame(int n, int frame)
{
   Slot& s = g_slots[n];
   if (s.uploaded == frame)
   {
      return;
   }
   s.uploaded = static_cast<uint8_t>(frame);
   const auto tiles = static_cast<uint32_t>(asset::tiles_per_frame(*s.sprite));
   hal::vram::copy32(video::Region::ObjVram, static_cast<uint32_t>(n * kTilesPerSlot * video::kTileBytes4bpp),
                     s.sprite->tiles + static_cast<uint32_t>(frame) * tiles * kTileWords, tiles * kTileWords,
                     hal::vram::Priority::MustLand, video::Source::Rom);
}

/// Advances a slot's animation when its entry's time is over (I3: before the entry is shown, so a caller between
/// two updates sees the entry last shown).
/// @param s  the slot
void advance(Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   const uint8_t span = a.durations != nullptr ? a.durations[s.step] : a.period;
   if (a.count <= 1 || s.paused || s.timer < span)
   {
      return;
   }
   s.timer = 0;
   if (s.step + 1 < a.count)
   {
      ++s.step;
   }
   else if (a.loop)
   {
      s.step = 0;
   }
}

/// Counts a frame of the entry shown (half speed: every other frame); the count stops at the entry's span.
/// @param s  the slot
void count_frame(Slot& s)
{
   const asset::Animation& a = s.sprite->animations[s.animation];
   const uint8_t span = a.durations != nullptr ? a.durations[s.step] : a.period;
   s.tick = !s.tick;
   if (s.half && s.tick)
   {
      return;
   }
   if (s.timer < span)
   {
      ++s.timer;
   }
}

} // namespace

void reset()
{
   for (int n = 0; n < kSlots; ++n)
   {
      g_slots[n] = Slot{};
      g_slots[n].scale = kQ8One;
      video::set_obj(static_cast<uint8_t>(g_first_entry + n), video::kObjHidden);
   }
   for (int k = kSlots; k < kEntries; ++k)
   {
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), video::kObjHidden);
   }
   for (Instance& i : g_instances)
   {
      i = Instance{};
   }
   g_first_entry = 0;   // T1: entry n is slot n's again
   set_sorted(false);   // DS1: OAM entry n is slot n again
   video::set_obj_layer(true);
}

void load(uint8_t slot, const asset::Sprite& sprite)
{
   ENGINE_ASSERT(slot < kSlots);
   const int frame_tiles = asset::tiles_per_frame(sprite);
   const int tiles = streamed(sprite) ? frame_tiles : frame_tiles * sprite.frame_count;
   ENGINE_ASSERT(tiles <= kTilesPerSlot && sprite.animation_count >= 1);
   Slot& s = g_slots[slot];
   s.sprite = &sprite;
   s.animation = 0;
   s.step = 0;
   s.timer = 0;
   s.paused = false;
   s.half = false;
   s.set_count = 0;
   ++s.generation;   // C2: its instances drew the sprite before
   // a streamed sprite loads the frame its first animation starts on; update() streams the others
   const int first = streamed(sprite) ? current_frame(s) : 0;
   s.uploaded = streamed(sprite) ? static_cast<uint8_t>(first) : kNoFrame;
   video::load_obj_tiles(static_cast<video::TileIndex>(slot * kTilesPerSlot),
                         sprite.tiles + static_cast<uint32_t>(first * frame_tiles) * kTileWords,
                         static_cast<uint16_t>(tiles));
   // Slot n uses OBJ bank n; color 0 is transparent and never loaded.
   palette::load_obj(static_cast<uint8_t>(slot * palette::kBankColors + 1), sprite.palette + 1,
                     palette::kBankColors - 1);
}

void unload(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   ++g_slots[slot].generation;   // C2: its instances draw nothing now
   g_slots[slot].set_count = 0;
   g_slots[slot].sprite = nullptr;
   g_slots[slot].shown = 0;
   g_slots[slot].uploaded = kNoFrame;
}

void show(uint8_t slot, bool on)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].shown = on ? 1 : 0;
}

void set_position(uint8_t slot, int16_t x, int16_t y)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].x = x;
   g_slots[slot].y = y;
}

void play(uint8_t slot, uint16_t animation)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   if (s.sprite == nullptr || animation >= s.sprite->animation_count)
   {
      return;
   }
   s.animation = static_cast<uint8_t>(animation);
   s.step = 0;
   s.timer = 0;
}

void play_at(uint8_t slot, uint16_t animation, uint8_t entry)
{
   ENGINE_ASSERT(slot < kSlots);
   Slot& s = g_slots[slot];
   if (s.sprite == nullptr || animation >= s.sprite->animation_count)
   {
      return;
   }
   const asset::Animation& a = s.sprite->animations[animation];
   s.animation = static_cast<uint8_t>(animation);
   s.step = entry < a.count ? entry : static_cast<uint8_t>(a.count - 1);
   s.timer = 0;
}

void pause(uint8_t slot, bool paused)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].paused = paused;
}

void set_half_speed(uint8_t slot, bool half)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].half = half;
}

uint16_t animation(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_slots[slot].animation;
}

uint8_t entry(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_slots[slot].step;
}

void set_transform(uint8_t slot, uint16_t scale_q8, uint16_t angle)
{
   ENGINE_ASSERT(slot < kAffineSlots);
   g_slots[slot].scale = scale_q8;
   g_slots[slot].angle = angle;
}

void set_priority(uint8_t slot, uint8_t priority)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].priority = static_cast<uint8_t>(priority & kPriorityMask);
}

void set_mode(uint8_t slot, video::ObjMode mode)
{
   ENGINE_ASSERT(slot < kSlots && mode < video::ObjMode::NUM_OBJ_MODES);
   g_slots[slot].mode = mode;
}

void set_order(uint8_t slot, uint8_t sub_priority)
{
   ENGINE_ASSERT(slot < kSlots);
   g_slots[slot].sub = sub_priority;
}

void set_sorted(bool on)
{
   if (on == g_order.sorted && on)
   {
      return;   // the order of the frames before is kept
   }
   g_order.sorted = on;
   for (int n = 0; n < kEntries; ++n)
   {   // a new order starts from the slots' own, the instances after them
      g_order.items[n] = static_cast<uint8_t>(n);
      g_order.entry[n] = static_cast<uint8_t>(n);
   }
}

uint8_t oam_entry(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots);
   return g_order.entry[slot];
}

namespace
{

/// Advances a slot's animation one frame and gives its OAM entry (I3: the shown frame streamed in; DS1: its top
/// edge kept for the sort).
/// @param n  the slot
/// @return its attributes, or video::kObjHidden
video::ObjAttr slot_attr(int n)
{
   Slot& s = g_slots[n];
   if (s.sprite == nullptr || !s.shown || s.scale == 0)
   {
      return video::kObjHidden;
   }
   if (s.set_count > 0)
   {
      // C2: a frame set: the slot shows its first frame, unanimated
      video::ObjDesc d{};
      d.shape = s.sprite->shape;
      d.size = s.sprite->size;
      d.palette_bank = static_cast<uint8_t>(n);
      d.mode = s.mode;
      d.priority = s.priority;
      d.tile = static_cast<video::TileIndex>(n * kTilesPerSlot);
      d.x = s.x;
      d.y = s.y;
      s.top = d.y;
      const int w = video::obj_width(s.sprite->shape, s.sprite->size);
      const int h = video::obj_height(s.sprite->shape, s.sprite->size);
      if (d.x + w <= 0 || d.x >= video::kScreenWidth || d.y + h <= 0 || d.y >= video::kScreenHeight)
      {
         return video::kObjHidden;
      }
      return video::make_obj(d);
   }
   // Animation: once a step has been shown for its duration (or `period`) display frames, move on (loop or hold
   // the last frame), then show the step and count the frame (I3: advancing first keeps the step last shown visible
   // to callers between updates; what shows is unchanged)
   advance(s);
   const int frame = current_frame(s);
   const uint8_t flips = current_flips(s);
   count_frame(s);
   if (streamed(*s.sprite))
   {
      stream_frame(n, frame);
   }
   const int w = video::obj_width(s.sprite->shape, s.sprite->size);
   const int h = video::obj_height(s.sprite->shape, s.sprite->size);
   const bool transformed = n < kAffineSlots && (s.scale != kQ8One || s.angle != 0);
   video::ObjDesc d{};
   d.shape = s.sprite->shape;
   d.size = s.sprite->size;
   d.palette_bank = static_cast<uint8_t>(n);
   d.mode = s.mode;
   d.priority = s.priority;
   const int shown = streamed(*s.sprite) ? 0 : frame;   // a streamed slot's tiles hold the frame shown
   d.tile = static_cast<video::TileIndex>(n * kTilesPerSlot + shown * asset::tiles_per_frame(*s.sprite));
   d.hflip = (flips & asset::kFlipH) != 0;
   d.vflip = (flips & asset::kFlipV) != 0;
   if (transformed)
   {
      d.affine = true;
      d.double_size = true;
      d.affine_group = static_cast<uint8_t>(n);
      // The doubled box is centred on the sprite: it starts half a sprite up and left.
      d.x = static_cast<int16_t>(s.x - w / kHalf);
      d.y = static_cast<int16_t>(s.y - h / kHalf);
      write_matrix(n);
   }
   else
   {
      d.x = s.x;
      d.y = s.y;
   }
   s.top = d.y;
   // Off-screen positions: OAM coordinates wrap, so hide what cannot show.
   const int box_w = transformed ? w * kDoubleSize : w;
   const int box_h = transformed ? h * kDoubleSize : h;
   if (d.x + box_w <= 0 || d.x >= video::kScreenWidth || d.y + box_h <= 0 || d.y >= video::kScreenHeight)
   {
      return video::kObjHidden;
   }
   return video::make_obj(d);
}

/// C2: an instance's OAM entry.
/// @param k  the instance
/// @return its attributes, or video::kObjHidden
video::ObjAttr instance_attr(int k)
{
   Instance& i = g_instances[k];
   if (!i.used || !i.shown)
   {
      return video::kObjHidden;
   }
   const Slot& s = g_slots[i.slot];
   if (s.sprite == nullptr || i.generation != s.generation || (s.set_count == 0 && streamed(*s.sprite)))
   {
      return video::kObjHidden;   // the slot's tiles hold no frame of its (a streamed slot keeps one frame only)
   }
   const int frames = s.set_count > 0 ? s.set_count : s.sprite->frame_count;
   if (i.frame >= frames)
   {
      return video::kObjHidden;
   }
   const int w = video::obj_width(s.sprite->shape, s.sprite->size);
   const int h = video::obj_height(s.sprite->shape, s.sprite->size);
   i.top = i.y;
   if (i.x + w <= 0 || i.x >= video::kScreenWidth || i.y + h <= 0 || i.y >= video::kScreenHeight)
   {
      return video::kObjHidden;
   }
   video::ObjDesc d{};
   d.shape = s.sprite->shape;
   d.size = s.sprite->size;
   d.palette_bank = i.slot;   // the slot's palette
   d.mode = s.mode;
   d.priority = i.priority;
   d.tile = static_cast<video::TileIndex>(i.slot * kTilesPerSlot + i.frame * asset::tiles_per_frame(*s.sprite));
   d.hflip = (i.flips & asset::kFlipH) != 0;
   d.vflip = (i.flips & asset::kFlipV) != 0;
   d.x = i.x;
   d.y = i.y;
   return video::make_obj(d);
}

/// C2: an instance's record.
/// @param instance  the instance
/// @return it
Instance& instance_at(uint8_t instance)
{
   ENGINE_ASSERT(instance < kInstances && g_instances[instance].used);
   return g_instances[instance < kInstances ? instance : 0];
}

} // namespace

void load_frames(uint8_t slot, const asset::Sprite& sprite, const uint8_t* frames, uint8_t count)
{
   ENGINE_ASSERT(slot < kSlots && count >= 1 && count <= kMaxSetFrames);
   ENGINE_ASSERT(count * asset::tiles_per_frame(sprite) <= kTilesPerSlot);
   Slot& s = g_slots[slot];
   s.sprite = &sprite;
   s.animation = 0;
   s.step = 0;
   s.timer = 0;
   s.paused = true;
   s.half = false;
   s.uploaded = kNoFrame;
   ++s.generation;
   s.set_count = count <= kMaxSetFrames ? count : static_cast<uint8_t>(kMaxSetFrames);
   for (int k = 0; k < s.set_count; ++k)
   {
      ENGINE_ASSERT(frames[k] < sprite.frame_count);
      s.set_frames[k] = frames[k];
   }
   upload_set(slot);
   palette::load_obj(static_cast<uint8_t>(slot * palette::kBankColors + 1), sprite.palette + 1,
                     palette::kBankColors - 1);
}

uint8_t add_instance(uint8_t slot)
{
   ENGINE_ASSERT(slot < kSlots && g_slots[slot].sprite != nullptr);
   for (uint8_t k = 0; k < kInstances; ++k)
   {
      Instance& i = g_instances[k];
      if (!i.used)
      {
         i = Instance{};
         i.used = true;
         i.slot = slot;
         i.generation = g_slots[slot].generation;
         return k;
      }
   }
   return kNoInstance;
}

void remove_instance(uint8_t instance)
{
   if (instance < kInstances)
   {
      g_instances[instance].used = false;   // one reset() freed already is free
   }
}

void show_instance(uint8_t instance, bool on)
{
   instance_at(instance).shown = on;
}

void set_instance_position(uint8_t instance, int16_t x, int16_t y)
{
   Instance& i = instance_at(instance);
   i.x = x;
   i.y = y;
}

void set_instance_frame(uint8_t instance, uint8_t frame, uint8_t flips)
{
   Instance& i = instance_at(instance);
   i.frame = frame;
   i.flips = flips;
   i.generation = g_slots[i.slot].generation;   // the slot's load now
}

void set_instance_priority(uint8_t instance, uint8_t priority)
{
   instance_at(instance).priority = static_cast<uint8_t>(priority & kPriorityMask);
}

void set_instance_order(uint8_t instance, uint8_t sub_priority)
{
   instance_at(instance).sub = sub_priority;
}

uint8_t instance_entry(uint8_t instance)
{
   ENGINE_ASSERT(instance < kInstances);
   return g_order.entry[kSlots + (instance < kInstances ? instance : 0)];
}

void update()
{
   for (int n = 0; n < kSlots; ++n)
   {
      g_attrs[n] = slot_attr(n);
   }
   for (int k = 0; k < kInstances; ++k)
   {
      g_attrs[kSlots + k] = instance_attr(k);
   }
   if (g_order.sorted)
   {
      sort_order();
   }
   for (int k = 0; k < kEntries; ++k)
   {
      const uint8_t n = g_order.sorted ? g_order.items[k] : static_cast<uint8_t>(k);
      g_order.entry[n] = static_cast<uint8_t>(k);
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), g_attrs[n]);
   }
}

void set_first_entry(uint8_t first)
{
   ENGINE_ASSERT(first + kEntries <= video::kNumObjs);
   if (first == g_first_entry)
   {
      return;
   }
   for (int k = 0; k < kEntries; ++k)
   {
      video::set_obj(static_cast<uint8_t>(g_first_entry + k), video::kObjHidden);   // the entries left
   }
   g_first_entry = first;
}

void reupload()
{
   for (int n = 0; n < video::kNumObjs; ++n)
   {
      video::set_obj(static_cast<uint8_t>(n), video::kObjHidden);   // a scene's entries past the slots too
   }
   for (int n = 0; n < kSlots; ++n)
   {
      const Slot& s = g_slots[n];
      if (s.sprite == nullptr)
      {
         continue;
      }
      if (s.set_count > 0)
      {
         upload_set(n);   // C2: a frame set
         continue;
      }
      const int frame_tiles = asset::tiles_per_frame(*s.sprite);
      const bool stream = streamed(*s.sprite);
      const int first = stream ? s.uploaded : 0;   // a streamed slot's tiles hold the frame it last uploaded
      const int tiles = stream ? frame_tiles : frame_tiles * s.sprite->frame_count;
      if (stream && s.uploaded == kNoFrame)
      {
         continue;
      }
      hal::vram::copy32(video::Region::ObjVram, static_cast<uint32_t>(n * kTilesPerSlot * video::kTileBytes4bpp),
                        s.sprite->tiles + static_cast<uint32_t>(first * frame_tiles) * kTileWords,
                        static_cast<uint32_t>(tiles) * kTileWords, hal::vram::Priority::MustLand,
                        video::Source::Rom);
   }
}

} // namespace capsule::gfx::sprites
