/// @file
/// Field text placeholders (D-22 decision 6): `{player}`, `{rival}`, ...
///
/// A game declares its field placeholders in its scripts manifest
/// (capsule.scripts/1 "placeholders", tools/script_compiler.py); each is
/// bound to a name slot, a script variable, a term or a party record field.
/// Field texts (script messages and menus, location names) encode them as
/// kCodePlaceholder followed by kFirstFieldArg + the declaration index.
///
/// The printers (text::print(), text::layer::print() and so the script
/// message box) replace each one, when they reach it, by the text the
/// **field resolver** writes. The game registers the resolver once at start
/// with set_field_resolver(): usually script::placeholder_text(), which
/// resolves every binding the script bank knows, wrapped by the game's own
/// function for the record bindings (the game owns its party, see
/// script::placeholder_text()). The engine names no placeholder itself.
///
/// The resolver writes glyph codes only (codes above kMaxGlyphCode are
/// dropped), at most kMaxPlaceholderCodes of them; the expansion is not
/// scanned again, so a placeholder never expands into another.
#pragma once

#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::text
{

/// Writes the text of one field placeholder.
/// @param index     the placeholder's declaration index (its argument - kFirstFieldArg)
/// @param out       where the glyph codes go
/// @param capacity  most codes to write (kMaxPlaceholderCodes)
/// @return codes written (no kCodeEnd), 0 to @p capacity
using FieldResolver = int (*)(uint8_t index, Code* out, int capacity);

/// A field placeholder's text, ready to print: its glyphs and kCodeEnd.
struct FieldText
{
   Code codes[kMaxPlaceholderCodes + 1];   ///< Glyph codes, ending with kCodeEnd.
};

/// Sets the field resolver the printers use (nullptr: none; printing a field
/// placeholder without one asserts, and release builds print nothing for it).
/// @param resolver  the game's resolver
void set_field_resolver(FieldResolver resolver);

/// The field resolver the printers use.
/// @return it, or nullptr
FieldResolver field_resolver();

/// Expands one field placeholder through the resolver.
/// @param arg  the code after kCodePlaceholder (is_field_arg())
/// @param out  receives the glyphs and kCodeEnd (empty without a resolver or for a battle argument)
/// @return glyphs written
int expand_field(Code arg, FieldText& out);

/// Cells the first line of a text takes: glyphs, plus each field
/// placeholder's expansion; control escapes, prompts and battle placeholders
/// take none. The line ends at kCodeNewline, kCodePageBreak or kCodeEnd.
/// @param codes  encoded text
/// @return cells
int line_cells(const Code* codes);

} // namespace capsule::text
