/// @file
/// The bench's output: one JSON object per line on stdout, each with a "t" (type) member first.
///
/// Types: "bench" (the header), "line" (a transcript line in the WP-2 golden format), "rng" (a draw from the battle
/// stream), "vm" (an effect VM host call: owner, hook, program, pc, source position), "event" (a raw battle event),
/// "request" (an effect request and its answer), "trapped" (C5: a human battler's turn choice while try_switch hooks
/// trap it: turn, battler; capsule::battle::is_trapped()), "note" (something the scenario asked for that could not be done
/// exactly), "stop" (why and where the run stopped) and "error" (a bad scenario or data; the exit status is 1).
#pragma once

#include <cstdint>
#include <string>

namespace capsule::bench
{

/// One output record, built member by member and written as one line.
class Record
{
public:
   /// Starts a record.
   /// @param type  its "t" member
   explicit Record(const char* type);

   /// Adds a string member.
   /// @param key    name
   /// @param value  text (UTF-8)
   /// @return this record
   Record& str(const char* key, const std::string& value);

   /// Adds an integer member.
   /// @param key    name
   /// @param value  the integer
   /// @return this record
   Record& num(const char* key, int64_t value);

   /// Adds a boolean member.
   /// @param key    name
   /// @param value  the flag
   /// @return this record
   Record& flag(const char* key, bool value);

   /// Adds a member whose value is already JSON text (an array or object).
   /// @param key   name
   /// @param json  the JSON text
   /// @return this record
   Record& raw(const char* key, const std::string& json);

   /// Writes the line to stdout.
   void write();

private:
   /// Adds a member's name.
   /// @param key  name
   void key_of(const char* key);

   std::string text_;   ///< The JSON text so far.
};

/// Writes an error record.
/// @param message  what is wrong
/// @param where    where in the scenario ("parties[0].members[1].species"), or empty
void error(const std::string& message, const std::string& where = std::string());

/// Writes every line of a transcript text as "line" records and empties it.
/// @param text  transcript text (lines ending in a newline)
void flush_lines(std::string& text);

} // namespace capsule::bench
