/// @file
/// The scene / animation viewer (BF, D-38): a command-line tool on the software video model.
///
///    capsule_anim_view --disasm BANK.bin        lists a bank's scripts, symbols and commands (the engine's reader)
///    capsule_anim_view --frames 1,60,... --out FILE [--count N]
///                                               plays the sandbox's scenes (scenes.h) and writes the listed frames
///                                               (frame k: the picture after the k-th vertical blank, as the GBA's
///                                               k-th frame) as raw little-endian BGR555, 240 x 160 each, in order
///    capsule_anim_view --check                  plays two rounds and checks the report (every script run ended
///                                               with End, scanline tables were shown), printing a frame hash
#include "scenes.h"

#include "capsule/anim/bank.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"

#include "hal/soft/soft_video.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace capsule::hal::system
{
// The viewer's assert handler (declared and documented in capsule/hal/system.h): report and abort.
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::fprintf(stderr, "ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
}   // namespace capsule::hal::system

namespace
{

namespace soft = capsule::hal::soft;
namespace anim = capsule::anim;

constexpr int kMaxFrames = 256;          ///< Frames one run may write.
constexpr int kMaxBankBytes = 1 << 20;   ///< Largest bank --disasm reads.
constexpr int kDecimal = 10;             ///< Frame numbers are decimal.
constexpr uint32_t kFnvBasis = 2166136261u;   ///< FNV-1a start.
constexpr uint32_t kFnvPrime = 16777619u;     ///< FNV-1a step.
constexpr int kCheckRounds = 2;               ///< Rounds --check plays.
constexpr int kBytesPerPixel = 2;             ///< BGR555 bytes.
constexpr int kByteBits = 8;                  ///< Bits per byte.
constexpr uint8_t kByteMask = 0xFF;           ///< One byte.

uint16_t g_frame[soft::kFramePixels];    ///< The composed frame.
alignas(uint32_t) uint8_t g_bank_bytes[kMaxBankBytes];   ///< --disasm's bank.

/// The soft HAL's vertical blank: queued uploads land, the registers latch, the frame is composed.
void vblank()
{
   capsule::hal::vram::flush();
   soft::latch();
   soft::compose(g_frame);
}

/// Writes a frame as little-endian BGR555.
/// @param out  the file
void write_frame(std::FILE* out)
{
   for (uint16_t c : g_frame)
   {
      const uint8_t b[kBytesPerPixel] = {static_cast<uint8_t>(c & kByteMask), static_cast<uint8_t>(c >> kByteBits)};
      std::fwrite(b, 1, kBytesPerPixel, out);
   }
}

/// Lists a bank.
/// @param path  its file
/// @return exit status
int disasm(const char* path)
{
   std::FILE* f = std::fopen(path, "rb");
   if (f == nullptr)
   {
      std::fprintf(stderr, "cannot read %s\n", path);
      return 1;
   }
   const size_t n = std::fread(g_bank_bytes, 1, sizeof(g_bank_bytes), f);
   std::fclose(f);
   anim::Bank b{};
   const anim::BankError e = anim::open(g_bank_bytes, static_cast<uint32_t>(n), b);
   if (e != anim::BankError::None)
   {
      std::fprintf(stderr, "not a good bank (error %d)\n", static_cast<int>(e));
      return 1;
   }
   std::printf("%u scripts, %u symbols, %u game opcodes, %u code bytes%s\n", b.script_count, b.symbol_count,
               b.opcode_count, b.code_size, (b.flags & anim::kFlagIndexed) != 0 ? ", indexed" : "");
   for (uint16_t i = 0; i < b.symbol_count; ++i)
   {
      std::printf("symbol %u: kind %d index %u %s\n", i, static_cast<int>(anim::symbol_kind(b, i)),
                  anim::symbol_index(b, i), anim::symbol_name(b, i));
   }
   for (uint16_t i = 0; i < b.script_count; ++i)
   {
      std::printf("script %u: %s at %u\n", i, anim::script_name(b, i), anim::script_address(b, i));
   }
   for (uint32_t at = 0; at < b.code_size;)
   {
      const uint32_t size = anim::command_bytes(b, at);
      if (size == 0)
      {
         std::printf("%6u: bad command %u\n", at, b.code[at]);
         return 1;
      }
      const uint8_t op = b.code[at];
      const char* name = op <= anim::kLastEngineOpcode ? anim::op_name(op) : anim::game_opcode_name(b, op);
      std::printf("%6u: %-18s (%s)", at, name != nullptr ? name : "?", anim::signature(b, op));
      for (uint32_t i = 1; i < size; ++i)
      {
         std::printf(" %02x", b.code[at + i]);
      }
      std::printf("\n");
      at += size;
   }
   uint32_t bad = 0;
   static uint32_t bits[anim::boundary_words(kMaxBankBytes)];
   if (!anim::check_code(b, bits, bad))
   {
      std::printf("code check failed at %u\n", bad);
      return 1;
   }
   return 0;
}

/// Plays frames and writes the listed ones.
/// @param list   comma-separated frame numbers (1-based, increasing)
/// @param path   the output file
/// @return exit status
int frames(const char* list, const char* path)
{
   int wanted[kMaxFrames];
   int count = 0;
   for (const char* p = list; *p != '\0' && count < kMaxFrames;)
   {
      char* end = nullptr;
      wanted[count++] = static_cast<int>(std::strtol(p, &end, kDecimal));
      p = *end == ',' ? end + 1 : end;
   }
   std::FILE* out = std::fopen(path, "wb");
   if (out == nullptr || count == 0)
   {
      std::fprintf(stderr, "nothing to write\n");
      return 1;
   }
   soft::reset();
   capsule::hal::vram::reset();
   int next = 0;
   uint32_t f = 0;
   for (int k = 1; next < count; ++k)
   {
      vblank();   // the k-th vertical blank: frame k shows
      if (k == wanted[next])
      {
         write_frame(out);
         ++next;
      }
      sandbox_anim::frame(f++);
   }
   std::fclose(out);
   return 0;
}

/// Plays rounds and checks the report.
/// @return exit status
int check()
{
   soft::reset();
   capsule::hal::vram::reset();
   uint32_t hash = kFnvBasis;
   for (uint32_t f = 0; f < static_cast<uint32_t>(kCheckRounds * sandbox_anim::kTotalFrames); ++f)
   {
      vblank();
      for (uint16_t c : g_frame)
      {
         hash = (hash ^ c) * kFnvPrime;
      }
      sandbox_anim::frame(f);
   }
   const sandbox_anim::Report& r = sandbox_anim::report();
   std::printf("scripts ended %u, player errors %u, scanline frames %u, frame hash %08x\n", r.scripts_ended,
               r.player_errors, r.scanline_frames, hash);
   const bool ok = r.scripts_ended >= kCheckRounds && r.player_errors == 0 &&
                   r.scanline_frames >= kCheckRounds * sandbox_anim::kSceneFrames;
   return ok ? 0 : 1;
}

}   // namespace

/// The viewer's command line (see the file comment).
/// @param argc  argument count
/// @param argv  arguments
/// @return exit status
int main(int argc, char** argv)
{
   const char* list = nullptr;
   const char* out = nullptr;
   for (int i = 1; i < argc; ++i)
   {
      if (std::strcmp(argv[i], "--disasm") == 0 && i + 1 < argc)
      {
         return disasm(argv[i + 1]);
      }
      if (std::strcmp(argv[i], "--check") == 0)
      {
         return check();
      }
      if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
      {
         list = argv[++i];
      }
      else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
      {
         out = argv[++i];
      }
   }
   if (list != nullptr && out != nullptr)
   {
      return frames(list, out);
   }
   constexpr int kUsageStatus = 2;
   std::fprintf(stderr, "usage: capsule_anim_view --disasm BANK | --frames LIST --out FILE | --check\n");
   return kUsageStatus;
}
