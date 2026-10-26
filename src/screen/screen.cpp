/// @file
/// Full-screen scenes (see capsule/screen/screen.h).
#include "capsule/screen/screen.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/screen/arena.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"

namespace capsule::screen
{
namespace
{

namespace video = hal::video;

/// One open screen.
struct Entry
{
   const Screen* s;     ///< The screen.
   uint8_t arg;         ///< Its setup's argument.
   uint8_t state;       ///< Its setup's state.
   Phase phase;         ///< Where it is.
   bool resumed;        ///< Its setup runs again (the screen above it closed).
   uint32_t mark;       ///< UM: arena::used() at its first setup call (its blocks start here; given back on its pop).
};

Entry g_stack[kMaxDepth];             ///< The open screens, bottom first.
int g_depth = 0;                      ///< Entries of g_stack.
bool g_field_returning = false;       ///< The last screen left: the field resumes.
uint8_t g_field_state = 0;            ///< Host::resume's state.
const Host* g_host = nullptr;         ///< The field's host (nullptr: default_host()).
const Screen* g_replacement = nullptr;   ///< replace(): opens when the top screen has left.
uint8_t g_replacement_arg = 0;        ///< Its argument.
ui::Keys g_keys{};                    ///< The frame's keys.
uint32_t g_frames = 0;                ///< frame() calls since the stack was empty.
bool g_in_frame = false;              ///< C13a: frame() is running (close_now() leaves at its end).
bool g_leave_pending = false;         ///< C13a: close_now() was called in this frame: the top leaves at its end.
constexpr int kBelowTop = 2;          ///< Entries from the top to the screen below it, plus one.
CAPSULE_EWRAM_BSS gfx::palette::Snapshot g_field_colors;   ///< The field's base colours (default host).

/// The host in use.
/// @return it
const Host& host()
{
   return g_host != nullptr ? *g_host : default_host();
}

/// The screen's steps in order.
/// @param s           the screen
/// @param[out] count  entries
/// @return the steps
const Step* steps_of(const Screen& s, uint8_t& count)
{
   if (s.steps == nullptr)
   {
      count = kDefaultStepCount;
      return kDefaultSteps;
   }
   ENGINE_ASSERT(s.step_count <= kMaxSteps);
   count = s.step_count;
   return s.steps;
}

/// Runs a screen's steps (its second callback).
/// @param s  the screen
void run_steps(const Screen& s)
{
   uint8_t count = 0;
   const Step* steps = steps_of(s, count);
   for (uint8_t i = 0; i < count; ++i)
   {
      switch (steps[i])
      {
      case Step::Tasks:
         tasks::run();
         break;
      case Step::Animate:
         sprites::animate();
         break;
      case Step::BuildOam:
         sprites::build_oam();
         break;
      case Step::CopyTilemaps:
         layers::copy_scheduled();
         break;
      case Step::Fade:
         fade::update();
         break;
      case Step::Main:
         if (s.main != nullptr)
         {
            s.main();
         }
         break;
      default:
         break;
      }
   }
   if (s.vblank != nullptr)
   {
      s.vblank();
   }
}

/// Starts a transition's fade.
/// @param t  the transition
void start(const Transition& t)
{
   if (t.on)
   {
      fade::cancel();   // a transition takes over whatever fade ran
      fade::begin(t.fade, t.style);
   }
}

/// What is below the top screen runs its frame (an open fade over it).
void run_below()
{
   if (g_depth >= kBelowTop)
   {
      run_steps(*g_stack[g_depth - kBelowTop].s);   // its steps call the fade
      return;
   }
   if (host().frame != nullptr)
   {
      host().frame();
   }
   fade::update();
}

/// What is below the top screen gives up its video memory.
void give_up_below()
{
   if (g_depth >= kBelowTop)
   {
      const Screen& below = *g_stack[g_depth - kBelowTop].s;
      if (below.leave != nullptr)
      {
         below.leave(true);
      }
      return;
   }
   if (host().suspend != nullptr)
   {
      host().suspend();
   }
}

/// One setup call of the top screen (its plan applied before the first); its in fade when it is done.
/// @param e  the entry
void run_setup(Entry& e)
{
   const Screen& s = *e.s;
   if (e.state == 0 && (e.phase == Phase::Handover || e.phase == Phase::Left))
   {
      // its first setup call: what is below gave up its video memory (and, with it, any arena blocks it releases in
      // its leave(true)), so the arena's top is where this screen's blocks start
      e.mark = arena::used();
      if (s.plan != nullptr)
      {
         layers::apply(*s.plan);
      }
   }
   if (e.phase != Phase::Resuming)
   {
      e.phase = Phase::Setup;
   }
   if (s.setup == nullptr || s.setup(e.state, e.arg, e.resumed))
   {
      start(s.in);
      e.phase = Phase::Running;
      e.resumed = false;
   }
}

/// Opens a screen on the stack.
/// @param s       the screen
/// @param arg     its argument
/// @param opening run its open fade
/// @return false when the stack is full
bool open_entry(const Screen& s, uint8_t arg, bool opening)
{
   if (g_depth >= kMaxDepth)
   {
      return false;
   }
   if (g_depth == 0 && !g_field_returning)
   {
      g_frames = 0;
   }
   Entry& e = g_stack[g_depth++];
   e = Entry{&s, arg, 0, Phase::Handover, false, arena::used()};   // the mark again at its first setup call
   if (opening && s.open.on)
   {
      e.phase = Phase::Opening;
      start(s.open);
   }
   return true;
}

/// The top screen leaves (Screen::leave(false)); what is below (or a replacement) takes over the next frame.
void leave_top()
{
   Entry& e = g_stack[g_depth - 1];
   if (e.s->leave != nullptr)
   {
      e.s->leave(false);
   }
   e.phase = Phase::Left;
}

/// The top screen has left: it is popped, and the screen below (or a replacement, or the field) takes over this
/// frame.
void pop_top()
{
   --g_depth;
   arena::release_to(g_stack[g_depth].mark);   // UM: the screen left for good: its arena blocks go back
   if (g_replacement != nullptr)
   {
      const Screen* next = g_replacement;
      g_replacement = nullptr;
      Entry& e = g_stack[g_depth++];
      e = Entry{next, g_replacement_arg, 0, Phase::Left, false, arena::used()};   // what is below stays given up
      run_setup(e);
      return;
   }
   if (g_depth == 0)
   {
      g_field_returning = true;
      g_field_state = 0;
      if (host().resume == nullptr || host().resume(g_field_state))
      {
         g_field_returning = false;
      }
      return;
   }
   Entry& below = g_stack[g_depth - 1];
   below.phase = Phase::Resuming;
   below.state = 0;
   below.resumed = true;
   if (below.s->plan != nullptr)
   {
      layers::apply(*below.s->plan);
   }
   run_setup(below);
}

/// The default host's suspend.
void default_suspend()
{
   gfx::palette::save(g_field_colors);
   map::suspend();
}

/// The default host's resume: the reference games' four return steps, one a frame.
/// @param state  the step
/// @return true when the field is back
bool default_resume(uint8_t& state)
{
   constexpr uint8_t kObjects = 0;    ///< The game's objects and scripts.
   constexpr uint8_t kGraphics = 1;   ///< The view's graphics.
   constexpr uint8_t kCallback = 2;   ///< The game's field callback.
   const Host& h = host();
   switch (state)
   {
   case kObjects:
      if (h.on_return != nullptr)
      {
         h.on_return();
      }
      ++state;
      return false;
   case kGraphics:
      restore_field_graphics();
      ++state;
      return false;
   case kCallback:
      if (h.field_callback == nullptr || h.field_callback())
      {
         ++state;
      }
      return false;
   default:
      return true;
   }
}

constexpr Host kDefaultHost = {default_suspend, default_resume, nullptr, nullptr, nullptr};   ///< default_host().

}   // namespace

const Host& default_host()
{
   return kDefaultHost;
}

void set_host(const Host& h)
{
   g_host = &h;
   hal::vram::set_tile_guard(layers::tiles_clear_of_maps);   // C13a (R9 M4): from the field's first frame on
}

void restore_field_graphics()
{
   layers::reset();
   sprites::reset();
   tasks::reset();
   map::resume(true);
   gfx::sprites::reupload();
   gfx::palette::restore(g_field_colors);
   video::set_blend(video::BlendMode::Off, 0, 0);
   video::set_windows(false, false, false);
   video::set_mosaic(0);
}

bool push(const Screen& s, uint8_t arg)
{
   return open_entry(s, arg, true);
}

bool push_now(const Screen& s, uint8_t arg)
{
   return open_entry(s, arg, false);
}

void close()
{
   if (g_depth == 0)
   {
      return;
   }
   Entry& e = g_stack[g_depth - 1];
   e.phase = Phase::Closing;
   start(e.s->out);
}

void close_now()
{
   if (g_depth == 0)
   {
      return;
   }
   Entry& e = g_stack[g_depth - 1];
   e.phase = Phase::Closing;
   fade::cancel();
   // C13a (R9 H1): the frame close_now() is called in is the reference games' SetMainCallback2 frame: the screen
   // leaves at its end (its remaining steps still run, as the rest of that frame's callback does) and what comes
   // next runs from the next frame, as after close()'s fade (the task that sees the fade end leaves that frame)
   if (g_in_frame)
   {
      g_leave_pending = true;
      return;
   }
   leave_top();   // called between frames: the frame that called it has ended
}

void replace(const Screen& s, uint8_t arg)
{
   g_replacement = &s;
   g_replacement_arg = arg;
   close();
}

void replace_now(const Screen& s, uint8_t arg)
{
   g_replacement = &s;
   g_replacement_arg = arg;
   close_now();
}

namespace
{

/// One frame of the stack (frame() without the end-of-frame leave).
void frame_body()
{
   if (g_depth == 0)
   {
      if (g_field_returning && (host().resume == nullptr || host().resume(g_field_state)))
      {
         g_field_returning = false;
      }
      return;
   }
   Entry& e = g_stack[g_depth - 1];
   switch (e.phase)
   {
   case Phase::Opening:
      if (!fade::active())
      {
         e.phase = Phase::Handover;   // the fade ended last frame: what is below runs this frame, then gives up
      }
      run_below();
      break;
   case Phase::Handover:
      give_up_below();
      run_setup(e);
      break;
   case Phase::Setup:
   case Phase::Resuming:
      run_setup(e);
      break;
   case Phase::Running:
      if (e.s->input != nullptr)
      {
         e.s->input();
      }
      if (e.phase == Phase::Running || e.phase == Phase::Closing)
      {
         run_steps(*e.s);
      }
      break;
   case Phase::Closing:
      if (!fade::active() && !g_leave_pending)
      {
         leave_top();   // what is below resumes next frame
         break;
      }
      run_steps(*e.s);
      break;
   case Phase::Left:
      pop_top();
      break;
   default:
      break;
   }
}

}   // namespace

void frame(const ui::Keys& keys)
{
   g_keys = keys;
   ++g_frames;
   g_in_frame = true;
   frame_body();
   g_in_frame = false;
   if (g_leave_pending)
   {
      g_leave_pending = false;
      if (g_depth > 0 && g_stack[g_depth - 1].phase == Phase::Closing)
      {
         leave_top();   // close_now() in this frame: the frame's steps ran, the screen leaves at its end
      }
   }
}

bool active()
{
   return g_depth > 0 || g_field_returning;
}

int depth()
{
   return g_depth;
}

const Screen* top()
{
   return g_depth > 0 ? g_stack[g_depth - 1].s : nullptr;
}

Phase phase()
{
   return g_depth > 0 ? g_stack[g_depth - 1].phase : Phase::NUM_PHASES;
}

const ui::Keys& keys()
{
   return g_keys;
}

uint32_t frames()
{
   return g_frames;
}

void reset()
{
   g_depth = 0;
   g_leave_pending = false;
   g_field_returning = false;
   g_field_state = 0;
   g_replacement = nullptr;
   g_frames = 0;
   fade::cancel();
   arena::reset();   // UM: every block, the field's too (tests)
}

}   // namespace capsule::screen
