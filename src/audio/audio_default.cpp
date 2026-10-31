/// @file
/// The engine's default audio table: empty (no players, no sounds, no clips; the mixer at 13,379 Hz with 5
/// channels, master volume 12, mono). A build with audio links its generated table (tools/audio_compiler.py)
/// instead, which keeps this file out of the link.
#include "capsule/audio/rom.h"

namespace capsule::audio::rom
{
namespace
{

inline constexpr uint8_t kRate13379 = 3;     ///< 224 samples a frame.
inline constexpr uint8_t kChannels = 5;      ///< The reference driver's sampled channels.
inline constexpr uint8_t kMasterVolume = 12; ///< Its master volume.
inline constexpr uint8_t kDacBits = 8;       ///< 8-bit DAC.

}   // namespace

const Table kTable = {nullptr, 0, nullptr, 0, kRate13379, kChannels, kMasterVolume, kDacBits, 0, nullptr, 0, nullptr};

}   // namespace capsule::audio::rom
