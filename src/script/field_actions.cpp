/// @file
/// Field actions, flight points and the escape point over the script bank (see capsule/script/field_actions.h).
#include "capsule/script/field_actions.h"

#include "capsule/core/memory.h"
#include "capsule/growth/data.h"
#include "capsule/script/menu.h"
#include "capsule/script/script.h"

namespace capsule::script::actions
{

namespace
{

/// The module's state (zero at start-up; EWRAM on the GBA: menus are not speed-critical).
struct State
{
   Available available;   ///< The attached requirement check, or nullptr.
   bool menu_open;        ///< The flight menu waits for an answer.
   menu::Paged page;      ///< Its pages.
};

/// The state.
CAPSULE_EWRAM_BSS State g;

/// The index of the n-th recorded flight point.
/// @param n  0 up
/// @return its point index, or flight_count() when fewer are recorded
uint8_t nth_recorded(int n)
{
   const uint8_t total = flight_count();
   for (uint8_t k = 0; k < total; ++k)
   {
      if (recorded(k) && n-- == 0)
      {
         return k;
      }
   }
   return total;
}

/// Writes a recorded flight point's label as a menu option.
/// @param index  entry (the index-th recorded point)
/// @param out    menu::kOptionCodes codes
void option_text(int index, text::Code* out)
{
   const FlightPointInfo* fp = flight_point(nth_recorded(index));
   int n = 0;
   if (fp != nullptr)
   {
      const text::Code* label = kBank.text + fp->label;
      for (; label[n] != text::kCodeEnd && n < menu::kOptionCodes - 1; ++n)
      {
         out[n] = label[n];
      }
   }
   out[n] = text::kCodeEnd;
}

/// Whether a VarId names a variable of the bank.
/// @param v  the id (kNoVar / kNoScriptId: none)
/// @return true when it does
bool has_var(uint16_t v)
{
   return v != kNoVar && v < kBank.var_count;
}

} // namespace

uint8_t count()
{
   return kBank.field_action_count;
}

const FieldActionInfo* info(uint8_t action)
{
   return action < kBank.field_action_count ? &kBank.field_actions[action] : nullptr;
}

void attach(Available fn)
{
   g.available = fn;
}

uint8_t available(uint8_t action)
{
   if (g.available == nullptr || action >= count())
   {
      return kNoMember;
   }
   return g.available(action);
}

void set_member(uint8_t slot)
{
   if (has_var(kBank.action_member_var))
   {
      set_var(VarId{kBank.action_member_var}, slot);
   }
}

int32_t state(int32_t ref)
{
   if (ref < 0 || ref >= kBank.field_action_ref_count)
   {
      return 0;
   }
   const FieldActionRef& r = kBank.field_action_refs[ref];
   return r.kind == kRefFlag ? (flag(FlagId{r.id}) ? 1 : 0) : var(VarId{r.id});
}

uint8_t flight_count()
{
   return kBank.flight_point_count;
}

const FlightPointInfo* flight_point(uint8_t point)
{
   return point < kBank.flight_point_count ? &kBank.flight_points[point] : nullptr;
}

bool recorded(uint8_t point)
{
   const FlightPointInfo* fp = flight_point(point);
   return fp != nullptr && flag(FlagId{fp->flag});
}

void record(uint8_t point)
{
   const FlightPointInfo* fp = flight_point(point);
   if (fp != nullptr)
   {
      set_flag(FlagId{fp->flag}, true);
   }
}

void menu_open()
{
   int total = 0;
   for (uint8_t k = 0; k < flight_count(); ++k)
   {
      total += recorded(k) ? 1 : 0;
   }
   g.menu_open = total > 0;
   if (g.menu_open)
   {
      menu::open(g.page, total, option_text);
   }
}

int32_t menu_poll()
{
   if (!g.menu_open)
   {
      return kNoFlight;
   }
   const int32_t a = menu::poll(g.page);
   if (a == menu::kWaiting)
   {
      return kFlightWaiting;
   }
   g.menu_open = false;
   return a >= 0 ? nth_recorded(static_cast<int>(a)) : kNoFlight;
}

/// Whether the bank declares the escape point's variables.
/// @param e  the bank's escape variables
/// @return true when all are declared
bool has_escape(const EscapeVars& e)
{
   return has_var(e.map_hi) && has_var(e.map_lo) && has_var(e.x) && has_var(e.y) && has_var(e.facing);
}

void record_escape(AssetKey map, int16_t x, int16_t y, uint8_t facing)
{
   const EscapeVars& e = kBank.escape;
   if (!has_escape(e))
   {
      return;
   }
   // the map's asset key in two u16 halves (0: none; no map has key 0); cells biased into the u16 range
   const uint32_t key = raw(map);
   set_var(VarId{e.map_hi}, static_cast<int32_t>(key >> kEscapeKeyHalfBits));
   set_var(VarId{e.map_lo}, static_cast<int32_t>(key & kEscapeKeyHalfMask));
   set_var(VarId{e.x}, int32_t{x} + kEscapeBias);
   set_var(VarId{e.y}, int32_t{y} + kEscapeBias);
   set_var(VarId{e.facing}, facing);
}

bool escape_point(AssetKey& map, int16_t& x, int16_t& y, uint8_t& facing)
{
   const EscapeVars& e = kBank.escape;
   if (!has_escape(e))
   {
      return false;
   }
   const uint32_t key = (static_cast<uint32_t>(var(VarId{e.map_hi})) << kEscapeKeyHalfBits) |
                        static_cast<uint32_t>(var(VarId{e.map_lo}));
   if (key == 0)
   {
      return false;   // none recorded
   }
   map = AssetKey{key};
   x = static_cast<int16_t>(var(VarId{e.x}) - kEscapeBias);
   y = static_cast<int16_t>(var(VarId{e.y}) - kEscapeBias);
   facing = static_cast<uint8_t>(var(VarId{e.facing}));
   return true;
}

} // namespace capsule::script::actions

namespace capsule::rules
{

int32_t field_state(int32_t ref)
{
   return script::actions::state(ref);
}

} // namespace capsule::rules
