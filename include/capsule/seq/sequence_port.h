/// @file
/// What the sequence player needs from the rest of the engine (P-8: plain
/// functions, one implementation linked per build).
///
/// - namespace port: engine systems. src/seq/sequence_port_hal.cpp implements
///   them on the current HAL; calls whose system does not exist yet (asset
///   table, sprites, blending, windows, audio, text box, map renderer) are
///   documented no-op stubs until it lands.
/// - namespace game: game hooks: conditions, lists and RNG entropy. The engine
///   ships src/seq/sequence_game_default.cpp (no conditions, empty lists); a
///   game builds its own file instead.
///
/// Tests link a recording mock of both namespaces; the sequence test app links
/// a recording, simulating version.
#pragma once

#include "capsule/core/types.h"

#include <cstdint>

/// Engine systems the sequence player drives.
namespace capsule::seq::port
{

/// @name Video (hal::video)
///@{

/// Selects the display mode.
/// @param mode  0, 1 or 2
void set_video_mode(uint8_t mode);

/// Loads a tiles/map/palette bundle onto a layer via the VRAM queue (A-5).
/// @param bg     layer 0-3
/// @param asset  background asset
void bg_load(uint8_t bg, AssetKey asset);

/// Shows or hides a layer.
/// @param bg  layer 0-3
/// @param on  show it
void bg_show(uint8_t bg, bool on);

/// Positions a layer: scroll for a regular layer, reference point for affine.
/// @param bg  layer 0-3
/// @param x   pixels
/// @param y   pixels
void bg_offset(uint8_t bg, int16_t x, int16_t y);

/// Sets the color special effect.
/// @param mode        BlendMode
/// @param src_layers  source layers: bit 0-3 BG, 4 OBJ, 5 backdrop
/// @param tgt_layers  target layers, same bits
void blend(uint8_t mode, uint8_t src_layers, uint8_t tgt_layers);

/// Sets the blend weights.
/// @param src  source weight, 0-16
/// @param tgt  target weight, 0-16
void blend_weights(uint8_t src, uint8_t tgt);

/// Sets the hardware brightness fade (A-6).
/// @param level  0 (none) to 16 (full)
/// @param white  toward white instead of black
void brightness(uint8_t level, bool white);

/// Software palette blend toward a color (A-6).
/// @param mask   palettes: bit 0-15 BG banks, 16-31 OBJ banks
/// @param color  BGR555 color to blend toward
/// @param coef   0 (none) to 16 (all color)
void palette_fade(uint32_t mask, uint16_t color, uint8_t coef);

/// Sets a hardware window.
/// @param win    window 0 or 1
/// @param flags  layers and effects inside the window
/// @param x0     left, pixels
/// @param y0     top, pixels
/// @param x1     right, pixels
/// @param y1     bottom, pixels
void window(uint8_t win, uint16_t flags, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);
///@}

/// @name Sprites (A-8)
///@{

/// Loads a sprite asset into a slot.
/// @param slot   sprite slot
/// @param asset  sprite asset
void sprite_load(uint8_t slot, AssetKey asset);

/// Shows or hides a sprite.
/// @param slot  sprite slot
/// @param on    show it
void sprite_show(uint8_t slot, bool on);

/// Positions a sprite.
/// @param slot  sprite slot
/// @param x     pixels
/// @param y     pixels
void sprite_pos(uint8_t slot, int16_t x, int16_t y);

/// Starts one of the sprite's animations.
/// @param slot  sprite slot
/// @param anim  animation index
void sprite_anim(uint8_t slot, uint16_t anim);

/// Uses the sprite as an object window mask.
/// @param slot  sprite slot
/// @param on    enable
void sprite_obj_window(uint8_t slot, bool on);

/// Sets a sprite's scale and rotation.
/// @param slot      affine sprite slot (< kMaxAffine)
/// @param scale_q8  scale, Q8 (256 = 1.0)
/// @param angle     angle, 16-bit turn (0 = identity)
void sprite_affine(uint8_t slot, uint16_t scale_q8, uint16_t angle);

/// Makes a sprite semi-transparent.
/// @param slot  sprite slot
/// @param on    enable
void sprite_blend(uint8_t slot, bool on);

/// Hides and frees every sequence sprite.
void sprites_clear();
///@}

/// @name Effects
///@{

/// Starts or stops a particle emitter (A-8).
/// @param slot   emitter slot
/// @param on     run it
/// @param asset  emitter asset
void emitter(uint8_t slot, bool on, AssetKey asset);

/// Starts or stops a palette cycle/blend table (A-6, A-9).
/// @param slot   animation slot
/// @param on     run it
/// @param asset  palette animation asset
void palette_anim(uint8_t slot, bool on, AssetKey asset);

/// Starts or stops an HBlank DMA effect on a layer (A-7).
/// @param bg     layer 0-3
/// @param on     run it
/// @param asset  scanline effect asset
void scanline(uint8_t bg, bool on, AssetKey asset);

/// Stops every emitter, palette animation and scanline effect.
void effects_clear();
///@}

/// @name Text (C-1, C-01c)
///@{

/// Opens or closes the text box.
/// @param open  open it
/// @param x     left, cells
/// @param y     top, cells
/// @param w     width, cells
/// @param h     height, cells
void text_box(bool open, uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/// Prints a text asset.
/// @param target  0-3 = BG layer (y wraps with the layer), 4 = text box
/// @param x       anchor column for the alignment
/// @param y       row
/// @param align   TextAlign
/// @param typed   character by character instead of all at once
/// @param style   text style index (game table)
/// @param text    text asset
void text_print(uint8_t target, uint8_t x, uint8_t y, uint8_t align, bool typed, uint8_t style, AssetKey text);

/// Clears rows of a text target.
/// @param target     0-3 = BG layer, 4 = text box
/// @param first_row  first row
/// @param rows       rows to clear; 0 = all
void text_clear(uint8_t target, uint8_t first_row, uint8_t rows);

/// Whether a typed print is still running.
/// @return true while text is being typed
bool text_busy();

/// Clears every text target and closes the text box.
void text_clear_all();
///@}

/// @name View-only map renderer (C-03 chunks)
///@{

/// Shows a map with no player or event logic.
/// @param map  map asset; AssetKey{0} hides it
void map_show(AssetKey map);

/// Positions the map camera.
/// @param x  pixels
/// @param y  pixels
void map_camera(int16_t x, int16_t y);
///@}

/// @name Audio (C-14c)
///@{

/// Starts a song.
/// @param song            song asset; AssetKey{0} stops the music
/// @param fade_in_frames  fade-in length
void song_play(AssetKey song, uint16_t fade_in_frames);

/// Fades the current song out.
/// @param frames  fade length
void song_fade(uint16_t frames);

/// Whether a song is playing.
/// @return true while music plays
bool song_playing();

/// Position in the current song.
/// @return frames since the current song started; 0 when none plays
uint32_t song_frame();

/// Plays a sound effect.
/// @param sound  sound asset
void sfx_play(AssetKey sound);
///@}

} // namespace capsule::seq::port

/// Game hooks the sequence player asks the game.
namespace capsule::seq::game
{

/// A game condition.
/// @param id  1-based, the manifest's 'conditions' order
/// @return whether it holds
bool cond(uint8_t id);

/// Length of a game list.
/// @param list  1-based, the manifest's 'lists' order
/// @return number of items
uint16_t list_count(uint8_t list);

/// One item of a game list.
/// @param list   1-based list id
/// @param index  item, 0 to list_count(list) - 1
/// @return the item's asset
AssetKey list_asset(uint8_t list, uint16_t index);

/// Mixes entropy into the gameplay RNG at the first title press (A-4). The
/// engine default calls rng::seed() with @p frame_counter and
/// hal::system::entropy(); the HAL has already mixed in the first key press
/// of the session, so this adds the title screen's own timing.
/// @param frame_counter  frames since the sequence started
void rng_mix_entropy(uint32_t frame_counter);

} // namespace capsule::seq::game
