/// @file
/// The headless audio backend's capture (D-34 M4 B1b, format §11): every frame's mixed segment (L2), ordered tone
/// writes (L3) and the output model's samples (L4), for tests and apps/audio_render. Only programs that link
/// `src/hal/headless/audio_headless.cpp` (the headless platform, the audio tests, audio_render) may use it.
#pragma once

#include "../common/apu_model.h"

#include <cstdint>

namespace capsule::hal::audio::capture
{

/// Called after each frame_done() with the finished frame (nullptr: none). For a program that writes every frame
/// out as it goes.
using FrameSink = void (*)(const apu::FrameOut& frame);

/// Whether hal::audio::init() ran (the core started audio).
/// @return true once started
bool started();

/// The last finished frame (all zero before the first frame_done()).
/// @return it
const apu::FrameOut& last();

/// The output model (its state for state logs).
/// @return it
const apu::Model& model();

/// Sets the per-frame sink.
/// @param sink  the function (nullptr: none)
void set_sink(FrameSink sink);

/// Runs one vertical blank the way system::update() does once audio is started: core::vsync(), core::vblank(),
/// hal::audio::frame_done(). For programs that drive the core without the system HAL (apps/audio_render).
void step();

} // namespace capsule::hal::audio::capture
