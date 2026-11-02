/// @file
/// Scanline effects for scenes (see capsule/screen/scanline.h).
#include "capsule/screen/scanline.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/screen/tasks.h"

namespace capsule::screen::scanline
{
namespace
{

namespace video = hal::video;

constexpr int kTables = 2;              ///< Tables used in turn.
constexpr int kSineScale = 256;         ///< A sample is sine x amplitude / 256.
constexpr uint8_t kWavePriority = 0;    ///< The wave task's priority (runs first).

/// @name The wave task's data words
///@{
constexpr int kDataStart = 0;        ///< First line.
constexpr int kDataEnd = 1;          ///< Line after the last.
constexpr int kDataLength = 2;       ///< Samples before the start sample goes back to the first (256 / frequency).
constexpr int kDataOffset = 3;       ///< The start sample.
constexpr int kDataWait = 4;         ///< Frames left before the start sample moves.
constexpr int kDataDelay = 5;        ///< The wait after each move.
///@}

// The tables are read by the HBlank transfer, a halfword or word a line: EWRAM is fast enough.
CAPSULE_EWRAM_BSS uint32_t g_tables[kTables][kEntries];   ///< The two tables (16-bit effects use their first half).
CAPSULE_EWRAM_BSS uint16_t g_wave[kWaveSamples];          ///< The wave's samples.
uint8_t g_back = 0;                                       ///< The table written for the next frame.
bool g_dirty[kTables] = {};                               ///< A table clear() could not zero yet (it was shown).
bool g_active = false;                                    ///< An effect is set.
video::LineRegister g_reg = video::LineRegister::Bg0Hofs; ///< Its register.
video::LineWidth g_width = video::LineWidth::Half;        ///< Its width.
uint8_t g_wave_task = tasks::kNoTask;                     ///< The wave's task, or kNoTask.
int16_t g_wave_base = 0;                                  ///< The value the wave adds.

/// Zeroes a table.
/// @param t  0 or 1
void zero(int t)
{
   for (uint32_t& e : g_tables[t])
   {
      e = 0;
   }
   g_dirty[t] = false;
}

/// The back table, zeroed first if clear() left it to do (it is no longer shown once it is the back table again).
/// @return its index
int back_table()
{
   if (g_dirty[g_back])
   {
      zero(g_back);
   }
   return g_back;
}

/// A table as 16-bit entries.
/// @param t  0 or 1
/// @return its entries
uint16_t* table16(int t)
{
   return reinterpret_cast<uint16_t*>(g_tables[t]);
}

/// The wave's sample at an index (0 past the samples).
/// @param i  the index
/// @return the sample
uint16_t sample(int i)
{
   return i < kWaveSamples ? g_wave[i] : uint16_t{0};
}

/// Writes the wave into a table's lines from a start sample.
/// @param t       the table
/// @param d       the task's data
/// @param offset  the start sample
void write_wave(uint16_t* t, const int16_t* d, int offset)
{
   for (int line = d[kDataStart]; line < d[kDataEnd]; ++line)
   {
      t[line] = static_cast<uint16_t>(sample(offset++) + g_wave_base);
   }
}

/// The wave's task: writes the back table, moves the start sample every delay + 1 frames.
/// @param id  the task
void wave_task(uint8_t id)
{
   int16_t* d = tasks::task(id).data;
   write_wave(back16(), d, d[kDataOffset]);
   if (d[kDataWait] != 0)
   {
      --d[kDataWait];
      return;
   }
   d[kDataWait] = d[kDataDelay];
   if (++d[kDataOffset] == d[kDataLength])
   {
      d[kDataOffset] = 0;
   }
}

/// Ends the wave's task.
void end_wave()
{
   if (g_wave_task != tasks::kNoTask && tasks::active(g_wave_task) && tasks::task(g_wave_task).func == wave_task)
   {
      tasks::destroy(g_wave_task);
   }
   g_wave_task = tasks::kNoTask;
}

}   // namespace

void start(video::LineRegister reg, video::LineWidth width)
{
   ENGINE_ASSERT(width == video::LineWidth::Half || video::line_word_allowed(reg));
   g_reg = reg;
   g_width = width;
   g_active = true;
}

void clear()
{
   stop();
   // the shown table may still be read while this frame shows: it is zeroed when it next becomes the back table
   g_dirty[g_back ^ 1] = true;
   zero(g_back);
}

void stop()
{
   end_wave();
   g_active = false;
   video::set_line_effect(g_reg, g_width, nullptr);
}

bool active()
{
   return g_active;
}

uint16_t* back16()
{
   return table16(back_table());
}

uint32_t* back32()
{
   return g_tables[back_table()];
}

const uint16_t* shown16()
{
   return table16(g_back ^ 1);
}

void present()
{
   if (!g_active)
   {
      return;
   }
   video::set_line_effect(g_reg, g_width, g_tables[back_table()]);
   g_back = static_cast<uint8_t>(g_back ^ 1);
}

uint8_t start_wave(const Wave& w)
{
   ENGINE_ASSERT(w.sine != nullptr && w.frequency != 0 && w.start_line <= w.end_line && w.end_line <= kEntries);
   end_wave();
   g_wave_base = 0;
   uint8_t theta = 0;
   for (int i = 0; i < kWaveSamples; ++i)
   {
      g_wave[i] = static_cast<uint16_t>((w.sine[theta] * w.amplitude) / kSineScale);
      theta = static_cast<uint8_t>(theta + w.frequency);
   }
   const uint8_t id = tasks::create(wave_task, kWavePriority);
   if (id == tasks::kNoTask)
   {
      return kNoWave;
   }
   int16_t* d = tasks::task(id).data;
   d[kDataStart] = w.start_line;
   d[kDataEnd] = w.end_line;
   d[kDataLength] = static_cast<int16_t>(kWaveSamples / w.frequency);
   d[kDataOffset] = 0;
   d[kDataWait] = w.delay;
   d[kDataDelay] = w.delay;
   g_wave_task = id;
   for (int t = 0; t < kTables; ++t)
   {
      if (g_dirty[t])
      {
         zero(t);   // both tables get the wave at once (the reference games' rule)
      }
      write_wave(table16(t), d, 0);
   }
   return id;
}

void set_wave_base(int16_t base)
{
   g_wave_base = base;
}

}   // namespace capsule::screen::scanline
