/// @file
/// The engine's default growth script port (capsule/script/growth_port.h, G4): the statement
/// `transform(<slot>) -> var(v);` runs a growth::Change (trigger script) over the attached State and party; its
/// question goes through ask_open() / ask_poll() (src/script/script_growth_ask.cpp) and a move offer through the
/// items port's learn question. A game replaces this file's functions by defining them.
#include "capsule/script/growth_port.h"

#include "capsule/core/memory.h"
#include "capsule/growth/growth.h"
#include "capsule/script/port.h"

#include <new>

namespace capsule::script::growth
{

namespace
{

namespace gr = capsule::growth;

/// Where the running statement stands.
enum class Step : uint8_t
{
   Idle,       ///< Nothing runs.
   Asking,     ///< The question is open.
   Learning,   ///< A move offer's question is open.
   Finished,   ///< Over: the next poll answers the result.
};

/// The defaults' state: the attached State and party and the running Change, built in place (no heap: a Change
/// is a scope object over the State, it lives from transform_open() until the statement's result is known).
struct Port
{
   battle::State* lent;                                        ///< attach()'s State.
   monster::Party* party;                                      ///< attach()'s party.
   alignas(gr::Change) unsigned char change[sizeof(gr::Change)];   ///< The running Change's storage.
   bool live;                                                  ///< A Change lives in change.
   Step step;                                                  ///< Where the statement stands.
   uint8_t slot;                                               ///< The member.
   int16_t result;                                             ///< The result once Finished.
};

/// The state (zero at start-up; EWRAM on the GBA: not speed-critical).
CAPSULE_EWRAM_BSS Port g;

/// The running Change.
/// @return it (g.live must be true)
gr::Change& running()
{
   return *std::launder(reinterpret_cast<gr::Change*>(g.change));
}

/// Ends the running Change (its destructor writes the member back) and keeps the result.
/// @param result  the statement's result
void finish(int16_t result)
{
   if (g.live)
   {
      running().~Change();
      g.live = false;
   }
   g.result = result;
   g.step = Step::Finished;
}

/// After go(): the move offer's question, or the end.
/// @param stage  what go() or answer_learn() returned
void after_apply(gr::Stage stage)
{
   if (stage == gr::Stage::Learning)
   {
      items::learn_open(g.slot, running().offered_move());
      g.step = Step::Learning;
      return;
   }
   finish(kTransformChanged);
}

} // namespace

void attach(battle::State* lent, monster::Party* party)
{
   g.lent = lent;
   g.party = party;
}

void transform_open(uint8_t slot)
{
   if (g.live)
   {
      finish(kTransformNone);   // a statement left running: it ends here
   }
   g.result = kTransformNone;
   g.step = Step::Finished;
   if (g.lent == nullptr || g.party == nullptr || slot >= g.party->count)
   {
      return;
   }
   ::new (static_cast<void*>(g.change)) gr::Change(*g.lent, *g.party, slot, gr::make_context(gr::Trigger::Script));
   g.live = true;
   g.slot = slot;
   gr::Change& c = running();
   if (c.stage != gr::Stage::Asking)
   {
      finish(kTransformNone);
      return;
   }
   if (c.found.kind == gr::ChangeKind::Form)
   {
      c.go();   // a form change asks nothing
      finish(kTransformChanged);
      return;
   }
   ask_open(slot, c.from, c.found.into, c.cancellable());
   g.step = Step::Asking;
}

int16_t transform_poll()
{
   switch (g.step)
   {
   case Step::Asking:
   {
      const int8_t a = ask_poll();
      if (a == kAskWaiting)
      {
         return kTransformRunning;
      }
      if (a == kAskStop && running().stop())
      {
         finish(kTransformStopped);
         break;
      }
      after_apply(running().go());
      return g.step == Step::Finished ? g.result : kTransformRunning;
   }
   case Step::Learning:
   {
      const int16_t a = items::learn_poll();
      if (a < 0)
      {
         return kTransformRunning;
      }
      after_apply(running().answer_learn(static_cast<uint8_t>(a)));
      return g.step == Step::Finished ? g.result : kTransformRunning;
   }
   default:
      break;
   }
   g.step = Step::Idle;
   return g.result;
}

} // namespace capsule::script::growth
