/// @file
/// Fixed-cell text printing on background layers (see capsule/text/print.h)
/// and the field placeholder resolver (capsule/text/placeholder.h).
#include "capsule/text/print.h"

#include "capsule/core/assert.h"
#include "capsule/core/types.h"
#include "capsule/text/placeholder.h"

namespace capsule::text
{

namespace
{

FieldResolver g_field_resolver = nullptr;   ///< set_field_resolver().

} // namespace

void set_field_resolver(FieldResolver resolver)
{
   g_field_resolver = resolver;
}

FieldResolver field_resolver()
{
   return g_field_resolver;
}

int expand_field(Code arg, FieldText& out)
{
   int n = 0;
   if (is_field_arg(arg))
   {
      // A field placeholder reached a printer: the game must have registered its resolver.
      ENGINE_ASSERT(g_field_resolver != nullptr && "field placeholder printed without text::set_field_resolver()");
      if (g_field_resolver != nullptr)
      {
         const int got = g_field_resolver(static_cast<uint8_t>(arg - kFirstFieldArg), out.codes, kMaxPlaceholderCodes);
         // Keep the glyphs only (in place): the expansion is printed as it is, never scanned again.
         for (int i = 0; i < got && i < kMaxPlaceholderCodes; ++i)
         {
            if (out.codes[i] <= kMaxGlyphCode)
            {
               out.codes[n++] = out.codes[i];
            }
         }
      }
   }
   out.codes[n] = kCodeEnd;
   return n;
}

int line_cells(const Code* codes)
{
   int cells = 0;
   for (int n = 0; codes[n] != kCodeEnd && codes[n] != kCodeNewline && codes[n] != kCodePageBreak; ++n)
   {
      const Code c = codes[n];
      if (c == kCodeControl || c == kCodePlaceholder)
      {   // an escape and its code; a field placeholder takes its expansion's cells
         if (codes[n + 1] == kCodeEnd)
         {
            break;
         }
         if (c == kCodePlaceholder && is_field_arg(codes[n + 1]))
         {
            FieldText t;
            cells += expand_field(codes[n + 1], t);
         }
         n += escape_length(codes + n) - 1;   // I4: a control's argument bytes too
         continue;
      }
      if (c != kCodePrompt)
      {
         ++cells;
      }
   }
   return cells;
}

void print(const PrintTarget& target, const FontInfo& font, int x, int y, const Code* codes)
{
   int cx = x;

   // Writes one glyph cell and moves on.
   const auto put = [&](Code c)
   {
      ENGINE_ASSERT(c >= font.first_code && c < font.first_code + font.glyph_count);
      const auto tile = static_cast<hal::video::TileIndex>(target.font_first_tile + (c - font.first_code));
      hal::video::write_map_cell(target.bg, cx, y, hal::video::map_cell(tile, target.palette_bank));
      ++cx;
   };
   for (; *codes != kCodeEnd; ++codes)
   {
      const Code c = *codes;
      if (c == kCodeNewline || c == kCodePageBreak)
      {   // no pages here: a page break continues on the next row
         cx = x;
         ++y;
         continue;
      }
      if (c == kCodePrompt)
      {   // nothing to wait for in a fixed-cell print
         continue;
      }
      if (c == kCodeControl || c == kCodePlaceholder)
      {   // an escape and its code: no control is known here
         if (codes[1] == kCodeEnd)
         {
            break;
         }
         // Battle placeholders are replaced before printing (battle::format()); a raw one is a bug.
         ENGINE_ASSERT(c != kCodePlaceholder || is_field_arg(codes[1]));
         if (c == kCodePlaceholder)
         {
            FieldText t;
            const int n = expand_field(codes[1], t);
            for (int i = 0; i < n; ++i)
            {
               put(t.codes[i]);
            }
         }
         codes += escape_length(codes) - 1;   // I4: a control's argument bytes too
         continue;
      }
      put(c);
   }
}

void clear(const PrintTarget& target, const FontInfo& font)
{
   constexpr Code kBlank = kCodeSpace;
   ENGINE_ASSERT(kBlank >= font.first_code && kBlank < font.first_code + font.glyph_count);
   const auto tile = static_cast<hal::video::TileIndex>(target.font_first_tile + (kBlank - font.first_code));
   const hal::video::MapCell cell = hal::video::map_cell(tile, target.palette_bank);
   for (int y = 0; y < kBgMapCells; ++y)
   {
      for (int x = 0; x < kBgMapCells; ++x)
      {
         hal::video::write_map_cell(target.bg, x, y, cell);
      }
   }
}

} // namespace capsule::text
