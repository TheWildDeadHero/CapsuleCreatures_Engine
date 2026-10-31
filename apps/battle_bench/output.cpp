/// @file
/// The bench's JSON line output (see output.h).
#include "output.h"

#include "json.h"

#include <cinttypes>
#include <cstdio>

namespace capsule::bench
{

Record::Record(const char* type)
{
   text_ = "{\"t\": ";
   json::put_string(text_, type);
}

void Record::key_of(const char* key)
{
   text_ += ", ";
   json::put_string(text_, key);
   text_ += ": ";
}

Record& Record::str(const char* key, const std::string& value)
{
   key_of(key);
   json::put_string(text_, value);
   return *this;
}

Record& Record::num(const char* key, int64_t value)
{
   key_of(key);
   char buf[sizeof("-9223372036854775808")];
   std::snprintf(buf, sizeof(buf), "%" PRId64, value);
   text_ += buf;
   return *this;
}

Record& Record::flag(const char* key, bool value)
{
   key_of(key);
   text_ += value ? "true" : "false";
   return *this;
}

Record& Record::raw(const char* key, const std::string& json)
{
   key_of(key);
   text_ += json;
   return *this;
}

void Record::write()
{
   text_ += "}\n";
   std::fputs(text_.c_str(), stdout);
}

void error(const std::string& message, const std::string& where)
{
   Record r("error");
   r.str("message", message);
   if (!where.empty())
   {
      r.str("where", where);
   }
   r.write();
}

void flush_lines(std::string& text)
{
   size_t start = 0;
   while (start < text.size())
   {
      size_t end = text.find('\n', start);
      if (end == std::string::npos)
      {
         end = text.size();
      }
      Record("line").str("text", text.substr(start, end - start)).write();
      start = end + 1;
   }
   text.clear();
}

} // namespace capsule::bench
