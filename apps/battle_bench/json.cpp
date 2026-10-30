/// @file
/// The battle bench's JSON reader and writer (see json.h).
#include "json.h"

#include <cstdio>

namespace capsule::bench::json
{

namespace
{

/// Deepest nesting of arrays and objects (a scenario needs a few levels).
constexpr int kMaxDepth = 64;
/// Base of decimal digits.
constexpr int64_t kDecimal = 10;
/// Base of hexadecimal digits (\\u escapes).
constexpr uint32_t kHex = 16;
/// Value of the hexadecimal digit 'a'.
constexpr uint32_t kHexLetter = 10;
/// Digits of a \\u escape.
constexpr int kEscapeDigits = 4;
/// Largest magnitude a number may have (its digits must not overflow int64_t).
constexpr int64_t kMaxMagnitude = INT64_MAX / kDecimal - 1;

/// @name UTF-8 encoding of a code point
///@{
constexpr uint32_t kOneByteMax = 0x7F;           ///< Highest code point of one byte.
constexpr uint32_t kTwoByteMax = 0x7FF;          ///< Highest code point of two bytes.
constexpr uint32_t kThreeByteMax = 0xFFFF;       ///< Highest code point of three bytes.
constexpr uint32_t kLeadTwo = 0xC0;              ///< Lead byte of two bytes.
constexpr uint32_t kLeadThree = 0xE0;            ///< Lead byte of three bytes.
constexpr uint32_t kLeadFour = 0xF0;             ///< Lead byte of four bytes.
constexpr uint32_t kContinuation = 0x80;         ///< Continuation byte marker.
constexpr uint32_t kContinuationMask = 0x3F;     ///< Payload bits of a continuation byte.
constexpr int kContinuationBits = 6;             ///< Payload bits per continuation byte.
constexpr int kShiftTwo = 2 * kContinuationBits;     ///< Shift to the payload two continuation bytes up.
constexpr int kShiftThree = 3 * kContinuationBits;   ///< Shift to the payload three continuation bytes up.
constexpr uint32_t kHighSurrogate = 0xD800;      ///< First high surrogate.
constexpr uint32_t kLowSurrogate = 0xDC00;       ///< First low surrogate.
constexpr uint32_t kSurrogateEnd = 0xE000;       ///< Past the last low surrogate.
constexpr uint32_t kSurrogateBase = 0x10000;     ///< Code point of the first surrogate pair.
constexpr int kSurrogateBits = 10;               ///< Payload bits per surrogate.
///@}

/// Room for an error's position prefix.
constexpr int kWhereCap = 48;
/// First code that needs no escape in a JSON string.
constexpr unsigned char kFirstPlain = 0x20;

/// Appends a code point as UTF-8.
/// @param out  text
/// @param cp   code point
void put_utf8(std::string& out, uint32_t cp)
{
   if (cp <= kOneByteMax)
   {
      out += static_cast<char>(cp);
   }
   else if (cp <= kTwoByteMax)
   {
      out += static_cast<char>(kLeadTwo | (cp >> kContinuationBits));
      out += static_cast<char>(kContinuation | (cp & kContinuationMask));
   }
   else if (cp <= kThreeByteMax)
   {
      out += static_cast<char>(kLeadThree | (cp >> kShiftTwo));
      out += static_cast<char>(kContinuation | ((cp >> kContinuationBits) & kContinuationMask));
      out += static_cast<char>(kContinuation | (cp & kContinuationMask));
   }
   else
   {
      out += static_cast<char>(kLeadFour | (cp >> kShiftThree));
      out += static_cast<char>(kContinuation | ((cp >> kShiftTwo) & kContinuationMask));
      out += static_cast<char>(kContinuation | ((cp >> kContinuationBits) & kContinuationMask));
      out += static_cast<char>(kContinuation | (cp & kContinuationMask));
   }
}

/// The reader: a cursor over the document.
struct Reader
{
   const std::string& text;   ///< The document.
   size_t at = 0;             ///< Next character.
   int line = 1;              ///< Its line.
   int column = 1;            ///< Its column.
   std::string error;         ///< The first error.

   /// Whether the document has more characters.
   /// @return true before the end
   bool more() const
   {
      return at < text.size();
   }

   /// The next character (0 at the end).
   /// @return it
   char peek() const
   {
      return more() ? text[at] : '\0';
   }

   /// Takes one character.
   /// @return it
   char take()
   {
      const char c = text[at++];
      if (c == '\n')
      {
         ++line;
         column = 1;
      }
      else
      {
         ++column;
      }
      return c;
   }

   /// Records an error at the cursor (the first one wins).
   /// @param message  what is wrong
   /// @return false
   bool fail(const std::string& message)
   {
      if (error.empty())
      {
         char where[kWhereCap];
         std::snprintf(where, sizeof(where), "line %d, column %d: ", line, column);
         error = where + message;
      }
      return false;
   }

   /// Skips white space.
   void skip_space()
   {
      while (more() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
      {
         take();
      }
   }

   /// Takes a keyword (true, false, null).
   /// @param word  the keyword
   /// @return false when the text differs
   bool keyword(const char* word)
   {
      for (int k = 0; word[k] != '\0'; ++k)
      {
         if (peek() != word[k])
         {
            return fail(std::string("expected ") + word);
         }
         take();
      }
      return true;
   }

   /// Reads four hexadecimal digits.
   /// @param out  their value
   /// @return false on a bad digit
   bool hex4(uint32_t& out)
   {
      out = 0;
      for (int k = 0; k < kEscapeDigits; ++k)
      {
         const char c = more() ? take() : '\0';
         uint32_t d = 0;
         if (c >= '0' && c <= '9')
         {
            d = static_cast<uint32_t>(c - '0');
         }
         else if (c >= 'a' && c <= 'f')
         {
            d = static_cast<uint32_t>(c - 'a') + kHexLetter;
         }
         else if (c >= 'A' && c <= 'F')
         {
            d = static_cast<uint32_t>(c - 'A') + kHexLetter;
         }
         else
         {
            return fail("bad \\u escape");
         }
         out = out * kHex + d;
      }
      return true;
   }

   /// Reads a string literal (the cursor is on its opening quotation mark).
   /// @param out  its text
   /// @return false on an error
   bool string(std::string& out)
   {
      take();
      out.clear();
      while (true)
      {
         if (!more())
         {
            return fail("unterminated string");
         }
         const char c = take();
         if (c == '"')
         {
            return true;
         }
         if (static_cast<unsigned char>(c) < kFirstPlain)
         {
            return fail("control character in a string");
         }
         if (c != '\\')
         {
            out += c;
            continue;
         }
         if (!more())
         {
            return fail("unterminated string");
         }
         const char e = take();
         switch (e)
         {
         case '"':
         case '\\':
         case '/':
            out += e;
            break;
         case 'b':
            out += '\b';
            break;
         case 'f':
            out += '\f';
            break;
         case 'n':
            out += '\n';
            break;
         case 'r':
            out += '\r';
            break;
         case 't':
            out += '\t';
            break;
         case 'u':
         {
            uint32_t cp = 0;
            if (!hex4(cp))
            {
               return false;
            }
            if (cp >= kHighSurrogate && cp < kLowSurrogate)
            {
               // A high surrogate must be followed by \u and a low one.
               uint32_t low = 0;
               if (peek() != '\\' || (take(), peek() != 'u'))
               {
                  return fail("lone surrogate in a \\u escape");
               }
               take();
               if (!hex4(low))
               {
                  return false;
               }
               if (low < kLowSurrogate || low >= kSurrogateEnd)
               {
                  return fail("lone surrogate in a \\u escape");
               }
               cp = kSurrogateBase + ((cp - kHighSurrogate) << kSurrogateBits) + (low - kLowSurrogate);
            }
            else if (cp >= kLowSurrogate && cp < kSurrogateEnd)
            {
               return fail("lone surrogate in a \\u escape");
            }
            put_utf8(out, cp);
            break;
         }
         default:
            return fail(std::string("bad escape \\") + e);
         }
      }
   }

   /// Reads an integer.
   /// @param v  the value
   /// @return false on an error (also for a fraction or an exponent)
   bool number(Value& v)
   {
      const bool negative = peek() == '-';
      if (negative)
      {
         take();
      }
      if (!(peek() >= '0' && peek() <= '9'))
      {
         return fail("expected a digit");
      }
      if (peek() == '0' && at + 1 < text.size() && text[at + 1] >= '0' && text[at + 1] <= '9')
      {
         return fail("a number may not start with 0");
      }
      int64_t n = 0;
      while (peek() >= '0' && peek() <= '9')
      {
         if (n > kMaxMagnitude)
         {
            return fail("number too large");
         }
         n = n * kDecimal + (take() - '0');
      }
      if (peek() == '.' || peek() == 'e' || peek() == 'E')
      {
         return fail("numbers are integers here (no fraction or exponent)");
      }
      v.kind = Kind::Number;
      v.number = negative ? -n : n;
      return true;
   }

   /// Reads any value.
   /// @param v      the value
   /// @param depth  nesting so far
   /// @return false on an error
   bool value(Value& v, int depth)
   {
      skip_space();
      v.line = line;
      v.column = column;
      if (!more())
      {
         return fail("unexpected end of the document");
      }
      if (depth > kMaxDepth)
      {
         return fail("nested too deeply");
      }
      const char c = peek();
      if (c == '{')
      {
         return object(v, depth);
      }
      if (c == '[')
      {
         return array(v, depth);
      }
      if (c == '"')
      {
         v.kind = Kind::String;
         return string(v.text);
      }
      if (c == 't' || c == 'f')
      {
         v.kind = Kind::Bool;
         v.boolean = c == 't';
         return keyword(v.boolean ? "true" : "false");
      }
      if (c == 'n')
      {
         v.kind = Kind::Null;
         return keyword("null");
      }
      if (c == '-' || (c >= '0' && c <= '9'))
      {
         return number(v);
      }
      return fail(std::string("unexpected '") + c + "'");
   }

   /// Reads an array (the cursor is on '[').
   /// @param v      the value
   /// @param depth  nesting so far
   /// @return false on an error
   bool array(Value& v, int depth)
   {
      v.kind = Kind::Array;
      take();
      skip_space();
      if (peek() == ']')
      {
         take();
         return true;
      }
      while (true)
      {
         v.items.emplace_back();
         if (!value(v.items.back(), depth + 1))
         {
            return false;
         }
         skip_space();
         if (peek() == ',')
         {
            take();
            continue;
         }
         if (peek() == ']')
         {
            take();
            return true;
         }
         return fail("expected ',' or ']'");
      }
   }

   /// Reads an object (the cursor is on '{').
   /// @param v      the value
   /// @param depth  nesting so far
   /// @return false on an error (also for a repeated name)
   bool object(Value& v, int depth)
   {
      v.kind = Kind::Object;
      take();
      skip_space();
      if (peek() == '}')
      {
         take();
         return true;
      }
      while (true)
      {
         skip_space();
         if (peek() != '"')
         {
            return fail("expected a member name");
         }
         std::string key;
         if (!string(key))
         {
            return false;
         }
         if (v.find(key.c_str()) != nullptr)
         {
            return fail("member \"" + key + "\" given twice");
         }
         skip_space();
         if (peek() != ':')
         {
            return fail("expected ':'");
         }
         take();
         v.members.emplace_back(key, Value{});
         if (!value(v.members.back().second, depth + 1))
         {
            return false;
         }
         skip_space();
         if (peek() == ',')
         {
            take();
            continue;
         }
         if (peek() == '}')
         {
            take();
            return true;
         }
         return fail("expected ',' or '}'");
      }
   }
};

} // namespace

// Defined here, where Value is complete (json.h says why).
Value::~Value() = default;

const Value* Value::find(const char* key) const
{
   if (kind != Kind::Object)
   {
      return nullptr;
   }
   for (const auto& m : members)
   {
      if (m.first == key)
      {
         return &m.second;
      }
   }
   return nullptr;
}

bool parse(const std::string& text, Value& out, std::string& error)
{
   Reader r{text, 0, 1, 1, std::string()};
   out = Value{};
   if (!r.value(out, 0))
   {
      error = r.error;
      return false;
   }
   r.skip_space();
   if (r.more())
   {
      r.fail("text after the document");
      error = r.error;
      return false;
   }
   return true;
}

void put_string(std::string& out, const std::string& text)
{
   static const char kDigits[] = "0123456789abcdef";
   out += '"';
   for (const char c : text)
   {
      const unsigned char u = static_cast<unsigned char>(c);
      if (c == '"' || c == '\\')
      {
         out += '\\';
         out += c;
      }
      else if (c == '\n')
      {
         out += "\\n";
      }
      else if (c == '\t')
      {
         out += "\\t";
      }
      else if (c == '\r')
      {
         out += "\\r";
      }
      else if (u < kFirstPlain)
      {
         out += "\\u00";
         out += kDigits[u / kHex];
         out += kDigits[u % kHex];
      }
      else
      {
         out += c;
      }
   }
   out += '"';
}

const char* kind_name(Kind k)
{
   switch (k)
   {
   case Kind::Null:
      return "null";
   case Kind::Bool:
      return "a boolean";
   case Kind::Number:
      return "an integer";
   case Kind::String:
      return "a string";
   case Kind::Array:
      return "an array";
   case Kind::Object:
      return "an object";
   default:
      return "?";
   }
}

} // namespace capsule::bench::json
