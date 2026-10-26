// Compile-time checks of the core headers. Building this file is the test;
// it is compiled for the host and for armv4t (GBA) so both ABIs agree.
#include "capsule/core/assert.h"
#include "capsule/core/endian.h"
#include "capsule/core/pod.h"
#include "capsule/core/saturate.h"
#include "capsule/core/types.h"
#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"

#include <cstdint>
#include <limits>
#include <type_traits>

namespace
{

using namespace capsule;

// P-7 / P-1: a packed ROM record locks its size on both targets.
struct ExampleRecord
{
   uint16_t id;
   uint8_t kind;
   uint8_t flags;
   Q8Wide scale;
};
CAPSULE_POD(ExampleRecord, 8);

// P-9: strong types keep their raw size and convert only explicitly.
static_assert(sizeof(SpeciesIndex) == 2 && sizeof(MoveIndex) == 2 && sizeof(AssetKey) == 4);
static_assert(raw(SpeciesIndex{7}) == 7);
static_assert(!std::is_convertible_v<SpeciesIndex, MoveIndex>);
static_assert(!std::is_convertible_v<uint16_t, SpeciesIndex>);

enum ExampleFlag : uint8_t
{
   FLAG_A,
   FLAG_B,
   FLAG_C,
   NUM_EXAMPLE_FLAGS
};
using ExampleMask = uint8_t;
static_assert(fits_in<ExampleMask>(NUM_EXAMPLE_FLAGS));
static_assert(flag_mask<ExampleMask>(FLAG_C) == 0b100);
static_assert(!fits_in<uint8_t>(9));

// P-4: Q8 one and a multiplier applied as (value * q8) >> 8.
static_assert(kQ8One == 256);
static_assert((Q8Wide{100} * (kQ8One + kQ8One / 2)) >> kQ8Shift == 150);

// P-18: saturating arithmetic never wraps.
static_assert(sat_add<uint8_t>(250, 10, 0, 255) == 255);
static_assert(sat_sub<uint8_t>(5, 10, 0, 255) == 0);
static_assert(sat_add<int32_t>(std::numeric_limits<int32_t>::max(), 1, 0, 999) == 999);
static_assert(sat_add<int16_t>(10, 5, 0, 12) == 12);
static_assert(sat_sub<int16_t>(10, -5, 0, 12) == 12);

// C-1: 2bpp -> 4bpp expansion keeps pixel order (bits 1:0 = leftmost pixel).
static_assert(text::expand_2bpp(0x00) == 0x0000);
static_assert(text::expand_2bpp(0x01) == 0x0001);   // leftmost pixel = 1
static_assert(text::expand_2bpp(0x80) == 0x2000);   // rightmost pixel = 2
static_assert(text::expand_2bpp(0xE4) == 0x3210);   // pixels 0,1,2,3

// C-2: build-time encoding, newline and terminator.
constexpr auto kText = text::encode("Hi!\nA");
static_assert(sizeof(kText.codes) == 6);
static_assert(kText.codes[0] == 'H' && kText.codes[2] == '!');
static_assert(kText.codes[3] == text::kCodeNewline && kText.codes[5] == text::kCodeEnd);

// P-19: fixed little-endian layout regardless of the host's byte order.
constexpr uint8_t kLe[] = {0x34, 0x12, 0x78, 0x56, 0xFE, 0xFF};
static_assert(load_le16(kLe) == 0x1234 && load_le32(kLe) == 0x56781234u);
static_assert(load_le16s(kLe + 4) == -2);
static_assert(load_le32(kLe + 1) == 0xFE567812u);   // any byte offset, no alignment needed
constexpr bool kStoreRoundTrips = []
{
   uint8_t b[5] = {};
   store_le32(b + 1, 0xA1B2C3D4u);
   store_le16(b, 0x0102);   // overwrites b[0..1]
   return b[0] == 0x02 && b[1] == 0x01 && b[2] == 0xC3 && b[3] == 0xB2 && b[4] == 0xA1 &&
          load_le32(b + 1) == 0xA1B2C301u;
}();
static_assert(kStoreRoundTrips);

// Video HAL helpers.
static_assert(hal::video::rgb5(31, 0, 0) == 0x001F && hal::video::rgb5(0, 0, 31) == 0x7C00);
static_assert(hal::video::map_cell(5, 3, true, false) == (5 | 0x400 | (3 << 12)));

} // namespace

// The test links without a HAL, so it supplies the assert hook itself.
[[noreturn]] void capsule::hal::system::assert_failed(const char*, const char*, int)
{
   __builtin_trap();
}

int main()
{
   ENGINE_ASSERT(sizeof(ExampleRecord) == 8);
   return 0;
}
