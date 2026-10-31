/// @file
/// A small JSON reader and writer for the battle bench (host program only; never built for the GBA).
///
/// The reader takes RFC 8259 text with one restriction the engine imposes: numbers are integers (no fraction and
/// no exponent; the engine has no fractional numbers). Every value keeps the line and column it starts at, so a
/// scenario error points into the file.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace capsule::bench::json
{

/// What a value is.
enum class Kind : uint8_t
{
   Null,      ///< null
   Bool,      ///< true / false
   Number,    ///< An integer.
   String,    ///< Text (UTF-8).
   Array,     ///< Values.
   Object,    ///< Named values, in file order.
   NUM_KINDS  ///< Number of kinds.
};

/// One parsed value.
struct Value
{
   Kind kind = Kind::Null;                                ///< What it is.
   bool boolean = false;                                  ///< Bool.
   int64_t number = 0;                                    ///< Number.
   std::string text;                                      ///< String.
   std::vector<Value> items;                              ///< Array.
   std::vector<std::pair<std::string, Value>> members;    ///< Object.
   int line = 0;                                          ///< Where it starts (1 = first line).
   int column = 0;                                        ///< Where it starts (1 = first column).

   // `members` holds Value inside a std::pair, so the vector's destructor needs Value complete. C++20 makes that
   // destructor constexpr and Clang instantiates it with the class, where Value is still incomplete; declaring the
   // destructor here and defining it in json.cpp moves that to a point where Value is complete.
   /// An empty null value.
   Value() = default;
   /// A copy.
   /// @param other  the value copied
   Value(const Value& other) = default;
   /// Takes another value's contents.
   /// @param other  the value moved from
   Value(Value&& other) = default;
   /// Copies another value.
   /// @param other  the value copied
   /// @return this value
   Value& operator=(const Value& other) = default;
   /// Takes another value's contents.
   /// @param other  the value moved from
   /// @return this value
   Value& operator=(Value&& other) = default;
   /// Frees the strings and child values (defined in json.cpp, see above).
   ~Value();

   /// A member of an object.
   /// @param key  its name
   /// @return the value, or nullptr (also when this is not an object)
   const Value* find(const char* key) const;
};

/// Parses a whole document.
/// @param text   the document
/// @param out    the value
/// @param error  the message when it fails ("line L, column C: ...")
/// @return false on a syntax error
bool parse(const std::string& text, Value& out, std::string& error);

/// Appends a string as a JSON string literal (quotation marks included).
/// @param out   text
/// @param text  the string (UTF-8)
void put_string(std::string& out, const std::string& text);

/// A kind's name for messages.
/// @param k  kind
/// @return "null", "a boolean", ...
const char* kind_name(Kind k);

} // namespace capsule::bench::json
