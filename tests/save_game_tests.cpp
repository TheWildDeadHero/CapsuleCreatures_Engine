/// @file
/// N3 (D-32): the save image's Game section (capsule/save/save.h GameBlock): a game's block written and read back
/// byte for byte with its version, refused for another version or size (nothing changes), copied from the newest
/// copy when a write has no block; the image's size counts it. A block too large for two copies: the image is kept
/// once, and a write without the block keeps it through the built-in stage (the write erases where it was). On the
/// engine library's defaults (no bag, party or boxes data) and the RAM save medium (64 KB).
#include "capsule/save/save.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{

namespace save = capsule::save;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr uint32_t kBlockBytes = 64;    ///< The test game's block.
constexpr uint32_t kBigBlockBytes = 40000;   ///< A block larger than half the 64 KB medium: one copy.
constexpr uint16_t kVersion = 3;        ///< Its layout version.
constexpr uint8_t kPattern = 0x5A;      ///< Its bytes start from this.

uint8_t g_block[kBigBlockBytes];   ///< The game's block.
uint8_t g_read[kBigBlockBytes];    ///< Where a read puts it.
uint32_t g_bytes = kBlockBytes;    ///< The block's size in this part of the test.

/// Sources with only a game block.
/// @param bytes    the block, or nullptr
/// @param version  its version
/// @return the sources
save::Sources game_only(uint8_t* bytes, uint16_t version)
{
   save::Sources s{nullptr, nullptr, nullptr, nullptr};
   s.game = save::GameBlock{bytes, g_bytes, version};
   return s;
}

/// Whether two blocks are equal.
/// @param a  one
/// @param b  the other
/// @return true when every byte is
bool same(const uint8_t* a, const uint8_t* b)
{
   for (uint32_t i = 0; i < g_bytes; ++i)
   {
      if (a[i] != b[i])
      {
         return false;
      }
   }
   return true;
}

}   // namespace

/// The test game's block size.
/// @return its bytes in this part of the test
uint32_t capsule::save::game_block_bytes()
{
   return g_bytes;
}

int main()
{
   CHECK(save::section_size(save::Section::Game) == save::kGameHeadBytes + kBlockBytes);
   for (uint32_t i = 0; i < kBlockBytes; ++i)
   {
      g_block[i] = static_cast<uint8_t>(kPattern + i);
   }
   // nothing on the medium: a read finds nothing
   CHECK(save::read(game_only(g_read, kVersion)) == save::Status::Empty);
   CHECK(save::write(game_only(g_block, kVersion)) == save::Status::Ok);
   CHECK(save::read(game_only(g_read, kVersion)) == save::Status::Ok && same(g_read, g_block));
   // another version or size: refused, nothing changes
   for (uint8_t& b : g_read)
   {
      b = 0;
   }
   CHECK(save::read(game_only(g_read, kVersion + 1)) == save::Status::OtherShape);
   save::Sources shorter = game_only(g_read, kVersion);
   shorter.game.size = kBlockBytes - 1;
   CHECK(save::read(shorter) == save::Status::OtherShape);
   CHECK(g_read[0] == 0 && g_read[kBlockBytes - 1] == 0);
   // a write without the block copies it from the newest copy
   CHECK(save::write(game_only(nullptr, kVersion)) == save::Status::Ok);
   CHECK(save::read(game_only(g_read, kVersion)) == save::Status::Ok && same(g_read, g_block));
   // the other sources' reads leave the block alone; the whole image counts the section
   CHECK(save::read(game_only(nullptr, kVersion)) == save::Status::Ok);
   CHECK(save::image_size() >= save::kHeaderBytes + save::kSectionEntryBytes + save::kGameHeadBytes + kBlockBytes);
   // a block too large for two copies: one copy, and a write without the block keeps it (through the stage)
   g_bytes = kBigBlockBytes;
   CHECK(save::copy_count() == 1);
   for (uint32_t i = 0; i < kBigBlockBytes; ++i)
   {
      g_block[i] = static_cast<uint8_t>(kPattern ^ i);
   }
   CHECK(save::write(game_only(g_block, kVersion)) == save::Status::Ok);
   CHECK(save::write(game_only(nullptr, kVersion)) == save::Status::Ok);
   CHECK(save::write(game_only(nullptr, kVersion)) == save::Status::Ok);
   CHECK(save::read(game_only(g_read, kVersion)) == save::Status::Ok && same(g_read, g_block));
   std::printf("save game tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
