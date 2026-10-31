/// @file
/// Asset lookup by key (see capsule/asset/asset.h).
#include "capsule/asset/asset.h"

namespace capsule::asset
{

const Entry* find(AssetKey key)
{
   int lo = 0;
   int hi = static_cast<int>(kTable.count) - 1;
   while (lo <= hi)
   {
      const int mid = (lo + hi) / kHalf;
      const Entry& e = kTable.entries[mid];
      if (raw(e.key) == raw(key))
      {
         return &e;
      }
      if (raw(e.key) < raw(key))
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

const Background* background(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Background) ? &kTable.backgrounds[e->index] : nullptr;
}

const Sprite* sprite(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Sprite) ? &kTable.sprites[e->index] : nullptr;
}

const List* list(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::List) ? &kTable.lists[e->index] : nullptr;
}

const text::Code* text(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Text) ? kTable.texts[e->index] : nullptr;
}

const text::PropFont* font(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Font && kTable.fonts != nullptr) ? &kTable.fonts[e->index] : nullptr;
}

const Door* door(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Door && kTable.doors != nullptr) ? &kTable.doors[e->index] : nullptr;
}

const TextBox* text_box(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::TextBox && kTable.text_boxes != nullptr) ? &kTable.text_boxes[e->index]
                                                                                       : nullptr;
}

const Tiles* tiles(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Tiles && kTable.tiles != nullptr) ? &kTable.tiles[e->index] : nullptr;
}

const Tilemap* tilemap(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Tilemap && kTable.tilemaps != nullptr) ? &kTable.tilemaps[e->index]
                                                                                    : nullptr;
}

const Palette* palette(AssetKey key)
{
   const Entry* e = find(key);
   return (e != nullptr && e->kind == Kind::Palette && kTable.palettes != nullptr) ? &kTable.palettes[e->index]
                                                                                   : nullptr;
}

} // namespace capsule::asset
