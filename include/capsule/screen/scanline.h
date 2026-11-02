/// @file
/// Scanline effects for scenes (BF, D-38): one display register takes a value per scanline (hal::video::
/// set_line_effect()) from two tables used in turn, as the reference games' scanline effect does, so a scene writes
/// the next frame's values while the current frame shows the other table.
///
/// A scene:
/// - start() picks the register and the width, clear() zeroes both tables;
/// - writes the next frame's values into back16() / back32() (its tasks, its main function, a wave: start_wave());
/// - calls present() once a frame in its vertical blank function (the reference games' InitHBlankDmaTransfer): the
///   back table shows from the frame being finished, and the other one becomes the back table;
/// - stop() turns the effect off from that frame and ends the wave.
///
/// The wave (start_wave(), the reference games' wave task): a task of priority 0 that writes, once a frame, the
/// samples (sine[theta] x amplitude) / 256 (theta stepping by the frequency, 256 samples, the game's 256-entry 8.8
/// sine table; a read past the samples gives 0) plus the game's base value to the lines start to end - 1 of the back
/// table, starting a sample later every delay + 1 frames (back to the first after 256 / frequency).
#pragma once

#include "capsule/hal/video.h"

#include <cstdint>

/// Scanline effects.
namespace capsule::screen::scanline
{

inline constexpr int kEntries = hal::video::kLineEntries;   ///< Entries of a table.
inline constexpr int kWaveSamples = 256;                     ///< Samples of a wave.
inline constexpr uint8_t kNoWave = 0xFF;                     ///< start_wave() when no task was free.

/// Picks the register and width (the effect shows from the next present()).
/// @param reg    the register
/// @param width  the entries' width
void start(hal::video::LineRegister reg, hal::video::LineWidth width);

/// Zeroes both tables and stops the effect and its wave (the reference games' ScanlineEffect_Clear); the table the
/// frame being shown reads is zeroed when it next becomes the back table (back16(), present(), start_wave()).
void clear();

/// Stops the effect from the frame being finished (latched) and ends its wave.
void stop();

/// Whether an effect is set.
/// @return true between start() and stop()
bool active();

/// The table the next frame shows, 16-bit entries.
/// @return kEntries entries
uint16_t* back16();

/// The table the next frame shows, 32-bit entries.
/// @return kEntries entries
uint32_t* back32();

/// The table the frame being shown uses (read only by the scene: the transfer reads it).
/// @return kEntries 16-bit entries (as back16())
const uint16_t* shown16();

/// Shows the back table from the frame being finished and makes the other one the back table (once a frame, in
/// the vertical blank function; nothing while no effect is set).
void present();

/// A wave's settings (the reference games' ScanlineEffect_InitWave arguments).
struct Wave
{
   const int16_t* sine;     ///< The game's sine table: 256 entries, 8.8 (256 = 1.0).
   uint8_t start_line;      ///< First line written.
   uint8_t end_line;        ///< Line after the last.
   uint8_t frequency;       ///< Theta step per sample (1 or more).
   uint8_t amplitude;       ///< Sample scale.
   uint8_t delay;           ///< Frames each start sample is kept less one.
};

/// Starts a wave on a 16-bit effect: the samples made, written into both tables, and the wave's task created.
/// @param w  the settings
/// @return the task, or kNoWave
uint8_t start_wave(const Wave& w);

/// Sets the value the wave adds to every sample (e.g. the layer's scroll; 0 at start_wave()).
/// @param base  the value
void set_wave_base(int16_t base);

}   // namespace capsule::screen::scanline
