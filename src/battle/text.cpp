/// @file
/// Battle texts: format_text() fills message placeholders (field placeholders pass through to the printer).
/// See capsule/battle/battle.h; the modules share internal.h.
#include "internal.h"

namespace capsule::battle::detail
{

/// @name Text
///@{
constexpr int kMinTextCap = 2;                ///< Smallest output buffer: one code and the end.
constexpr int kMaxDigits = 10;                ///< Decimal digits of a uint32_t.
constexpr int kPlaceholderCodes = 2;          ///< A kept placeholder: kCodePlaceholder and its argument.
constexpr int kEscapeCodes = 2;               ///< A control escape: kCodeControl and its argument.
/// Powers of ten of a uint32_t's digits, highest first: numbers print by subtraction (no division: software
/// division on the ARM7TDMI; capsule/core/random.h keeps the same rule).
constexpr uint32_t kPowersOfTen[kMaxDigits] = {1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
                                               10000u, 1000u, 100u, 10u, 1u};
///@}

}   // namespace capsule::battle::detail

// -- public ------------------------------------------------------------------------------------------------

namespace capsule::battle
{

using namespace detail;

bool format_text(const State& s, const Event& e, text::Code* out, int cap)
{
   const text::Code* src = nullptr;
   if (e.kind == EventKind::Message && e.a < data().message_count)
   {
      src = data().messages[e.a];
   }
   else if (e.kind == EventKind::StatusText && e.a < data().status_count)
   {
      const StatusData& sd = data().statuses[e.a];
      const text::Code* const texts[kStatusTexts] = {sd.inflicted, sd.residual, sd.skip, sd.cured, sd.already};
      src = e.extra < kStatusTexts ? texts[e.extra] : nullptr;
   }
   else if (e.kind == EventKind::WeatherText && e.a < data().weather_count)
   {
      const WeatherData& w = data().weathers[e.a];
      const text::Code* const texts[kWeatherTexts] = {w.started, w.continues, w.ended, w.hurts};
      src = e.extra < kWeatherTexts ? texts[e.extra] : nullptr;
   }
   else if (e.kind == EventKind::Text && e.a < data().text_count)
   {
      src = data().texts[e.a];   // an owner text (effect `text`)
   }
   else if (e.kind == EventKind::PresenterText && e.a < data().presenter_count)
   {
      const PresenterData& pd = data().presenters[e.a];
      const text::Code* const texts[kPresenterTexts] = {pd.intro, pd.defeat, pd.victory};
      src = e.extra < kPresenterTexts ? texts[e.extra] : nullptr;
   }
   if (src == nullptr || cap < kMinTextCap)
   {
      return false;
   }
   int n = 0;
   auto put = [&](const text::Code* t)
   {
      for (; t != nullptr && *t != text::kCodeEnd && n < cap - 1; ++t)
      {
         out[n++] = *t;
      }
   };
   auto put_number = [&](uint32_t v)
   {
      bool started = false;   // no leading zeros; the last digit always prints
      for (int k = 0; k < kMaxDigits && n < cap - 1; ++k)
      {
         const uint32_t power = kPowersOfTen[k];
         uint8_t digit = 0;
         while (v >= power)
         {   // at most nine subtractions per digit
            v -= power;
            ++digit;
         }
         started = started || digit != 0 || k == kMaxDigits - 1;
         if (started)
         {
            out[n++] = static_cast<text::Code>(text::kCodeDigitZero + digit);
         }
      }
   };
   auto battler_name = [&](uint8_t i) -> const text::Code*
   {
      return i < s.battler_count ? s.battlers[i].name : nullptr;
   };
   for (; *src != text::kCodeEnd && n < cap - 1; ++src)
   {
      if (*src == text::kCodeControl)
      {   // a control escape (FC nn) is copied for the printer as one unit: its argument is never read as a code
         const int len = text::escape_length(src);   // I4: with its argument bytes
         if (len < kEscapeCodes || n + len >= cap)
         {
            break;   // a cut escape (or no room for it): the text ends before it
         }
         for (int i = 0; i < len - 1; ++i)
         {
            out[n++] = *src++;
         }
         out[n++] = *src;
         continue;
      }
      if (*src != text::kCodePlaceholder)
      {   // glyphs and newlines are copied for the printer
         out[n++] = *src;
         continue;
      }
      // FD nn: placeholder nn (text::Arg) becomes its text
      if (src[1] == text::kCodeEnd)
      {
         break;
      }
      ++src;
      switch (static_cast<text::Arg>(*src))
      {
      case text::Arg::User:
         put(battler_name(e.battler));
         break;
      case text::Arg::Target:
         put(battler_name(e.target < s.battler_count ? e.target : e.battler));
         break;
      case text::Arg::Move:
         put(e.b < data().move_count ? data().moves[e.b].name : nullptr);
         break;
      case text::Arg::Value:
         put_number(e.b);
         break;
      case text::Arg::Stat:
         put(e.extra < data().stage_count ? data().stages[e.extra].name : nullptr);
         break;
      case text::Arg::Party:   // b = party index (Message and PresenterText events)
         put(e.b < s.party_count ? s.parties[e.b].name : nullptr);
         break;
      case text::Arg::Passive:
         put(e.b < data().passive_count ? data().passives[e.b].name : nullptr);
         break;
      case text::Arg::Item:
         put(e.b < data().item_count ? data().items[e.b].name : nullptr);
         break;
      case text::Arg::Status:
         put(e.a < data().status_count ? data().statuses[e.a].name : nullptr);
         break;
      case text::Arg::NUM_ARGS:
      default:
         if (text::is_field_arg(*src) && n + kPlaceholderCodes < cap)
         {   // a field placeholder ({player}): kept for the printer's field resolver
            out[n++] = text::kCodePlaceholder;
            out[n++] = *src;
         }
         break;   // any other argument this engine does not know: nothing
      }
   }
   out[n] = text::kCodeEnd;
   return true;
}

}   // namespace capsule::battle
