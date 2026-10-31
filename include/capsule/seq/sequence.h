/// @file
/// Sequence player (P-3, P-13): one data-driven player for every scripted,
/// non-interactive presentation: boot/title flows, non-overworld cutscenes and
/// credits.
///
/// A sequence is ROM data compiled by tools/sequence_compiler.py from a
/// capsule.sequence/1 manifest (P-1); nothing is parsed at run time.
///
/// **Model.** Sequence = stages. Stage = up to kMaxTracks command tracks run in
/// parallel, plus exit rules. A track runs commands until it blocks (Wait /
/// WaitFor). Motion is set and forget: scroll, sprite velocity and every ramp
/// (screen fade, palette fade, blend weights, window rect, sprite
/// scale/rotation) is started by one command and advanced by the player each
/// frame.
///
/// **Stage exits**, each a Target (another stage, or exit with a code): done
/// (all tracks ended) | skip (newly pressed key) | timeout (frames) | song_end |
/// combo (held/pressed keys + optional condition) | Goto command. Input is
/// ignored on a stage's first frame, so its setup always runs.
///
/// **Game hooks** (sequence_port.h, namespace game): conditions (booleans such
/// as the player's appearance choice), lists (asset lists such as a showcase of
/// caught monsters) and exit codes are declared by name in the manifest and
/// answered by the game.
///
/// **Fast-forward:** while a stage's ff keys are held (and its ff condition
/// holds), the stage advances ff_speed logical frames per frame.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"
#include "capsule/hal/input.h"

#include <cstdint>

#ifndef CAPSULE_SEQ_MAX_TRACKS
/// Build-manifest limit (P-12): command tracks per stage. Pass the same value
/// to the compiler; the generated tables static_assert what they use.
#define CAPSULE_SEQ_MAX_TRACKS 8
#endif
#ifndef CAPSULE_SEQ_MAX_SPRITES
/// Build-manifest limit (P-12): sprite slots a sequence may use.
#define CAPSULE_SEQ_MAX_SPRITES 16
#endif
#ifndef CAPSULE_SEQ_MAX_AFFINE
/// Build-manifest limit (P-12): sprite slots 0..N-1 may use scale/rotation ramps.
#define CAPSULE_SEQ_MAX_AFFINE 4
#endif

/// The sequence player: title flows, cutscenes and credits.
namespace capsule::seq
{

using hal::input::KeyBits;

inline constexpr int kMaxTracks = CAPSULE_SEQ_MAX_TRACKS;    ///< Tracks per stage.
inline constexpr int kMaxSprites = CAPSULE_SEQ_MAX_SPRITES;   ///< Sprite slots.
inline constexpr int kMaxAffine = CAPSULE_SEQ_MAX_AFFINE;    ///< Slots with scale/rotation.
inline constexpr int kMaxBgs = 4;                         ///< Background layers.
inline constexpr int kMaxWindows = 2;                         ///< Hardware windows.
inline constexpr int kRectEdges = 4;                         ///< Edges of a window rect: x0, y0, x1, y1.
inline constexpr int kRectX0 = 0;                         ///< Window rect: left edge.
inline constexpr int kRectY0 = 1;                         ///< Window rect: top edge.
inline constexpr int kRectX1 = 2;                         ///< Window rect: right edge.
inline constexpr int kRectY1 = 3;                         ///< Window rect: bottom edge.
inline constexpr uint8_t kLevelFull = 16;                 ///< Full brightness / blend weight / palette coefficient.
inline constexpr int kMaxLoopDepth = 2;                         ///< Nested Repeat / ForEach.
inline constexpr int kMaxCmdsPerFrame = 64;            ///< Guard; the compiler rejects zero-time loops.
inline constexpr uint32_t kItemAsset =
   0xFFFFFFFFu;   ///< Asset operand meaning "current item of the innermost ForEach".
static_assert(kMaxAffine <= kMaxSprites);

/// @name Targets
/// Where a stage exit or Goto leads: 0x00-0x7F = stage index, 0x80 | code =
/// exit with code (0-126), 0xFF = none.
///@{
using Target = uint8_t;                       ///< Stage index, exit or none.
inline constexpr Target kExitBit = 0x80;     ///< Set in exit targets.
inline constexpr Target kNoTarget = 0xFF;     ///< No target.
inline constexpr uint8_t kExitCodeMask = 0x7F;   ///< Bits of an exit target that hold its code.

/// Target that exits the sequence with a code.
/// @param code  exit code, 0-126
/// @return the target
constexpr Target exit_with(uint8_t code)
{
   return static_cast<Target>(kExitBit | (code & kExitCodeMask));
}

/// Whether a target exits the sequence.
/// @param t  the target
/// @return true for exit targets
constexpr bool is_exit(Target t)
{
   return (t & kExitBit) != 0;
}

/// Exit code of an exit target.
/// @param t  an exit target
/// @return its code, 0-126
constexpr uint8_t exit_code_of(Target t)
{
   return t & kExitCodeMask;
}
///@}

/// Commands. Operands: a (8 bit), b (16 bit), c (32 bit). Numbering is locked
/// against tools/sequence_compiler.py (OPS list).
enum class Op : uint8_t
{
   End,            ///< Ends the track.
   Wait,           ///< b = frames.
   WaitFor,        ///< a = WaitKind, b = parameter; always yields at least one frame.
   Fade,           ///< a = FadeFlag mask, b = frames (hardware brightness, whole screen).
   SetVideoMode,   ///< a = 0, 1, 2.
   BgLoad,         ///< a = bg, c = asset.
   BgShow,         ///< a = bg, b = on.
   BgPos,          ///< a = bg, b = x, c = y (int16).
   BgScroll,       ///< a = bg, b = vx Q8, c = vy Q8.
   Blend,          ///< a = BlendMode, b = src | tgt << 8 (weights 0-16), c = src_layers | tgt_layers << 8.
   BlendRamp,      ///< b = src | tgt << 8 target weights, c = frames.
   SprLoad,        ///< a = slot, c = asset.
   SprShow,        ///< a = slot, b = on.
   SprPos,         ///< a = slot, b = x, c = y (int16).
   SprMove,        ///< a = slot, b = vx Q8, c = vy Q8 | frames << 16 (0 = until changed).
   SprAnim,        ///< a = slot, b = animation index.
   SprObjWindow,   ///< a = slot, b = on.
   SprAffine,      ///< a = slot (< kMaxAffine), b = target scale Q8, c = angle (u16 turn) | frames << 16.
   SprBlend,       ///< a = slot, b = on (semi-transparent object).
   Emitter,        ///< a = slot, b = on, c = particle emitter asset (A-8).
   PalAnim,        ///< a = slot, b = on, c = palette animation asset (A-6, A-9).
   PalFadeColor,   ///< b = BGR555 color used by the next PalFade.
   PalFade,        ///< a = target coefficient 0-16, b = frames, c = palette mask (bit 0-15 BG, 16-31 OBJ).
   Scanline,       ///< a = bg, b = on, c = scanline effect asset (A-7).
   Window,         ///< a = window, b = flags, c = x0 | y0 << 8 | x1 << 16 | y1 << 24.
   WindowMove,     ///< a = window, b = frames, c = target rect (same packing).
   TextBox,        ///< a = open, c = cell rect x | y << 8 | w << 16 | h << 24 (C-01c).
   TextStyle,      ///< a = style index (game text style table).
   Text,           ///< a = TextTarget | TextAlign << 3 | typed << 5, b = x | y << 8 (cells), c = text asset.
   TextClear,      ///< a = TextTarget, b = first_row | rows << 8 (0 rows = all).
   MapShow,        ///< c = map asset (0 = hide): view-only map renderer, no player or event logic.
   MapCamera,      ///< b = x, c = y (int16, px).
   MapScroll,      ///< b = vx Q8, c = vy Q8.
   Song,           ///< b = fade-in frames, c = song asset (0 = stop).
   SongFade,       ///< b = frames.
   Sfx,            ///< c = sound asset.
   Jump,           ///< b = command index relative to the track start.
   If,             ///< a = condition id, b = jump target (track-relative), c = 1 to negate.
   Repeat,         ///< b = count, c = exit index (track-relative).
   ForEach,        ///< a = list id, b = max items (0 = all), c = exit index (track-relative).
   Next,           ///< Closes Repeat / ForEach.
   Goto,           ///< a = Target.
   NUM_OPS         ///< Number of ops.
};

/// What a WaitFor command waits for.
enum class WaitKind : uint8_t
{
   Fade,        ///< Screen fade finished.
   Sprite,      ///< b = slot: movement and affine ramps finished.
   Text,        ///< Text printer idle (C-1).
   Input,       ///< b = keys, any newly pressed.
   SongEnd,     ///< Song stopped.
   SongFrame,   ///< b = frames since the song started (music sync).
   Blend,       ///< Blend ramp finished.
   PalFade,     ///< Palette fade finished.
   Window,      ///< Window moves finished.
   Cond,        ///< b = condition id true.
   NUM_WAIT_KINDS   ///< Number of wait kinds.
};

/// Color special effect of a Blend command.
enum class BlendMode : uint8_t
{
   Off,               ///< No blending.
   Alpha,             ///< Alpha blend source over target.
   Brighten,          ///< Fade toward white.
   Darken,            ///< Fade toward black.
   NUM_BLEND_MODES    ///< Number of modes.
};

/// Where a Text command prints.
enum class TextTarget : uint8_t
{
   Bg0,                ///< Background layer 0.
   Bg1,                ///< Background layer 1.
   Bg2,                ///< Background layer 2.
   Bg3,                ///< Background layer 3.
   Box,                ///< The text box (C-01c).
   NUM_TEXT_TARGETS    ///< Number of targets.
};

/// Alignment of a Text command around its anchor column.
enum class TextAlign : uint8_t
{
   Left,              ///< Starts at the anchor.
   Center,            ///< Centred on the anchor.
   Right,             ///< Ends at the anchor.
   NUM_TEXT_ALIGN     ///< Number of alignments.
};

/// Fade command flags (P-9: bit positions; masks via flag_mask()).
enum FadeFlag : uint8_t
{
   FADE_IN,          ///< Fade in from black / white (else fade out).
   FADE_WHITE,       ///< Fade to or from white (else black).
   NUM_FADE_FLAGS    ///< Number of flags.
};
using FadeMask = uint8_t;   ///< Set of FadeFlag bits.
static_assert(fits_in<FadeMask>(NUM_FADE_FLAGS));

/// Stage flags (P-9: bit positions; masks via flag_mask()).
enum StageFlag : uint8_t
{
   STAGE_RESET_VISUALS,      ///< Clear sprites, effects, text and map view on entry.
   STAGE_SEED_ON_PRESS,      ///< First key press mixes RNG entropy (A-4).
   STAGE_EXIT_ON_SONG_END,   ///< Take on_song_end when the song stops.
   NUM_STAGE_FLAGS           ///< Number of flags.
};
using StageFlagMask = uint8_t;   ///< Set of StageFlag bits.
static_assert(fits_in<StageFlagMask>(NUM_STAGE_FLAGS));

/// One command (ROM).
struct Cmd
{
   Op op;   ///< The command.
   uint8_t a;    ///< 8-bit operand.
   uint16_t b;    ///< 16-bit operand.
   uint32_t c;    ///< 32-bit operand.
};
CAPSULE_POD(Cmd, 8);

/// One stage: its tracks and exit rules (ROM).
struct Stage
{
   uint16_t track_first;      ///< First entry in Sequence::tracks.
   uint8_t track_count;      ///< Tracks, 1..kMaxTracks.
   StageFlagMask flags;            ///< StageFlag bits.
   KeyBits skip_mask;        ///< Any of these newly pressed takes on_skip.
   uint16_t timeout_frames;   ///< Frames before on_timeout; 0 = none.
   Target on_done;          ///< Taken when every track has ended.
   Target on_skip;          ///< Taken on a skip key.
   Target on_timeout;       ///< Taken after timeout_frames.
   Target on_song_end;      ///< Taken when the song stops (STAGE_EXIT_ON_SONG_END).
   uint8_t combo_first;      ///< First entry in Sequence::combos.
   uint8_t combo_count;      ///< Number of combos.
   KeyBits ff_mask;          ///< All held -> fast-forward.
   uint8_t ff_speed;         ///< Logical frames per frame while held (2-16).
   uint8_t ff_cond;          ///< 0 = always, else condition id.
   uint16_t reserved;         ///< Padding; 0.
};
CAPSULE_POD(Stage, 20);

/// A key combo exit (ROM). Fires when all `held` keys are down AND (pressed ==
/// 0 or any `pressed` key is newly pressed) AND (cond == 0 or the condition
/// holds). Declared order is priority; the compiler rejects combos shadowed by
/// an earlier one.
struct Combo
{
   KeyBits held;       ///< Keys that must all be held.
   KeyBits pressed;    ///< Any of these newly pressed (0 = no press needed).
   Target target;     ///< Where the combo leads.
   uint8_t cond;       ///< Condition id that must hold; 0 = none.
   uint16_t reserved;   ///< Padding; 0.
};
CAPSULE_POD(Combo, 8);

/// A compiled sequence: the generated tables (ROM).
struct Sequence
{
   const Stage* stages;        ///< Stage table.
   const uint16_t* tracks;        ///< First command index of each track.
   const Cmd* cmds;          ///< Command table.
   const Combo* combos;        ///< Combo table (may be nullptr).
   uint8_t stage_count;   ///< Number of stages.
   uint8_t start_stage;   ///< Stage entered by start().
};

/// Player status.
enum class Status : uint8_t
{
   Running,      ///< Still playing.
   Exited,       ///< A stage took an exit; Player::exit_code holds it.
   NUM_STATUS    ///< Number of statuses.
};

/// Linear Q8 interpolation shared by every timed effect.
struct Ramp
{
   Q8Wide value;      ///< Current value.
   Q8Wide step;       ///< Added each frame.
   Q8Wide target;     ///< Value reached when left hits 0.
   uint16_t left;       ///< Frames remaining; 0 = idle.
   uint16_t reserved;   ///< Padding.
};

/// Run-time state (RAM). Lives in the scene arena (A-3); about 1 KB with the
/// default limits.
struct Player
{
   /// One open Repeat / ForEach.
   struct Loop
   {
      uint16_t body;          ///< Command index of the loop body.
      uint16_t exit;          ///< Command index after the loop.
      uint16_t i;             ///< Current iteration.
      uint16_t n;             ///< Iterations.
      uint8_t list;           ///< ForEach list id; 0 for Repeat.
      uint8_t reserved[kPadAfterOneByte];    ///< Padding.
   };
   /// One command track.
   struct Track
   {
      uint16_t pc;            ///< Next command index.
      uint16_t base;          ///< First command of the track (jumps are relative to it).
      uint16_t wait;          ///< Frames left in a Wait.
      uint16_t wait_param;    ///< WaitFor parameter.
      uint8_t done;          ///< 1 once the track ended.
      uint8_t wait_kind;     ///< 0 = none, else WaitKind + 1.
      uint8_t depth;         ///< Open loops.
      uint8_t reserved;      ///< Padding.
      Loop loops[kMaxLoopDepth];   ///< Open loops, innermost last.
   };
   /// Position and velocity of a sprite, layer or the map camera.
   struct Motion
   {
      Q8Wide x;           ///< X, Q8 pixels.
      Q8Wide y;           ///< Y, Q8 pixels.
      Q8 vx;              ///< X velocity, Q8 pixels per frame.
      Q8 vy;              ///< Y velocity, Q8 pixels per frame.
      uint16_t frames;    ///< Frames of movement left; 0 = until changed (sprites only).
      uint8_t moving;     ///< 1 while velocity is non-zero.
      uint8_t reserved;   ///< Padding.
   };
   /// Scale and rotation ramps of an affine sprite slot.
   struct Affine
   {
      Ramp scale;   ///< Scale, Q8 of Q8 (256 = 1.0).
      Ramp angle;   ///< Angle, Q8 of a 16-bit turn.
   };

   const Sequence* seq;              ///< The sequence being played.
   Track tracks[kMaxTracks];      ///< Tracks of the current stage.
   Motion sprites[kMaxSprites];    ///< Sprite motion.
   Affine affine[kMaxAffine];      ///< Sprite scale/rotation.
   Motion bgs[kMaxBgs];            ///< Layer scroll motion.
   Motion map;                     ///< Map camera motion.
   Ramp fade;                    ///< Brightness level, Q8 0..16.
   Ramp blend_src;               ///< Source blend weight, Q8 0..16.
   Ramp blend_tgt;               ///< Target blend weight, Q8 0..16.
   Ramp pal_coef;                ///< Palette fade coefficient, Q8 0..16.
   Ramp window[kMaxWindows][kRectEdges];  ///< Window rects: x0, y0, x1, y1.
   uint32_t pal_mask;                ///< Palettes the palette fade applies to.
   uint32_t stage_frames;            ///< Logical frames in the current stage.
   uint32_t total_frames;            ///< Frames since start().
   uint16_t pal_color;               ///< Set by PalFadeColor.
   uint16_t pal_active_color;        ///< Color of the running palette fade.
   uint16_t window_flags[kMaxWindows];   ///< Window flags per window.
   KeyBits pressed;                 ///< This frame, for WaitFor Input.
   uint8_t fade_white;              ///< 1 when fading to or from white.
   uint8_t blend_mode;              ///< Current BlendMode.
   uint8_t text_style;              ///< Style for Text commands.
   uint8_t stage;                   ///< Current stage index.
   uint8_t enter_pending;           ///< 1 when the stage is entered on the next step().
   uint8_t seeded;                  ///< 1 once RNG entropy was mixed (STAGE_SEED_ON_PRESS).
   Status status;                  ///< Running or Exited.
   uint8_t exit_code;               ///< Exit code once Exited.
};

/// Starts a sequence: resets the player, clears visuals and enters the start
/// stage.
/// @param p    player state
/// @param seq  the compiled sequence; must outlive the player
void start(Player& p, const Sequence& seq);

/// Advances one frame; call once per frame from the scene callback (A-2).
/// @param p        player state
/// @param held     keys held this frame
/// @param pressed  keys newly pressed this frame
/// @return Status::Exited once a stage took an exit; exit_code then holds it
Status step(Player& p, KeyBits held, KeyBits pressed);

/// Enters a stage from outside (e.g. returning from a sub-screen).
/// @param p      player state
/// @param stage  stage index
void enter_stage(Player& p, uint8_t stage);

} // namespace capsule::seq
