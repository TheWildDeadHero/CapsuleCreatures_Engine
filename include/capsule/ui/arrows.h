/// @file
/// N3 (D-32, engine B2 core): a list's scroll arrows: two sprites (an up arrow above the list, a down arrow below
/// it) that bob along the vertical axis and show only while the list can scroll their way. Their sprite,
/// animations and bounce are the list's MenuStyle (arrow_sprite, arrow_up / arrow_down, arrow_amplitude,
/// arrow_speed); where they stand and which sprite slots they take are the caller's (capsule/gfx/slots.h: a screen
/// lends two of its slots while a list shows).
///
/// Frame by frame (update() once a frame): an arrow is hidden while the list is scrolled all the way its way (the
/// up arrow at scroll 0, the down arrow at the last scroll), else shown; its offset from its place is
/// amplitude * sine(phase) (the sine in 1/256 units, the product divided by 256 and cut towards zero), then its
/// phase moves arrow_speed sine steps (256 a turn; the down arrow's backwards). The first update() after open()
/// only starts the animations (they show nothing yet: hidden until then), so the arrows appear, unmoved, on the
/// second.
#pragma once

#include "capsule/ui/menu.h"

#include <cstdint>

namespace capsule::ui
{

/// The two arrows' state (plain data).
struct Arrows
{
   const MenuStyle* style;   ///< The list's style.
   uint8_t up_slot;          ///< Sprite slot of the up arrow.
   uint8_t down_slot;        ///< Sprite slot of the down arrow.
   int16_t x;                ///< Both arrows' left edge, pixels on screen.
   int16_t up_y;             ///< The up arrow's top edge.
   int16_t down_y;           ///< The down arrow's top edge.
   uint8_t up_phase;         ///< The up arrow's sine step.
   uint8_t down_phase;       ///< The down arrow's sine step.
   bool started;             ///< update() ran once since open().
   bool open;                ///< Shown.
};

/// Loads the arrows' sprite into the two slots, hidden (nothing happens when the style names no sprite).
/// @param a          the arrows
/// @param style      the list's style
/// @param up_slot    the up arrow's sprite slot
/// @param down_slot  the down arrow's sprite slot
/// @param x          their left edge, pixels on screen
/// @param up_y       the up arrow's top edge
/// @param down_y     the down arrow's top edge
void open(Arrows& a, const MenuStyle& style, uint8_t up_slot, uint8_t down_slot, int16_t x, int16_t up_y,
          int16_t down_y);

/// One frame: shows the arrows the list can scroll towards, bobs them.
/// @param a            the arrows
/// @param scroll       the list's first shown option
/// @param last_scroll  its scroll at the end (options - rows shown)
void update(Arrows& a, uint8_t scroll, uint8_t last_scroll);

/// Hides the arrows (their slots are free for the caller again).
/// @param a  the arrows
void close(Arrows& a);

/// One arrow's offset at a sine step: amplitude * sine / 256, cut towards zero (the sine in 1/256 units).
/// @param amplitude  pixels at the peak
/// @param phase      sine step (256 a turn)
/// @return pixels
int bounce(uint8_t amplitude, uint8_t phase);

}   // namespace capsule::ui
