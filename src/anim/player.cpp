/// @file
/// The animation-script player shell (see capsule/anim/player.h).
#include "capsule/anim/player.h"

#include "capsule/core/assert.h"
#include "capsule/core/endian.h"
#include "capsule/hal/video.h"
#include "capsule/screen/layers.h"

namespace capsule::anim
{
namespace
{

namespace sprites = screen::sprites;
namespace tasks = screen::tasks;
namespace video = hal::video;

constexpr int16_t kWaitAsOne = -1;   ///< Delay 0 waits as this (the wait mode's shortest step).

/// Whether the next n operand bytes are inside the code; stops the player when not.
/// @param p  the player
/// @param n  bytes
/// @return true when they are
bool have(Player& p, uint32_t n)
{
   if (!p.active || p.pc > p.bank->code_size || n > p.bank->code_size - p.pc)
   {
      fail(p, PlayerError::BadOperand);
      return false;
   }
   return true;
}

/// The game's hooks (an empty set when it gave none).
/// @param p  the player
/// @return the hooks
const Hooks& hooks_of(const Player& p)
{
   static constexpr Hooks kNone = {};
   return p.game->hooks != nullptr ? *p.game->hooks : kNone;
}

/// The size of a kind's table.
/// @param t     the tables
/// @param kind  the kind
/// @return entries
uint16_t table_size(const Tables& t, SymbolKind kind)
{
   switch (kind)
   {
   case SymbolKind::Template:
      return t.template_count;
   case SymbolKind::Task:
      return t.task_count;
   case SymbolKind::SoundTask:
      return t.sound_task_count;
   case SymbolKind::Callback:
      return t.callback_count;
   case SymbolKind::Data:
      return t.data_count;
   case SymbolKind::NUM_KINDS:
      break;
   }
   return 0;
}

/// Two strings equal.
/// @param a  one
/// @param b  the other
/// @return true when equal
bool same(const char* a, const char* b)
{
   while (*a != '\0' && *a == *b)
   {
      ++a;
      ++b;
   }
   return *a == *b;
}

/// Creates a task for a script and runs it once (CreateTask / CreateSoundTask).
/// @param p      the player
/// @param kind   SymbolKind::Task or SoundTask
/// @param count  the count it adds to
void create_task_of(Player& p, SymbolKind kind, uint8_t& count)
{
   const uint16_t index = read_symbol(p, kind);
   const uint8_t priority = read_u8(p);
   read_args(p);
   if (!p.active)
   {
      return;
   }
   const Tables& t = *p.game->tables;
   const tasks::Func func = kind == SymbolKind::Task ? t.tasks[index] : t.sound_tasks[index];
   const uint8_t id = tasks::create(func, priority);
   if (id == tasks::kNoTask)
   {
      return;
   }
   func(id);
   ++count;
}

}   // namespace

// --- reading -------------------------------------------------------------------------------------------------

uint8_t read_u8(Player& p)
{
   if (!have(p, 1))
   {
      return 0;
   }
   return p.bank->code[p.pc++];
}

int8_t read_s8(Player& p)
{
   return static_cast<int8_t>(read_u8(p));
}

uint16_t read_u16(Player& p)
{
   if (!have(p, sizeof(uint16_t)))
   {
      return 0;
   }
   const uint16_t v = load_le16(p.bank->code + p.pc);
   p.pc += sizeof(uint16_t);
   return v;
}

int16_t read_s16(Player& p)
{
   return static_cast<int16_t>(read_u16(p));
}

uint32_t read_u32(Player& p)
{
   if (!have(p, sizeof(uint32_t)))
   {
      return 0;
   }
   const uint32_t v = load_le32(p.bank->code + p.pc);
   p.pc += sizeof(uint32_t);
   return v;
}

uint32_t read_address(Player& p)
{
   const uint32_t a = read_u32(p);
   if (p.active && a >= p.bank->code_size)
   {
      fail(p, PlayerError::BadOperand);
      return 0;
   }
   return a;
}

uint16_t read_symbol(Player& p, SymbolKind kind)
{
   const uint16_t sym = read_u16(p);
   if (!p.active)
   {
      return 0;
   }
   if (sym >= p.bank->symbol_count || symbol_kind(*p.bank, sym) != kind)
   {
      fail(p, PlayerError::BadOperand);
      return 0;
   }
   const uint16_t index = p.map != nullptr ? p.map[sym] : symbol_index(*p.bank, sym);
   if (index >= table_size(*p.game->tables, kind))
   {
      fail(p, PlayerError::BadOperand);
      return 0;
   }
   return index;
}

uint8_t read_args(Player& p)
{
   const uint8_t n = read_u8(p);
   if (p.active && n > kMaxArgs)
   {
      fail(p, PlayerError::BadOperand);
      return 0;
   }
   for (uint8_t i = 0; i < n && p.active; ++i)
   {
      p.args[i] = read_s16(p);
   }
   return n;
}

// --- control -------------------------------------------------------------------------------------------------

void jump(Player& p, uint32_t address)
{
   if (address >= p.bank->code_size)
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   p.pc = address;
}

void wait_frames(Player& p, int16_t n)
{
   p.frames_to_wait = n != 0 ? n : kWaitAsOne;
   p.waiting = true;
}

void yield(Player& p)
{
   p.frames_to_wait = 1;
}

void fail(Player& p, PlayerError e)
{
   p.error = e;
   p.active = false;
}

uint16_t bind(const Bank& b, const Tables& t, uint16_t* map)
{
   uint16_t missing = 0;
   for (uint16_t i = 0; i < b.symbol_count; ++i)
   {
      const SymbolKind kind = symbol_kind(b, i);
      const char* name = symbol_name(b, i);
      const char* const* names = t.names[static_cast<int>(kind)];
      const uint16_t n = table_size(t, kind);
      map[i] = kUnbound;
      for (uint16_t j = 0; names != nullptr && j < n; ++j)
      {
         if (names[j] != nullptr && same(names[j], name))
         {
            map[i] = j;
            break;
         }
      }
      if (map[i] == kUnbound)
      {
         ++missing;
      }
   }
   return missing;
}

void start(Player& p, const Bank& b, const Game& g, uint16_t script, const uint16_t* map)
{
   p = Player{};
   p.bank = &b;
   p.game = &g;
   p.map = map;
   if (script >= b.script_count)
   {
      p.error = PlayerError::BadScript;
      return;
   }
   p.pc = script_address(b, script);
   p.command_pc = p.pc;
   p.active = true;
}

void stop(Player& p)
{
   p.active = false;
}

bool running(const Player& p)
{
   return p.active;
}

void frame(Player& p)
{
   if (!p.active)
   {
      return;
   }
   ++p.frames;
   if (p.waiting)
   {
      // wait mode: the frame the count is used up only leaves it; commands run again the next frame
      if (p.frames_to_wait <= 0)
      {
         p.waiting = false;
         p.frames_to_wait = 0;
      }
      else
      {
         --p.frames_to_wait;
      }
      return;
   }
   int run = 0;
   do
   {
      if (++run > kMaxCommandsPerFrame)
      {
         fail(p, PlayerError::Runaway);
         return;
      }
      if (p.pc >= p.bank->code_size)
      {
         fail(p, PlayerError::BadOperand);
         return;
      }
      p.command_pc = p.pc;
      const Command c = p.game->commands[p.bank->code[p.pc]];
      if (c == nullptr)
      {
         fail(p, PlayerError::BadOpcode);
         return;
      }
      ++p.commands;
      c(p);
   }
   while (p.frames_to_wait == 0 && p.active);
}

// --- generic commands ----------------------------------------------------------------------------------------

void cmd_end(Player& p)
{
   if (p.visual_count != 0 || p.sound_count != 0)
   {
      yield(p);
      return;
   }
   const Hooks& h = hooks_of(p);
   if (h.finish != nullptr && !h.finish(p))
   {
      yield(p);
      return;
   }
   ++p.pc;
   p.active = false;
}

void cmd_delay(Player& p)
{
   ++p.pc;
   const int16_t n = read_u8(p);
   if (p.active)
   {
      wait_frames(p, n);
   }
}

void cmd_wait_visual(Player& p)
{
   if (p.visual_count != 0)
   {
      yield(p);
      return;
   }
   ++p.pc;
   p.frames_to_wait = 0;
}

void cmd_wait_sound(Player& p)
{
   const Hooks& h = hooks_of(p);
   if (p.sound_count != 0 || (h.sound_busy != nullptr && h.sound_busy(p)))
   {
      yield(p);
      return;
   }
   ++p.pc;
   p.frames_to_wait = 0;
}

void cmd_jump(Player& p)
{
   ++p.pc;
   const uint32_t a = read_address(p);
   if (p.active)
   {
      jump(p, a);
   }
}

void cmd_call(Player& p)
{
   ++p.pc;
   const uint32_t a = read_address(p);
   if (!p.active)
   {
      return;
   }
   if (p.depth >= kCallDepth)
   {
      fail(p, PlayerError::CallDepth);
      return;
   }
   p.stack[p.depth++] = p.pc;
   jump(p, a);
}

void cmd_return(Player& p)
{
   if (p.depth == 0)
   {
      fail(p, PlayerError::CallDepth);
      return;
   }
   p.pc = p.stack[--p.depth];
}

void cmd_set_arg(Player& p)
{
   ++p.pc;
   const uint8_t n = read_u8(p);
   const int16_t v = read_s16(p);
   if (!p.active)
   {
      return;
   }
   if (n >= kMaxArgs)
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   p.args[n] = v;
}

void cmd_jump_if_arg_eq(Player& p)
{
   ++p.pc;
   const uint8_t n = read_u8(p);
   const int16_t v = read_s16(p);
   const uint32_t a = read_address(p);
   if (!p.active)
   {
      return;
   }
   if (n >= kMaxArgs)
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   if (p.args[n] == v)
   {
      jump(p, a);
   }
}

void cmd_create_sprite(Player& p)
{
   ++p.pc;
   const uint16_t index = read_symbol(p, SymbolKind::Template);
   const uint8_t flags = read_u8(p);
   read_args(p);
   if (!p.active)
   {
      return;
   }
   const SpriteTemplate& t = p.game->tables->templates[index];
   const Hooks& h = hooks_of(p);
   const uint8_t id = h.create_sprite != nullptr ? h.create_sprite(p, t, flags) : create_sprite(t, 0, 0, flags);
   if (id != sprites::kNoSprite)
   {
      ++p.visual_count;
   }
}

void cmd_create_task(Player& p)
{
   ++p.pc;
   create_task_of(p, SymbolKind::Task, p.visual_count);
}

void cmd_create_sound_task(Player& p)
{
   ++p.pc;
   create_task_of(p, SymbolKind::SoundTask, p.sound_count);
}

void cmd_load_gfx(Player& p)
{
   ++p.pc;
   const uint16_t tag = read_u16(p);
   if (!p.active)
   {
      return;
   }
   const Hooks& h = hooks_of(p);
   if (h.load_gfx != nullptr)
   {
      h.load_gfx(p, tag);
   }
   wait_frames(p, 1);
}

void cmd_unload_gfx(Player& p)
{
   ++p.pc;
   const uint16_t tag = read_u16(p);
   const Hooks& h = hooks_of(p);
   if (p.active && h.unload_gfx != nullptr)
   {
      h.unload_gfx(p, tag);
   }
}

void cmd_play_se(Player& p)
{
   ++p.pc;
   const uint16_t se = read_u16(p);
   const int8_t pan = read_s8(p);
   const Hooks& h = hooks_of(p);
   if (p.active && h.play_se != nullptr)
   {
      h.play_se(p, se, pan);
   }
}

void cmd_set_blend(Player& p)
{
   ++p.pc;
   const uint8_t mode = read_u8(p);
   const uint8_t first = read_u8(p);
   const uint8_t second = read_u8(p);
   const uint8_t eva = read_u8(p);
   const uint8_t evb = read_u8(p);
   if (!p.active)
   {
      return;
   }
   if (mode >= static_cast<uint8_t>(video::BlendMode::NUM_BLEND_MODES))
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   video::set_blend(static_cast<video::BlendMode>(mode), first, second);
   video::set_blend_alpha(eva, evb);
}

void cmd_blend_off(Player& p)
{
   ++p.pc;
   video::set_blend(video::BlendMode::Off, 0, 0);
   video::set_blend_alpha(0, 0);
}

void cmd_bg_show(Player& p)
{
   ++p.pc;
   const uint8_t bg = read_u8(p);
   const uint8_t on = read_u8(p);
   if (!p.active)
   {
      return;
   }
   if (bg >= video::kNumBgs)
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   screen::layers::show(bg, on != 0);
}

void cmd_bg_scroll(Player& p)
{
   ++p.pc;
   const uint8_t bg = read_u8(p);
   const int16_t x = read_s16(p);
   const int16_t y = read_s16(p);
   if (!p.active)
   {
      return;
   }
   if (bg >= video::kNumBgs)
   {
      fail(p, PlayerError::BadOperand);
      return;
   }
   screen::layers::set_scroll(bg, x, y);
}

void cmd_nop(Player& p)
{
   ++p.pc;
}

// --- sprites and tasks ---------------------------------------------------------------------------------------

uint8_t create_sprite(const SpriteTemplate& t, int16_t x, int16_t y, uint8_t subpriority)
{
   ENGINE_ASSERT(t.sprite != nullptr);
   if (t.sprite->affine != sprites::kAffineOff)
   {
      return sprites::create_and_animate(*t.sprite, x, y, subpriority, screen::affine::prepare, t.affine_anims);
   }
   return sprites::create_and_animate(*t.sprite, x, y, subpriority);
}

void finish_sprite(Player& p, uint8_t id)
{
   screen::affine::detach(id);
   sprites::destroy(id);
   if (p.visual_count != 0)
   {
      --p.visual_count;
   }
}

void finish_task(Player& p, uint8_t id)
{
   tasks::destroy(id);
   if (p.visual_count != 0)
   {
      --p.visual_count;
   }
}

void finish_sound_task(Player& p, uint8_t id)
{
   tasks::destroy(id);
   if (p.sound_count != 0)
   {
      --p.sound_count;
   }
}

}   // namespace capsule::anim
