/// @file
/// The scene kit's sandbox scenes (BF, D-38; simple shapes), shared by the sandbox app (sandbox_anim.cpp, every
/// target) and the viewer (anim_view.cpp, the software video model): they run frame by frame with no input, so the
/// GBA and the software renderer show the same pictures on the same frames.
///
/// Scenes, kSceneFrames frames each, in turn: 0 affine (a spinning, a pulsing 2x-box and a squashing ball, a
/// box turned by its matrix every frame), 1 scanline (a wave on BG0's x scroll, then per-line blend weights, then
/// window 0's edges per line: a diamond), 2 blend (three balls' palette tags blended and ramped, then a fade by tag),
/// 3 script (the animation player runs data/shapes.canim: graphics, sprites, an orbit task, a flash task, a blend,
/// a layer scroll, the waits).
#pragma once

#include <cstdint>

/// The sandbox's scenes.
namespace sandbox_anim
{

inline constexpr int kScenes = 4;          ///< Scenes.
inline constexpr int kSceneFrames = 180;   ///< Frames each scene runs.
inline constexpr int kTotalFrames = kScenes * kSceneFrames;   ///< One round of every scene.

/// What a round reported (the viewer's --check).
struct Report
{
   uint16_t scripts_ended;   ///< Times the script ended with End.
   uint16_t player_errors;   ///< Times the player stopped on an error.
   uint16_t scanline_frames; ///< Frames a scanline table was set.
};

/// One frame of the showcase (frame f of every round: scene f / kSceneFrames, its frame f % kSceneFrames): the
/// scene's setup on its first frame, its logic, the work that lands with the frame (the scanline table).
/// @param f  the frame, from 0
void frame(uint32_t f);

/// What the rounds so far reported.
/// @return the report
const Report& report();

}   // namespace sandbox_anim
