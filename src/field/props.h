/// @file
/// The field's props (C2, capsule/field/field.h place_prop()): the hooks field.cpp calls. Internal to the field.
#pragma once

#include "capsule/asset/asset.h"

#include <cstdint>

namespace capsule::field::props
{

/// Drops every prop and frees their sprite slots and instances (a new map: field.cpp's reset_cutscene()).
void reset();

/// Draws every prop for this frame (after the camera moved; field::update()).
void draw();

/// A sprite's standing frame for a facing, as the field shows a standing object (field.cpp, by its animations).
/// @param sprite  the sprite
/// @param facing  DIR_*
/// @param frame   receives the frame
/// @param flips   receives its asset::kFlip... bits
void standing_frame(const asset::Sprite& sprite, uint8_t facing, uint8_t& frame, uint8_t& flips);

} // namespace capsule::field::props
