/// @file
/// A test backend for the audio core (D-34 M4 B1a): implements capsule/hal/audio.h by recording what the core
/// writes, so the core's tests need no real backend. Tone registers read back the last byte written; wave RAM writes
/// are recorded as 16 byte writes at offsets 0x30-0x3F (wave RAM's place after the tone registers).
#pragma once

#include <cstdint>

namespace capsule::audio::test_hal
{

/// One recorded register write.
struct Write
{
   uint8_t reg;     ///< hal::audio::ToneReg value, or 0x30 + byte for wave RAM.
   uint8_t value;   ///< The byte.
};

/// Wave RAM bytes appear at this offset in the record.
inline constexpr uint8_t kWaveRecordBase = 0x30;
/// Writes kept between clear() calls.
inline constexpr uint16_t kMaxWrites = 1024;

/// The writes since the last clear().
/// @return the first one
const Write* writes();

/// Their number (writes past kMaxWrites are counted, not kept).
/// @return the count
uint16_t write_count();

/// Forgets the writes.
void clear();

/// restart_dma() calls since init().
/// @return the count
uint32_t dma_restarts();

/// The last set_output().
/// @return true for stereo
bool stereo();

/// The last high-quality block (CAPSULE_AUDIO_HQ), right side then left.
/// @param left  false: right side
/// @return its first sample (nullptr before the first)
const int16_t* hq(bool left);

}   // namespace capsule::audio::test_hal
