/// @file
/// The battle bench (D-23 section 3.3): a headless host program, linked against one game's generated battle data,
/// that runs a `capsule.bench/1` scenario and writes what happens as JSON lines (output.h): the transcript in the
/// WP-2 golden format (tests/support/battle_transcript.h), and on request every draw from the battle stream, every
/// effect VM host call with its source position, and every raw battle event.
///
///     battle_bench SCENARIO.json [--trace rng,vm,events]     (SCENARIO "-" reads stdin)
///
/// Exit status: 0 after a run (whatever its outcome), 1 for a bad scenario or out-of-date bench data, 2 for bad
/// arguments. Built for desktop and headless only (cmake/parts/bench.cmake, capsule_battle_bench()).
#include "bench_data.h"
#include "output.h"
#include "scenario.h"
#include "trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace capsule::hal::system
{
// Bench build: report the failed assert as an error record and abort.
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   bench::error(std::string("engine assert ") + file + ":" + std::to_string(line) + ": " + expr);
   std::fflush(stdout);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
using namespace capsule::battle;
using namespace capsule::bench;

/// @name Exit statuses
///@{
constexpr int kExitOk = 0;        ///< The scenario ran.
constexpr int kExitBad = 1;       ///< Bad scenario or data.
constexpr int kExitUsage = 2;     ///< Bad arguments.
///@}

/// Bytes read from the scenario file at a time.
constexpr size_t kReadChunk = 4096;

monster::Party g_parties[kMaxParties];   ///< The scenario's party records (static storage).
State g_state;                           ///< The battle (large; static storage).

/// Request kind names (RequestKind order).
constexpr const char* kRequestNames[] = {"Action", "Replace", "SwitchTarget", "LearnMove", "Choice"};
static_assert(sizeof(kRequestNames) / sizeof(kRequestNames[0]) ==
              static_cast<size_t>(RequestKind::NUM_REQUEST_KINDS));

/// Reads a whole file ("-" = stdin).
/// @param path  the file
/// @param out   its text
/// @return false when it cannot be read
bool read_file(const char* path, std::string& out)
{
   const bool in = std::strcmp(path, "-") == 0;
   FILE* f = in ? stdin : std::fopen(path, "rb");
   if (f == nullptr)
   {
      return false;
   }
   char buf[kReadChunk];
   size_t n = 0;
   while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
   {
      out.append(buf, n);
   }
   if (!in)
   {
      std::fclose(f);
   }
   return true;
}

/// Writes a "note" record.
/// @param message  what happened
void note(const std::string& message)
{
   Record("note").str("message", message).num("turn", g_state.turn).write();
}

/// The run: the scenario and where its scripted answers stand.
struct Run
{
   const Scenario& sc;       ///< The scenario.
   size_t next_answer = 0;   ///< First unused answer.
   bool failed = false;      ///< A scripted answer did not fit its request (error written).

   /// Answers an effect request (SwitchTarget, LearnMove, Choice): the next scripted answer, else the default
   /// (the first battler the battle accepts for SwitchTarget, 0 otherwise).
   /// @param s  battle
   /// @param r  the request
   void answer_effect(State& s, const Request& r)
   {
      const char* kind = kRequestNames[static_cast<int>(r.kind)];
      if (next_answer < sc.answers.size())
      {
         const Answer& a = sc.answers[next_answer];
         if (a.kind != r.kind)
         {
            error(std::string("the battle asks for ") + kind + " but the next answer is for " +
                     kRequestNames[static_cast<int>(a.kind)],
                  "answers[" + std::to_string(next_answer) + "]");
            failed = true;
            return;
         }
         ++next_answer;
         int32_t value = a.value;
         if (r.kind == RequestKind::SwitchTarget)
         {
            // The answer names a party member; the engine takes the battler.
            value += s.parties[s.battlers[r.battler].party].first_battler;
         }
         if (choose(s, r.battler, Action{ActionKind::Answer, static_cast<uint8_t>(value), kBench, 0}))
         {
            Record("request").str("kind", kind).num("battler", r.battler).num("answer", value).flag("scripted", true)
               .write();
            return;
         }
         note(std::string("the battle refused the scripted ") + kind + " answer; the default answer is used");
      }
      for (int v = 0; v < (r.kind == RequestKind::SwitchTarget ? s.battler_count : 1); ++v)
      {
         if (choose(s, r.battler, Action{ActionKind::Answer, static_cast<uint8_t>(v), kBench, 0}))
         {
            Record("request").str("kind", kind).num("battler", r.battler).num("answer", v).flag("scripted", false)
               .write();
            return;
         }
      }
      note(std::string("no answer to ") + kind + " was accepted");
   }

   /// Answers one human request (transcript::act() writes refused choices into the transcript, like the goldens).
   /// @param s    battle
   /// @param r    the request
   /// @param out  transcript
   void answer(State& s, const Request& r, std::string& out)
   {
      if (r.kind == RequestKind::Replace)
      {
         transcript::replace(s, r, out);
         return;
      }
      if (r.kind != RequestKind::Action)
      {
         answer_effect(s, r);
         return;
      }
      if (is_trapped(s, r.battler))
      {
         // C5: try_switch hooks keep it from switching out or running this turn (a query: nothing changes)
         Record("trapped").num("turn", s.turn).num("battler", r.battler).write();
      }
      Pick p{transcript::Choice{transcript::ChoiceKind::First, 0, kBench, 0}, kNone16};
      if (!sc.choices.empty())
      {
         // This turn's choices (the last repeats); one per position, or one for everybody.
         const size_t turn = s.turn > 0 ? static_cast<size_t>(s.turn - 1) : 0;
         const TurnPicks& picks = sc.choices[turn < sc.choices.size() ? turn : sc.choices.size() - 1];
         p = picks[r.position < picks.size() ? r.position : picks.size() - 1];
      }
      if (p.move != kNone16)
      {
         // A move by name: its slot in the battler's moves.
         int slot = -1;
         for (int k = 0; k < move_slots() && slot < 0; ++k)
         {
            slot = s.battlers[r.battler].moves[k] == p.move ? k : -1;
         }
         if (slot < 0)
         {
            note("battler " + std::to_string(r.battler) + " does not know the chosen move; the first usable move "
                 "is used");
            p.choice.kind = transcript::ChoiceKind::First;
         }
         else
         {
            p.choice.slot = static_cast<uint8_t>(slot);
         }
      }
      transcript::act(s, r, p.choice, out);
   }
};

/// Turns the --trace argument on in the options.
/// @param list  "rng,vm,events" (any of them)
/// @param t     options
/// @return false on an unknown name
bool parse_trace(const std::string& list, TraceOptions& t)
{
   size_t start = 0;
   while (start <= list.size())
   {
      size_t end = list.find(',', start);
      if (end == std::string::npos)
      {
         end = list.size();
      }
      const std::string w = list.substr(start, end - start);
      if (w == "rng")
      {
         t.rng = true;
      }
      else if (w == "vm")
      {
         t.vm = true;
      }
      else if (w == "events")
      {
         t.events = true;
      }
      else if (!w.empty())
      {
         return false;
      }
      start = end + 1;
   }
   return true;
}

/// Runs a read scenario.
/// @param sc  the scenario
/// @return exit status
int run(const Scenario& sc)
{
   std::string out;
   Record("bench")
      .str("schema", "capsule.bench/1")
      .str("manifest", kBenchData.manifest)
      .str("rng", CAPSULE_BENCH_RNG)
      .str("scenario", sc.name)
      .write();
   trace_start(g_state, sc.trace);
   transcript::put(out, "-- %s\n", sc.name.c_str());
   if (!start(g_state, sc.setup))
   {
      out += "start refused\n";
      flush_lines(out);
      Record("stop").str("reason", "refused").write();
      return kExitOk;
   }
   if (!apply_pokes(g_state, sc))
   {
      return kExitBad;
   }
   State& s = g_state;
   Run r{sc, 0, false};
   const char* reason = "limit";
   bool ended = false;
   int steps = 0;
   const uint8_t extras = sc.owner_texts ? transcript::kOwnerTexts : transcript::kPlainTranscript;
   for (; steps < sc.step_limit && !ended; ++steps)
   {
      transcript::drain(s, out, trace_event, nullptr, extras);
      flush_lines(out);
      if (sc.stop == StopKind::Turns && s.turn > sc.stop_value)
      {
         reason = "turns";
         break;
      }
      if (sc.stop == StopKind::Steps && steps >= sc.stop_value)
      {
         reason = "steps";
         break;
      }
      const Progress p = step(s);
      if (p == Progress::Ended)
      {
         transcript::drain(s, out, trace_event, nullptr, extras);
         ended = true;
         reason = "end";
      }
      else if (p == Progress::Input)
      {
         Request req;
         if (request(s, req))
         {
            r.answer(s, req, out);
            if (r.failed)
            {
               flush_lines(out);
               return kExitBad;
            }
         }
      }
   }
   if (!ended && sc.stop == StopKind::End)
   {
      out += "unfinished\n";
   }
   transcript::summary(s, out);
   flush_lines(out);
   Record("stop")
      .str("reason", reason)
      .num("steps", steps)
      .num("turn", s.turn)
      .str("outcome", transcript::outcome_name(s.outcome))
      .num("draws", draw_count())
      .num("faults", s.vm.faults)
      .num("events_lost", s.events_lost)
      .write();
   return kExitOk;
}

} // namespace

/// Runs one scenario.
/// @param argc  argument count
/// @param argv  scenario path, then optionally --trace LIST
/// @return exit status
int main(int argc, char** argv)
{
   TraceOptions extra;
   const char* path = nullptr;
   for (int k = 1; k < argc; ++k)
   {
      if (std::strcmp(argv[k], "--trace") == 0 && k + 1 < argc)
      {
         if (!parse_trace(argv[++k], extra))
         {
            std::fprintf(stderr, "battle_bench: --trace takes rng, vm and events\n");
            return kExitUsage;
         }
      }
      else if (path == nullptr)
      {
         path = argv[k];
      }
      else
      {
         path = nullptr;
         break;
      }
   }
   if (path == nullptr)
   {
      std::fprintf(stderr, "usage: battle_bench SCENARIO.json|- [--trace rng,vm,events]\n");
      return kExitUsage;
   }

   // The generated tables must describe the linked data (the same compile).
   if (kBenchData.program_count != kData.effects.program_count || kBenchData.owner_count != kData.effects.owner_count)
   {
      error("the bench data does not match the battle data (rebuild the bench)");
      return kExitBad;
   }
   std::string text;
   if (!read_file(path, text))
   {
      error(std::string("cannot read ") + path);
      return kExitBad;
   }
   json::Value doc;
   std::string message;
   if (!json::parse(text, doc, message))
   {
      error(message);
      return kExitBad;
   }
   static Scenario sc;
   if (!load_scenario(doc, g_parties, sc))
   {
      return kExitBad;
   }
   sc.trace.rng = sc.trace.rng || extra.rng;
   sc.trace.vm = sc.trace.vm || extra.vm;
   sc.trace.events = sc.trace.events || extra.events;
   const int status = run(sc);
   std::fflush(stdout);
   return status;
}
