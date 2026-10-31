/// @file
/// Locations (D-17): named places maps belong to (like the map sections of
/// the original handheld games). A location has a name, a banner style (the
/// name banner that slides in when the player enters it) and optional
/// previews (a short sequence shown full screen between maps).
/// Generated with the world by tools/map_compiler.py
/// (tools/location_compiler.py); the engine carries an empty default.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/hal/video.h"
#include "capsule/seq/sequence.h"
#include "capsule/text/encode.h"

#include <cstdint>

/// The overworld runner and its data.
namespace capsule::overworld
{

/// How a banner looks (ROM).
struct BannerStyle
{
   const uint32_t* frame;           ///< 9 4bpp tiles (corners, edges, interior), or nullptr: a plain box.
   const hal::video::Color* colors; ///< Palette entries 1-15 (1 ink, 2 shadow, 3 fill, 4 the box frame).
   uint16_t frames;                 ///< Frames it stays fully shown.
   uint8_t slide;                   ///< Frames it takes to slide in (and out).
   uint8_t reserved;                ///< 0.
};

/// A location preview (ROM): a sequence, shown when its conditions hold.
struct Preview
{
   const seq::Sequence* sequence;   ///< The sequence (full screen; the map is hidden).
   uint32_t conditions;             ///< Encounter condition bits that must all be set (0: always).
};

/// When a location's preview shows.
enum class PreviewRule : uint8_t
{
   Always,          ///< Every time the player enters the location.
   FirstVisit,      ///< Only the first time (a permanent bit per location).
   NUM_RULES        ///< Number of rules.
};

inline constexpr uint8_t kNoBanner = 0xFF;   ///< Location::banner: none.

/// A location (ROM).
struct Location
{
   const text::Code* name;   ///< Its name (encoded, kCodeEnd).
   uint16_t preview_first;   ///< First of its previews in Locations::previews (the last that holds wins).
   uint8_t preview_count;    ///< Previews.
   uint8_t banner;           ///< Banner style index, or kNoBanner.
   PreviewRule rule;         ///< When the preview shows.
   uint8_t reserved[kPadAfterOneByte];      ///< 0.
};

/// Every location of the world.
struct Locations
{
   const Location* locations;       ///< location_count locations (map::Map::location indexes them).
   const BannerStyle* styles;       ///< Banner styles.
   const Preview* previews;         ///< Previews of every location.
   uint8_t* visited;                ///< One bit per location (RAM, permanent, part of the save).
   uint16_t location_count;         ///< Locations.
   uint16_t visited_bytes;          ///< Bytes of visited.
   uint8_t style_count;             ///< Banner styles.
   uint8_t default_transition;      ///< script::Transition of warps that name none.
   uint8_t reserved[kPadAfterTwoBytes];             ///< 0.
};

/// The build's locations, generated with the world.
extern const Locations kLocations;

} // namespace capsule::overworld
