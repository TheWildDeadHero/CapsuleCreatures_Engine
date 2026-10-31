/// @file
/// Effect VM (D-22 decision 1, D-23 section 1): the bytecode that move,
/// passive, item, status, volatile, weather and side / field condition logic
/// compiles to, and the interpreter that runs it.
///
/// tools/effect_compiler.py compiles `.ceffect` sources to a Bank. The VM is
/// standalone: everything it reads, writes, draws or does goes through the
/// **host functions** declared at the end of this file. They are plain free
/// functions implemented by the embedder (the battle engine; a mock in
/// tests/effect_vm_tests.cpp) and resolved at link time, like the other
/// replaceable defaults (battle_empty.cpp, script_empty.cpp). The embedder
/// defines the opaque HostState (its battle state) and PointCtx (the
/// pipeline context the generated point handlers read).
///
/// **Values** are int32. Add, subtract, multiply, shifts and negation
/// saturate; division and remainder by 0 give 0; comparisons give 0 or 1. Q8
/// numbers appear only as literal arguments (1.5 = 384), used by MulQ8 and
/// Modify `by`.
///
/// **Machine.** A stack machine with kEffectStack operand slots and
/// kEffectLocals locals per run, sub calls up to kEffectCallDepth deep, and
/// nested runs (hooks a primitive triggers) up to kMaxEffectDepth deep on
/// Machine::frames. Jumps go forward only; the two back-edges are Loop (a
/// counter in a local, bounded by the compiler's `max`) and Next (a battler
/// bit mask, at most kMaxBattlers rounds). Every limit is checked at run time
/// as well: a broken program ends with Status::Fault and Machine::faults
/// counts it; nothing crashes. kEffectOpsPerRun bounds one resume() or
/// invoke() as a backstop (ENGINE_ASSERT in debug builds, and a fault).
///
/// **Suspension.** Only top-level runs (begin() with resumable set: a move's
/// use / hit / after_move phases, condition residual phases) may stop in the
/// middle: Yield returns Status::Yielded, and Ask returns Status::Asked when
/// the host cannot answer at once (answer() pushes the answer and goes on).
/// The whole state is the plain EffectRun, so it can live in the battle
/// state and be copied with it.
///
/// **Bytecode** (little-endian; branch targets are 16-bit offsets from the
/// first byte of their own program, so programs are relocatable). The
/// opcode is one byte, its immediates follow; "pops" lists the operands
/// taken from the stack in push order:
///
/// | Op | Immediates | Pops | Pushes | Does |
/// |---|---|---|---|---|
/// | End | — | | | ends the run |
/// | Push8 / Push16 / Push32 | signed value | | v | constant |
/// | Dup / Drop / Swap | — | a / a / a b | a a / — / b a | |
/// | Load / Store | local8 | — / v | v / — | locals |
/// | Add Sub Mul Div Mod Min Max Shl Shr MulQ8 | — | a b | r | saturating; x / 0 = x % 0 = 0 |
/// | Neg Abs Not | — | a | r | |
/// | Clamp | — | v lo hi | r | max(lo, min(hi, v)) |
/// | Pick | n8 | i v0 .. vn-1 | v[clamp(i, 0, n - 1)] | table select |
/// | Eq Ne Lt Le Gt Ge | — | a b | 0 / 1 | |
/// | Jmp / Jz / Jnz | off16 | — / c / c | | forward only |
/// | Loop | local8 off16 | | | --local; back to off16 (at or before the Loop) while > 0 |
/// | ForEach | set8 subj8 local8 off16 | | | first battler of the set into local, the rest into local + 1; none: to off16 |
/// | Next | local8 off16 | | | next battler of local + 1 into local and back to off16 (at or before it); none left: falls through |
/// | Call | program16 | | | runs a sub |
/// | Ret | — | | | back to the caller (the outermost level ends) |
/// | Yield | — | | | suspends a top-level run |
/// | Stop / Fail | — | | | ends with Status::Stopped / Status::Failed |
/// | Get | ref8 subj8 arg8 | | v | a read (Ref) |
/// | Has | kind8 subj8 id8 | | 0 / 1 | status, volatile, tag, type, move flag, side / field condition |
/// | Subj | subj8 | | battler | a subject's battler index |
/// | RoleGet | subj8 role8 elem8 | | v | a record role of a battler's party record |
/// | Set | ref8 subj8 arg8 | v | | counters, words, status counter / scratch |
/// | RoleSet | subj8 role8 elem8 | v | | |
/// | Rand / Between / Chance | — | n / lo hi / n m | r | draws (host), in execution order |
/// | Damage | — | | dealt | standard hit on the target |
/// | DamageHp | subj8 flags8 | amount | dealt | DamageFlag |
/// | SelfHit | subj8 | power | | the self-hit pipeline |
/// | Heal | subj8 | amount | | |
/// | Inflict | subj8 status8 flags8 | | ok | |
/// | Cure | subj8 flags8 | | | kCureQuiet |
/// | Stage | subj8 stage8 | delta | applied | |
/// | VolOn / VolOff | subj8 vol8 | turns / — | | condition-own try_start, start / already, end hooks |
/// | SideOn / SideOff | subj8 cond8 flags8 / subj8 cond8 | turns / — | | the subject's side; kSideAddLayer |
/// | FieldOn / FieldOff | cond8 | turns / — | | |
/// | Weather | weather8 | turns | | |
/// | Switch | subj8 mode8 | replacement | | SwitchMode |
/// | Lock / Unlock | subj8 flags8 / subj8 | | | next action = this move; kLockFree |
/// | UseMove | subj8 | move | | queues a chained move |
/// | SetTarget / Credit | — | battler / amount | | |
/// | ConsumeItem | subj8 | | | |
/// | SetItem / SetPassive | subj8 | id | | |
/// | SetTypes | subj8 n8 | n types | | |
/// | Modify | mode8 | arg | | ctx.value by q8 / add / set |
/// | Immune | — | heal q8 | | ends with Status::Immune |
/// | Block | text16 | | | ends with Status::Blocked (kNoText: silent) |
/// | Text / Say / Announce / Cue | text16 / msg8 / — / cue8 | | | presentation |
/// | Ask | kind8 subj8 | | answer | a request (AskKind) |
/// | SayAt | msg8 subj8 subj8 | value | | an engine message about two subjects with a value |
/// | Restore | subj8 res8 flags8 | amount | restored | a resource up to its maximum (kResourceFull: to it) |
/// | Raise | subj8 res8 flags8 | delta | applied | a move resource's raises (its maximum and pool follow) |
/// | Odds | — | n m by on | 0 / 1 | effect odds: rules::effect_odds() for battler by against on (host, E9) |
/// | ResetStages | subj8 | | changed | every stage back to 0, no stage_change hooks, no texts (E13b) |
/// | Delay | subj8 | amount turns | ok | the move's pending effect at the subject's position (its land hook, E13b) |
/// | Reward | — | amount | | adds to State::reward (E13b) |
/// | EndBattle | — | | | the battle ends at once, Outcome::Escaped; the run stops (E13b) |
/// | Teach | subj8 | move | 1 / 2 | learned (1) or offered in State::learn (2); fails when known or not takeable (G2) |
/// | CallMove | subj8 flag8 | move | | UseMove that refuses a move with move flag flag8 (or none): the run fails (G1) |
/// | DelayHit | subj8 hit8 | turns | ok | a delayed hit (BattleData::delayed_hits) with a snapshot of the user (G1) |
/// | Absorb | — | | | the hit's before_damage: a decoy took it (MoveCursor CURSOR_ABSORBED, ref `absorbed`; G1) |
/// | Recruit | subj8 | bonus | 1 / 0 | the battle's Item action only: the recruit checks (capsule/battle/data/recruit.h); 1: recruited (the battle ends, Outcome::Recruited), 0: it broke free; refused (owned, no room, SETUP_NO_RECRUIT): the run fails (G3) |
/// | Form | subj8 form8 | | 1 / 0 | the subject's form by name id (growth FORM_<NAME>; 0: its species' own); 0: the same form or none of that name; battle only in battle, kept by a field context's member (G4) |
/// | Transform | subj8 | | 1 | a field context's member: the item_use rules of the running item (capsule/growth/growth.h); none holds (or in battle): the run fails (G4) |
/// | Train | subj8 stat8 | amount | applied | the subject's training value of a stat within the caps (structure training); stats follow (G4) |
/// | Encounter | kind8 id8 | a b c | | an encounter hook's change to the roll (EncounterOp; capsule/battle/encounter_lead.h): outside an encounter or encounter_made run the run fails (F1) |
/// | FieldEffect | effect8 | amount | 1 | starts a timed field effect by its stack rule (capsule/script/field_effects.h); refused: the run fails (F1) |
/// | Inherit | subj8 role8 mode8 | a b | changed | the combine hook's inheritance into the result's record (InheritOp; subj8 = the input or kSubjNone); outside a combination the run fails (B1) |
///
/// Restore and Raise reach a battler resource, or the context move's slot of a move resource
/// (kResourceEverySlot: every slot); Ref::Resource reads and writes the same pool (D-25).
///
/// Stage's id8 holds the stage in its low bits, kStageQuiet and kStageSilent; a primitive's
/// host may end the run (Outcome::stop: Status::Stopped, e.g. a hit on an
/// immune target ends the move's work on it; Outcome::fail: Status::Failed,
/// e.g. a condition whose try_start hook failed).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/core/pod.h"

#include <cstdint>

namespace capsule::battle::effect
{

/// @name Limits (D-23 section 1.2)
///@{
inline constexpr int kEffectStack = 16;         ///< Operand stack slots per run.
inline constexpr int kEffectLocals = 8;         ///< Locals per run.
inline constexpr int kEffectCallDepth = 4;      ///< Nested sub calls per run.
inline constexpr int kMaxEffectDepth = 4;       ///< Nested runs (a primitive's hooks run inside it).
inline constexpr int kEffectOpsPerRun = 2048;   ///< Instructions one resume() or invoke() may execute.
inline constexpr int kHookEventBudget = 32;     ///< Worst-case events of one hook program (compiler-checked).
inline constexpr int kMaxRepeat = 16;           ///< Highest `repeat ... max N`.
inline constexpr int kMaxActionArgs = 3;        ///< Operands a primitive takes from the stack.
///@}

/// No text (Block without a message).
inline constexpr uint16_t kNoText = 0xFFFF;
/// The answer is not known yet (Switch: the host picks the replacement).
inline constexpr int32_t kHostPicks = -1;

/// Opcodes (see the file comment for the immediates and stack effects).
enum class Op : uint8_t
{
   End,           ///< End the run.
   Push8,         ///< Push a signed 8-bit constant.
   Push16,        ///< Push a signed 16-bit constant.
   Push32,        ///< Push a 32-bit constant.
   Dup,           ///< Duplicate the top.
   Drop,          ///< Drop the top.
   Swap,          ///< Swap the top two.
   Load,          ///< Push a local.
   Store,         ///< Pop into a local.
   Add,           ///< a + b.
   Sub,           ///< a - b.
   Mul,           ///< a * b.
   Div,           ///< a / b (0 when b is 0).
   Mod,           ///< a % b (0 when b is 0).
   Neg,           ///< -a.
   Abs,           ///< |a|.
   Min,           ///< min(a, b).
   Max,           ///< max(a, b).
   Clamp,         ///< max(lo, min(hi, v)).
   MulQ8,         ///< (a * b) >> 8.
   Shl,           ///< a << b (b clamped to 0-31).
   Shr,           ///< a >> b (arithmetic; b clamped to 0-31).
   Pick,          ///< Table select.
   Eq,            ///< a == b.
   Ne,            ///< a != b.
   Lt,            ///< a < b.
   Le,            ///< a <= b.
   Gt,            ///< a > b.
   Ge,            ///< a >= b.
   Not,           ///< a == 0.
   Jmp,           ///< Jump forward.
   Jz,            ///< Jump forward when the popped value is 0.
   Jnz,           ///< Jump forward when the popped value is not 0.
   Loop,          ///< Bounded back-edge (repeat).
   ForEach,       ///< Start a battler loop.
   Next,          ///< Next round of a battler loop.
   Call,          ///< Call a sub.
   Ret,           ///< Return from a sub.
   Yield,         ///< Suspend (top-level runs only).
   Stop,          ///< End: the action / handler stops.
   Fail,          ///< End: the move fails.
   Get,           ///< Read.
   Has,           ///< Membership test.
   Subj,          ///< Battler index of a subject.
   RoleGet,       ///< Read a record role.
   Set,           ///< Write.
   RoleSet,       ///< Write a record role.
   Rand,          ///< 0 to n - 1.
   Between,       ///< lo to hi.
   Chance,        ///< n in m.
   Damage,        ///< Standard hit.
   DamageHp,      ///< Direct HP damage.
   SelfHit,       ///< Self-hit pipeline.
   Heal,          ///< Restore HP.
   Inflict,       ///< Give a primary status.
   Cure,          ///< Remove the primary status.
   Stage,         ///< Change a stage.
   VolOn,         ///< Start a volatile.
   VolOff,        ///< End a volatile.
   SideOn,        ///< Start (or layer) a side condition.
   SideOff,       ///< End a side condition.
   FieldOn,       ///< Start a field condition.
   FieldOff,      ///< End a field condition.
   Weather,       ///< Set the weather.
   Switch,        ///< Switch a battler out.
   Lock,          ///< Lock the next action to this move.
   Unlock,        ///< Drop a lock.
   UseMove,       ///< Queue a chained move.
   SetTarget,     ///< Choose the move's target (target phase).
   Credit,        ///< Add to the move's dealt damage.
   ConsumeItem,   ///< Use up the held item.
   SetItem,       ///< Replace the held item.
   SetPassive,    ///< Replace the passive.
   SetTypes,      ///< Replace the types.
   Modify,        ///< Change ctx.value.
   Immune,        ///< Verdict: immune (heals by a q8 share of max HP).
   Block,         ///< Verdict: blocked.
   Text,          ///< Show an owner text.
   Say,           ///< Show an engine message.
   Announce,      ///< Announce the owner.
   Cue,           ///< Play an animation cue.
   Ask,           ///< Request an answer.
   SayAt,         ///< Show an engine message about two subjects with a value.
   Restore,       ///< Refill a resource (D-25).
   Raise,         ///< Raise a move resource's raises (D-25).
   Odds,          ///< n in m effect odds of one battler against another (the ruleset's effect_odds, E9).
   ResetStages,   ///< Every stage of a battler back to 0, silently (E13b; pushes the stages it changed).
   Delay,         ///< Pending effect of the move at a battler's position after some turns (E13b; pushes ok).
   Reward,        ///< Add to the battle's reward pool (E13b).
   EndBattle,     ///< End the battle at once: nobody wins (Outcome::Escaped; E13b).
   Teach,         ///< Offer a move as a level-up does (an item teaching a move; G2).
   CallMove,      ///< Queue a chained move unless it has a move flag (the run fails; G1).
   DelayHit,      ///< A delayed hit at a battler's position: the user's snapshot, landing through `damage` (G1).
   Absorb,        ///< Mark the hit in progress as taken by a decoy (before_damage; G1).
   Recruit,       ///< Try to recruit a battler: the recruit checks; success ends the battle (use_battle; G3).
   Form,          ///< Change a battler's form (G4).
   Transform,     ///< Apply the item_use transformation rules to a field context's member (G4).
   Train,         ///< Change a training value within the caps (G4).
   Encounter,     ///< Change the encounter roll in progress (an encounter hook; EncounterOp; F1).
   FieldEffect,   ///< Start a timed field effect (F1).
   Inherit,       ///< Inherit into a combination's result record (the combine hook; InheritOp; B1).
   NUM_OPS        ///< Number of opcodes.
};
/// Number of opcodes.
inline constexpr int kOpCount = static_cast<int>(Op::NUM_OPS);

/// How a primitive's immediates read (the generic Action decode).
enum class Imm : uint8_t
{
   None,          ///< No immediates (or decoded by the VM itself).
   Subj,          ///< subj8.
   SubjId,        ///< subj8, id8.
   SubjIdFlags,   ///< subj8, id8, flags8.
   SubjFlags,     ///< subj8, flags8.
   SubjCount,     ///< subj8, n8 (n operands).
   Id,            ///< id8.
   Text,          ///< id16.
   IdSubj,        ///< id8, subj8.
   IdSubjSubj,    ///< id8, subj8, subj8 (the second subject goes to Action::about).
   IdFlags,       ///< id8, flags8 (F1: Encounter's kind and its type or species field).
   NUM_IMMS       ///< Number of layouts.
};

/// An opcode's shape.
struct OpInfo
{
   uint8_t size;     ///< Immediate bytes after the opcode.
   uint8_t pops;     ///< Operands popped (Pick: plus n + 1, SetTypes: plus n).
   uint8_t pushes;   ///< Values pushed.
   Imm imm;          ///< Primitive decode (Imm::None: the VM decodes it).
};
CAPSULE_POD(OpInfo, 4);

/// @name Immediate sizes
///@{
inline constexpr uint8_t kImm0 = 0;   ///< No immediate.
inline constexpr uint8_t kImm1 = 1;   ///< One byte.
inline constexpr uint8_t kImm2 = 2;   ///< Two bytes.
inline constexpr uint8_t kImm3 = 3;   ///< Three bytes.
inline constexpr uint8_t kImm4 = 4;   ///< Four bytes.
inline constexpr uint8_t kImm5 = 5;   ///< Five bytes (ForEach).
///@}

/// @name Stack effects
///@{
inline constexpr uint8_t kPop0 = 0;   ///< Pops nothing.
inline constexpr uint8_t kPop1 = 1;   ///< Pops one.
inline constexpr uint8_t kPop2 = 2;   ///< Pops two.
inline constexpr uint8_t kPop3 = 3;   ///< Pops three.
inline constexpr uint8_t kPop4 = 4;   ///< Pops four.
inline constexpr uint8_t kPush0 = 0;  ///< Pushes nothing.
inline constexpr uint8_t kPush1 = 1;  ///< Pushes one.
inline constexpr uint8_t kPush2 = 2;  ///< Pushes two (Dup, Swap).
///@}

/// Shape of every opcode, in Op order (tools/effect_compiler.py OPS mirrors it).
inline constexpr OpInfo kOpInfo[kOpCount] = {
   {kImm0, kPop0, kPush0, Imm::None},          // End
   {kImm1, kPop0, kPush1, Imm::None},          // Push8
   {kImm2, kPop0, kPush1, Imm::None},          // Push16
   {kImm4, kPop0, kPush1, Imm::None},          // Push32
   {kImm0, kPop1, kPush2, Imm::None},          // Dup
   {kImm0, kPop1, kPush0, Imm::None},          // Drop
   {kImm0, kPop2, kPush2, Imm::None},          // Swap
   {kImm1, kPop0, kPush1, Imm::None},          // Load
   {kImm1, kPop1, kPush0, Imm::None},          // Store
   {kImm0, kPop2, kPush1, Imm::None},          // Add
   {kImm0, kPop2, kPush1, Imm::None},          // Sub
   {kImm0, kPop2, kPush1, Imm::None},          // Mul
   {kImm0, kPop2, kPush1, Imm::None},          // Div
   {kImm0, kPop2, kPush1, Imm::None},          // Mod
   {kImm0, kPop1, kPush1, Imm::None},          // Neg
   {kImm0, kPop1, kPush1, Imm::None},          // Abs
   {kImm0, kPop2, kPush1, Imm::None},          // Min
   {kImm0, kPop2, kPush1, Imm::None},          // Max
   {kImm0, kPop3, kPush1, Imm::None},          // Clamp
   {kImm0, kPop2, kPush1, Imm::None},          // MulQ8
   {kImm0, kPop2, kPush1, Imm::None},          // Shl
   {kImm0, kPop2, kPush1, Imm::None},          // Shr
   {kImm1, kPop1, kPush1, Imm::None},          // Pick (+ n)
   {kImm0, kPop2, kPush1, Imm::None},          // Eq
   {kImm0, kPop2, kPush1, Imm::None},          // Ne
   {kImm0, kPop2, kPush1, Imm::None},          // Lt
   {kImm0, kPop2, kPush1, Imm::None},          // Le
   {kImm0, kPop2, kPush1, Imm::None},          // Gt
   {kImm0, kPop2, kPush1, Imm::None},          // Ge
   {kImm0, kPop1, kPush1, Imm::None},          // Not
   {kImm2, kPop0, kPush0, Imm::None},          // Jmp
   {kImm2, kPop1, kPush0, Imm::None},          // Jz
   {kImm2, kPop1, kPush0, Imm::None},          // Jnz
   {kImm3, kPop0, kPush0, Imm::None},          // Loop
   {kImm5, kPop0, kPush0, Imm::None},          // ForEach
   {kImm3, kPop0, kPush0, Imm::None},          // Next
   {kImm2, kPop0, kPush0, Imm::None},          // Call
   {kImm0, kPop0, kPush0, Imm::None},          // Ret
   {kImm0, kPop0, kPush0, Imm::None},          // Yield
   {kImm0, kPop0, kPush0, Imm::None},          // Stop
   {kImm0, kPop0, kPush0, Imm::None},          // Fail
   {kImm3, kPop0, kPush1, Imm::None},          // Get
   {kImm3, kPop0, kPush1, Imm::None},          // Has
   {kImm1, kPop0, kPush1, Imm::None},          // Subj
   {kImm3, kPop0, kPush1, Imm::None},          // RoleGet
   {kImm3, kPop1, kPush0, Imm::None},          // Set
   {kImm3, kPop1, kPush0, Imm::None},          // RoleSet
   {kImm0, kPop1, kPush1, Imm::None},          // Rand
   {kImm0, kPop2, kPush1, Imm::None},          // Between
   {kImm0, kPop2, kPush1, Imm::None},          // Chance
   {kImm0, kPop0, kPush1, Imm::None},          // Damage
   {kImm2, kPop1, kPush1, Imm::SubjFlags},     // DamageHp
   {kImm1, kPop1, kPush0, Imm::Subj},          // SelfHit
   {kImm1, kPop1, kPush0, Imm::Subj},          // Heal
   {kImm3, kPop0, kPush1, Imm::SubjIdFlags},   // Inflict
   {kImm2, kPop0, kPush0, Imm::SubjFlags},     // Cure
   {kImm2, kPop1, kPush1, Imm::SubjId},        // Stage
   {kImm2, kPop1, kPush0, Imm::SubjId},        // VolOn
   {kImm2, kPop0, kPush0, Imm::SubjId},        // VolOff
   {kImm3, kPop1, kPush0, Imm::SubjIdFlags},   // SideOn
   {kImm2, kPop0, kPush0, Imm::SubjId},        // SideOff
   {kImm1, kPop1, kPush0, Imm::Id},            // FieldOn
   {kImm1, kPop0, kPush0, Imm::Id},            // FieldOff
   {kImm1, kPop1, kPush0, Imm::Id},            // Weather
   {kImm2, kPop1, kPush0, Imm::SubjId},        // Switch (id = mode)
   {kImm2, kPop0, kPush0, Imm::SubjFlags},     // Lock
   {kImm1, kPop0, kPush0, Imm::Subj},          // Unlock
   {kImm1, kPop1, kPush0, Imm::Subj},          // UseMove
   {kImm0, kPop1, kPush0, Imm::None},          // SetTarget
   {kImm0, kPop1, kPush0, Imm::None},          // Credit
   {kImm1, kPop0, kPush0, Imm::Subj},          // ConsumeItem
   {kImm1, kPop1, kPush0, Imm::Subj},          // SetItem
   {kImm1, kPop1, kPush0, Imm::Subj},          // SetPassive
   {kImm2, kPop0, kPush0, Imm::SubjCount},     // SetTypes (+ n)
   {kImm1, kPop1, kPush0, Imm::None},          // Modify
   {kImm0, kPop1, kPush0, Imm::None},          // Immune
   {kImm2, kPop0, kPush0, Imm::Text},          // Block
   {kImm2, kPop0, kPush0, Imm::Text},          // Text
   {kImm1, kPop0, kPush0, Imm::Id},            // Say
   {kImm0, kPop0, kPush0, Imm::None},          // Announce
   {kImm1, kPop0, kPush0, Imm::Id},            // Cue
   {kImm2, kPop0, kPush1, Imm::IdSubj},        // Ask
   {kImm3, kPop1, kPush0, Imm::IdSubjSubj},    // SayAt
   {kImm3, kPop1, kPush1, Imm::SubjIdFlags},   // Restore
   {kImm3, kPop1, kPush1, Imm::SubjIdFlags},   // Raise
   {kImm0, kPop4, kPush1, Imm::None},          // Odds
   {kImm1, kPop0, kPush1, Imm::Subj},          // ResetStages
   {kImm1, kPop2, kPush1, Imm::Subj},          // Delay (amount, turns)
   {kImm0, kPop1, kPush0, Imm::None},          // Reward
   {kImm0, kPop0, kPush0, Imm::None},          // EndBattle
   {kImm1, kPop1, kPush1, Imm::Subj},          // Teach (move)
   {kImm2, kPop1, kPush0, Imm::SubjId},        // CallMove (id = the refusing move flag)
   {kImm2, kPop1, kPush1, Imm::SubjId},        // DelayHit (id = the delayed hit; turns)
   {kImm0, kPop0, kPush0, Imm::None},          // Absorb
   {kImm1, kPop1, kPush1, Imm::Subj},          // Recruit (bonus)
   {kImm2, kPop0, kPush1, Imm::SubjId},        // Form (id = the form name)
   {kImm1, kPop0, kPush1, Imm::Subj},          // Transform
   {kImm2, kPop1, kPush1, Imm::SubjId},        // Train (id = the stat; amount)
   {kImm2, kPop3, kPush0, Imm::IdFlags},       // Encounter (id = EncounterOp, flags = type / species field)
   {kImm1, kPop1, kPush1, Imm::Id},            // FieldEffect (id = the effect; amount)
   {kImm3, kPop2, kPush1, Imm::SubjIdFlags},   // Inherit (id = the role, flags = InheritOp; a, b)
};

/// What an Encounter op changes in the roll in progress (F1; tools/effect_compiler.py ENCOUNTER_KINDS mirrors the
/// order). Operands a, b, c in push order.
enum class EncounterOp : uint8_t
{
   Rate,         ///< encounter_rate N D: the trigger odds times a / b (b >= 1).
   Levels,       ///< encounter_levels LO HI: the entry's lowest level + a, highest + b.
   LevelBias,    ///< encounter_level_bias N: the encounter_level pipeline's level_bias input = a.
   SkipBelow,    ///< encounter_skip_below LEVEL: no encounter with a creature below level a.
   PreferType,   ///< encounter_type TYPE N D: with odds a in b, the pick keeps to entries of type flags.
   PreferField,  ///< encounter_field FIELD V N D: with odds b in c, the pick keeps to entries whose species field
                 ///< flags is a.
   Reroll,       ///< encounter_reroll: encounter_made only: the creature is picked again.
   NUM_ENCOUNTER_OPS   ///< Number of kinds.
};

/// What an Inherit op does to a combination's result (B1; tools/effect_compiler.py INHERIT_KINDS mirrors the order;
/// capsule/combine/combine.h). Operands a, b in push order; the result is the holder's record.
enum class InheritOp : uint8_t
{
   Copy,    ///< inherit ROLE from INPUT: every element of the role from the input's record.
   Pick,    ///< inherit_pick ROLE N: a distinct elements of the role, each from a random input.
   Odds,    ///< inherit_odds ROLE from INPUT N D: Copy with odds a in b.
   Add,     ///< inherit_add ROLE AMOUNT: the role + a, within 0 and its maximum.
   NUM_INHERIT_OPS   ///< Number of kinds.
};

/// The shape of an opcode.
/// @param op  the opcode (< kOpCount)
/// @return its OpInfo
constexpr const OpInfo& op_info(Op op)
{
   return kOpInfo[static_cast<int>(op)];
}

/// What Get / Set read or write (the arg byte: stat, stage, counter slot,
/// word slot, type slot, data column or condition).
enum class Ref : uint8_t
{
   Hp,              ///< Current HP.
   MaxHp,           ///< Maximum HP.
   Level,           ///< Level.
   Stat,            ///< A stat (arg = stat).
   Stage,           ///< A stage (arg = stage).
   Status,          ///< Primary status (kNone8: none).
   StatusCounter,   ///< Primary-status counter (sleep turns, ...); writable.
   StatusAux,       ///< Primary-status scratch (escalation, ...); writable.
   Counter,         ///< A volatile counter (arg = counter slot); writable.
   Word,            ///< A volatile word (arg = word slot); writable.
   Type,            ///< A type (arg = type slot).
   Passive,         ///< Passive.
   Item,            ///< Held item.
   Species,         ///< Species.
   LastMove,        ///< Last move used.
   TakenAmount,     ///< Last damage taken: amount.
   TakenFrom,       ///< Last damage taken: from (battler).
   TakenMove,       ///< Last damage taken: move.
   TakenCategory,   ///< Last damage taken: category.
   TakenTurn,       ///< Last damage taken: turn.
   Position,        ///< Field position.
   Side,            ///< Side index.
   Party,           ///< Party index.
   SpeciesField,    ///< A species data column (arg = column).
   Alive,           ///< 1 while HP > 0.
   CanSwitch,       ///< 1 when a replacement is available.
   MoveType,        ///< The context move's type.
   MoveCategory,    ///< Its category.
   MovePower,       ///< Its power.
   MoveAccuracy,    ///< Its accuracy.
   MovePriority,    ///< Its priority.
   MoveBase,        ///< Its base of a move resource (arg = resource; D-25).
   MoveField,       ///< A move data column (arg = column).
   WeatherId,       ///< Current weather (kNone8: none).
   WeatherTurns,    ///< Weather turns left.
   FieldTurns,      ///< A field condition's turns left (arg = condition).
   SideTurns,       ///< A side condition's turns left (arg = condition).
   SideLayers,      ///< A side condition's layers (arg = condition).
   SideActive,      ///< Battlers active on the side.
   Turn,            ///< Turn number.
   Hits,            ///< Hits the move has landed so far.
   Crit,            ///< 1 when the current hit is critical.
   Battler,         ///< The subject's battler index (VM; point handlers read it through point_get()).
   Value,           ///< ctx.value (VM).
   Dealt,           ///< ctx.dealt (VM).
   Move,            ///< ctx.move (VM).
   Owner,           ///< ctx.owner (VM).
   Acted,           ///< 1 once the battler acted this turn.
   Locked,          ///< 1 while the move in progress came from a lock (Op::Lock).
   Resource,        ///< A resource's pool (arg = resource; a move resource: the context move's slot); writable.
   ResourceMax,     ///< Its maximum (arg = resource).
   MoveCost,        ///< The context move's fixed cost of a resource (arg = resource; 0 for a formula cost).
   TargetedBy,      ///< The last move that targeted the battler since it entered (any move; kNone16: none; E13b).
   DelayedTurns,    ///< Turns left of the pending effect at the battler's position (0: none; E13b).
   Human,           ///< 1 when the battler's party is human-controlled (E13b).
   Unowned,         ///< 1 when the battler's party is unowned (PARTY_UNOWNED; E13b).
   Gene,            ///< A stat's gene (arg = stat; the stat_genes role's value the battler was built with; E13b).
   PlayerSide,      ///< 1 when the battler is on the human player's side (player_side(); C5: coin-scatter coins).
   Absorbed,        ///< 1 when a decoy took the latest hit of the move in progress (Op::Absorb; G1).
   FieldEffectActive,     ///< 1 while timed field effect arg runs (capsule/script/field_effects.h; F1).
   FieldEffectRemaining,  ///< What timed field effect arg has left (steps or minutes; F1).
   NUM_REFS         ///< Number of refs.
};

/// Whether the VM reads a ref itself (the subject's battler index and the context registers); every other
/// ref is a host read.
/// @param ref  the ref
/// @return true for Battler, Value, Dealt, Move and Owner
constexpr bool vm_ref(Ref ref)
{
   return ref >= Ref::Battler && ref <= Ref::Owner;
}

/// What Has tests.
enum class HasKind : uint8_t
{
   Status,      ///< The battler has this primary status.
   Volatile,    ///< The battler has this volatile.
   Tag,         ///< The battler carries this tag.
   Type,        ///< The battler has this type.
   MoveFlag,    ///< The context move has this flag.
   SideCond,    ///< The side has this condition.
   FieldCond,   ///< The field has this condition.
   NUM_KINDS    ///< Number of kinds.
};

/// @name Subjects (subj8)
/// Codes 0-6 name a context register or a pseudo subject; kSubjLocal + n is
/// the battler whose index is in local n; kSubjBattler + n is battler n; kSubjSide on top of a battler
/// subject means that battler's side.
///@{
inline constexpr uint8_t kSubjHolder = 0;      ///< ctx.holder.
inline constexpr uint8_t kSubjOther = 1;       ///< ctx.other.
inline constexpr uint8_t kSubjUser = 2;        ///< ctx.user.
inline constexpr uint8_t kSubjTarget = 3;      ///< ctx.target.
inline constexpr uint8_t kSubjMove = 4;        ///< The context move.
inline constexpr uint8_t kSubjField = 5;       ///< The field.
inline constexpr uint8_t kSubjNone = 6;        ///< Nothing (global reads).
inline constexpr uint8_t kSubjLocal = 0x10;    ///< + n: the battler in local n.
inline constexpr uint8_t kSubjBattler = 0x20;  ///< + n: battler n (B1: a combination's input, `input.N`).
inline constexpr uint8_t kSubjSide = 0x80;     ///< Flag: the battler's side.
inline constexpr uint8_t kSubjBaseMask = 0x7F; ///< The subject without kSubjSide.
///@}

/// What a resolved subject is.
enum class PlaceKind : uint8_t
{
   Battler,   ///< A battler.
   Side,      ///< A battler's side.
   Move,      ///< The context move.
   Field,     ///< The field.
   None,      ///< Global.
   NUM_KINDS  ///< Number of kinds.
};

/// A resolved subject, as the host sees it.
struct Place
{
   PlaceKind kind;    ///< What it is.
   uint8_t battler;   ///< Battler (Battler, Side: the battler whose side; else ctx.holder).
   uint16_t move;     ///< ctx.move.
};
CAPSULE_POD(Place, 4);

/// Battler sets for ForEach (relative to the subject).
enum class BattlerSet : uint8_t
{
   Foes,           ///< Active foes.
   AdjacentFoes,   ///< Active foes the format calls adjacent.
   Allies,         ///< Active allies (not the subject).
   Others,         ///< Every other active battler.
   All,            ///< Every active battler.
   NUM_SETS        ///< Number of sets.
};

/// Modify modes.
enum class ModifyMode : uint8_t
{
   By,         ///< value * q8 >> 8.
   Add,        ///< value + arg.
   Set,        ///< arg.
   NUM_MODES   ///< Number of modes.
};

/// Switch modes.
enum class SwitchMode : uint8_t
{
   Pass,       ///< Pass stages and passing volatiles to the replacement.
   Ask,        ///< Plain switch to the chosen replacement.
   Random,     ///< Forced switch to a replacement the host picks.
   NUM_MODES   ///< Number of modes.
};

/// Requests (Ask kind8).
enum class AskKind : uint8_t
{
   SwitchTarget,   ///< A replacement an effect asks for (a switch that passes on, a hit-and-switch move).
   LearnMove,      ///< Which move to forget.
   Choice,         ///< One of n options (reserved).
   NUM_KINDS       ///< Number of kinds.
};

/// @name Primitive flags
///@{
inline constexpr uint8_t kDamageTyped = 1;          ///< DamageHp: apply type effectiveness.
inline constexpr uint8_t kDamageQuiet = 2;          ///< DamageHp: no message.
inline constexpr uint8_t kDamageNoFaintCheck = 4;   ///< DamageHp: no faint check yet.
inline constexpr uint8_t kInflictQuiet = 1;         ///< Inflict: no failure message.
inline constexpr uint8_t kCureQuiet = 1;            ///< Cure: no "cured" text.
inline constexpr uint8_t kSideAddLayer = 1;         ///< SideOn: add a layer (turns ignored).
inline constexpr uint8_t kLockFree = 1;             ///< Lock: the locked action pays no cost (D-25).
inline constexpr uint8_t kResourceEverySlot = 1;    ///< Restore / Raise: every move slot of a move resource.
inline constexpr uint8_t kResourceFull = 2;         ///< Restore: to the maximum (the amount is ignored).
inline constexpr uint8_t kStageQuiet = 0x80;        ///< Stage id8: no message when the stage cannot change.
inline constexpr uint8_t kStageSilent = 0x40;       ///< Stage id8: no message at all (the Stage event stays; E13b).
inline constexpr uint8_t kStageIdMask = 0x3F;       ///< Stage id8: the stage.
///@}

/// Owner kinds (what a block of logic belongs to).
enum class OwnerKind : uint8_t
{
   Move,             ///< A move.
   Passive,          ///< A passive.
   Item,             ///< An item.
   Status,           ///< A primary status.
   Volatile,         ///< A volatile.
   Weather,          ///< A weather.
   SideCondition,    ///< A side condition.
   FieldCondition,   ///< A field condition.
   Global,           ///< Ruleset-level handlers for every battler.
   NUM_KINDS         ///< Number of kinds.
};

/// Hooks and move phases (Handler::hook; tools/effect_compiler.py HOOKS mirrors the order).
enum class Hook : uint8_t
{
   SwitchIn,        ///< A battler entered.
   SwitchOut,       ///< A battler leaves (before resets).
   Faint,           ///< A battler fainted.
   TurnStart,       ///< A turn begins.
   BeforeMove,      ///< Before the user acts (stop: turn lost).
   UsedMove,        ///< After the move's costs were paid.
   BeforeAccuracy,  ///< Target, before the accuracy check (protection).
   TryHit,          ///< Target (and the move's own phase): immune / block.
   BeforeDamage,    ///< Target, value = damage.
   DamagingHit,     ///< Target, after damage.
   AfterHit,        ///< User, value = dealt.
   AfterMove,       ///< After the move (and the move's own phase).
   SetStatus,       ///< A status is about to be set (block).
   StageChange,     ///< A stage is about to change (block / modify).
   Update,          ///< HP changed.
   Residual,        ///< End of turn.
   Order,           ///< Turn-order key (value).
   Start,           ///< Condition-own: started.
   Already,         ///< Condition-own: already there.
   End,             ///< Condition-own: ended.
   SideResidual,    ///< Side condition, end of turn.
   FieldResidual,   ///< Field condition / weather, end of turn.
   Use,             ///< Move phase: once, user level.
   Target,          ///< Move phase: script targeting.
   Hit,             ///< Move phase: per target.
   TryStart,        ///< Condition-own: about to start (immune / block / fail refuse it).
   TrySwitch,       ///< A battler wants to switch out or run away (block: it is trapped; E13b). C5: holder = it,
                    ///< then each active foe (its per_foe handlers; other = the battler); value = the battler.
   Redirect,        ///< Another battler's chosen single-target move picked its target (`target`; value kNoClaim
                    ///< in; modify set BATTLER claims it: the first valid claim in position order wins; E13b, C5).
   Land,            ///< Move-owned: its pending effect (Op::Delay) lands (holder = the battler there; E13b).
   UseField,        ///< Item-owned: used from the bag out of battle (holder = the party member; G2: a field
                    ///< context, include/capsule/inventory/use.h). Fail: no effect.
   UseBattle,       ///< Item-owned: the battle's Item action (G3 runs it; G2 compiles it).
   AiScore,         ///< The AI scores a move (pure: modify the score, block = veto; holder = user, then target;
                    ///< move none: a switching rule asks whether to switch out; G1, capsule/battle/data/ai.h).
   WeatherChanged,  ///< The acting weather changed (C6): holder = each active battler, other = who changed it,
                    ///< value = the weather acting now (255: none); forms that follow the weather change at once.
   Encounter,       ///< An encounter roll out of battle, before the pick (F1; capsule/battle/encounter_lead.h):
                    ///< holder = the party's lead (a field context), value = the method. Block: no encounter.
   EncounterMade,   ///< After the pick, before the battle (F1): holder = the lead, target = other = the creature made
                    ///< (its record), value = its level. Block: no encounter.
   Steps,           ///< A step effect's run out of battle (F3; capsule/battle/step_effects.h): holder = each party
                    ///< member (a field context), value = the step effect. Fail / block end the owner's run.
   Combine,         ///< A combination's result built out of battle (B1; capsule/combine/combine.h): holder = the
                    ///< result, input.N = the inputs, value = the method, dealt = their count; fail / block refuse.
   Hatch,           ///< An egg cycle out of battle (B2): holder = each member not an egg, value = the steps
                    ///< each egg's counter loses (modify changes it; the largest answer counts).
   NUM_HOOKS        ///< Number of hooks.
};
/// Number of hooks (HookMask bits).
inline constexpr int kHookCount = static_cast<int>(Hook::NUM_HOOKS);

/// Bits in a point mask (Owner::point_mask) and in a battler mask. Points are the ruleset's (a point handler's
/// Handler::hook is the point's id in the ruleset's declaration order; tools/effect_compiler.py takes the names from
/// the structure's "points").
inline constexpr int kMaskBits = 32;
static_assert(kMaxBattlers <= kMaskBits);   // ForEach keeps a battler bit mask in one local

/// Bits per word of a hook mask.
inline constexpr int kHookMaskWordBits = 32;
/// Words of a hook mask: as many as the hooks need (G3 widened it past 32 hooks; a hook appended to Hook grows it
/// by itself, a word per 32 hooks, and tools/effect_compiler.py emits the same count).
inline constexpr int kHookMaskWords = (kHookCount + kHookMaskWordBits - 1) / kHookMaskWordBits;

/// A bit per Hook with a handler (Owner::hooks): fixed words of 32 bits, so an owner stays 4-byte aligned and a test
/// is one load, one shift and one mask on the ARM7TDMI (no 64-bit arithmetic).
struct HookMask
{
   uint32_t words[kHookMaskWords];   ///< Hook h is bit h % 32 of word h / 32.
};

/// Whether a hook mask has a hook's bit.
/// @param m  the mask
/// @param h  the hook
/// @return true if set
constexpr bool has_hook(const HookMask& m, Hook h)
{
   const unsigned i = static_cast<unsigned>(h);
   return ((m.words[i / kHookMaskWordBits] >> (i % kHookMaskWordBits)) & 1u) != 0;
}

/// @name Program, handler and owner flags
///@{
inline constexpr uint8_t kProgramYields = 1;   ///< May yield.
inline constexpr uint8_t kProgramAsks = 2;     ///< May ask.
inline constexpr uint8_t kProgramRng = 4;      ///< May draw.
inline constexpr uint8_t kHandlerPoint = 1;    ///< Handler::program is a point handler (generated C++).
/// switch_in: also runs once per foe (ctx.value = kPartFoe); the legacy effect lists' per-foe components.
/// try_switch (C5): runs only when a foe of the holder switches out or runs (not for the holder's own).
inline constexpr uint8_t kHandlerPerFoe = 2;
inline constexpr int32_t kPartUser = 0;        ///< switch_in ctx.value: the run for the battler itself.
inline constexpr int32_t kPartFoe = 1;         ///< switch_in ctx.value: a kHandlerPerFoe handler's run for one foe.
inline constexpr uint8_t kOwnerAnnounce = 1;   ///< The owner is announced when its handlers act.
inline constexpr int32_t kNoClaim = 0xFF;      ///< redirect ctx.value in: nobody claimed the move (C5; = kBench).
///@}

/// One program in Bank::code.
struct ProgramInfo
{
   uint32_t offset;      ///< First byte in Bank::code.
   uint16_t length;      ///< Bytes.
   uint8_t max_stack;    ///< Deepest operand stack (proven, subs included).
   uint8_t locals;       ///< Locals used (subs included).
   uint8_t flags;        ///< kProgram* flags.
   uint8_t events_max;   ///< Worst-case events (per segment between yields).
   uint16_t reserved;    ///< 0.
};
CAPSULE_POD(ProgramInfo, 12);

/// One handler of an owner.
struct Handler
{
   uint8_t hook;       ///< Hook, or Point with kHandlerPoint.
   uint8_t flags;      ///< kHandler* flags.
   uint16_t program;   ///< Program index, or point function with kHandlerPoint.
};
CAPSULE_POD(Handler, 4);

/// An owner's logic: its handlers and masks.
struct Owner
{
   HookMask hooks;           ///< Bit per Hook with a handler (has_hook(); G3: was a 32-bit hook_mask).
   uint32_t point_mask;      ///< Bit per Point with a handler.
   uint16_t name;            ///< Index in its kind's table (move, status, ...).
   uint16_t first_handler;   ///< First in Bank::handlers.
   uint16_t tags;            ///< Tag bits it gives its holder.
   uint8_t handler_count;    ///< Handlers.
   OwnerKind kind;           ///< Kind.
   uint8_t flags;            ///< kOwner* flags.
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
/// Bytes of an Owner besides its hook mask.
inline constexpr int kOwnerBytesBesidesHooks = 16;
CAPSULE_POD(Owner, kOwnerBytesBesidesHooks + static_cast<int>(sizeof(HookMask)));

/// Whether an owner has a handler for a hook.
/// @param o  the owner
/// @param h  the hook
/// @return true if it has
constexpr bool has_hook(const Owner& o, Hook h)
{
   return has_hook(o.hooks, h);
}

/// Compiled effect logic (tools/effect_compiler.py).
struct Bank
{
   const uint8_t* code;            ///< Every program's bytes.
   const ProgramInfo* programs;    ///< Programs.
   const Handler* handlers;        ///< Handlers, owner by owner.
   const Owner* owners;            ///< Owners.
   uint32_t code_size;             ///< Bytes of code.
   uint16_t program_count;         ///< Programs.
   uint16_t handler_count;         ///< Handlers.
   uint16_t owner_count;           ///< Owners.
   uint16_t reserved;              ///< 0.
};

/// @name Debug line table (bench builds only; D-23 section 3.3)
/// Where a bank's code comes from in its sources: tools/effect_compiler.py emit_line_table() writes one for a bank
/// compiled under track_lines(); only the battle bench (apps/battle_bench) generates and links it. The bank itself
/// and every game build stay without it.
///@{

/// One entry: the code of a program from pc on (up to the program's next entry) comes from a source position.
struct SourceLine
{
   uint16_t program;    ///< Program index (Bank::programs).
   uint16_t pc;         ///< First instruction offset in the program.
   uint16_t line;       ///< Source line (1 = first).
   uint16_t column;     ///< Source column (1 = first).
   uint16_t file;       ///< Index in LineTable::files.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(SourceLine, 12);

/// A bank's entries, sorted by program and pc.
struct LineTable
{
   const SourceLine* lines;    ///< Entries.
   const char* const* files;   ///< Source file names (as the compiler was given them).
   uint16_t line_count;        ///< Entries used.
   uint16_t file_count;        ///< Files used.
};
///@}

/// Context registers of one run.
struct Context
{
   int32_t value;    ///< The hook's value (damage, stage delta, order key, ...).
   int32_t dealt;    ///< Damage the move dealt so far.
   uint16_t move;    ///< The move involved.
   uint16_t owner;   ///< Owner index of the running program.
   uint8_t holder;   ///< Battler holding the owner.
   uint8_t other;    ///< The other battler of the event.
   uint8_t user;     ///< The move's user.
   uint8_t target;   ///< The move's current target.
};
CAPSULE_POD(Context, 16);

/// One sub call's return point.
struct CallFrame
{
   uint16_t program;   ///< Caller.
   uint16_t pc;        ///< Return offset in it.
};
CAPSULE_POD(CallFrame, 4);

/// Where a run stands.
enum class RunState : uint8_t
{
   Idle,      ///< Not started, or finished.
   Running,   ///< Ready to run (started or yielded).
   Asking,    ///< Waits for answer().
   NUM_STATES ///< Number of states.
};

/// A run: the whole VM state of one program invocation (plain data).
struct EffectRun
{
   Context ctx;                          ///< Context registers.
   int32_t stack[kEffectStack];          ///< Operand stack.
   int32_t locals[kEffectLocals];        ///< Locals.
   CallFrame calls[kEffectCallDepth];    ///< Sub return points.
   uint16_t program;                     ///< Running program.
   uint16_t pc;                          ///< Next instruction (offset in the program).
   uint8_t sp;                           ///< Operand stack slots in use.
   uint8_t call_depth;                   ///< Sub calls in progress.
   RunState state;                       ///< Where it stands.
   uint8_t resumable;                    ///< 1: top-level (may yield and ask).
};
CAPSULE_POD(EffectRun, 136);

/// Why a run faulted.
enum class Fault : uint8_t
{
   None,             ///< No fault.
   StackOverflow,    ///< Pushed past kEffectStack.
   StackUnderflow,   ///< Popped an empty stack.
   BadLocal,         ///< Local index out of range.
   BadOpcode,        ///< Not an opcode.
   BadJump,          ///< A jump backwards or past the end, or a back-edge forwards.
   BadProgram,       ///< A program index or pc out of range.
   CallDepth,        ///< Sub calls past kEffectCallDepth.
   OpBudget,         ///< More than kEffectOpsPerRun instructions.
   Depth,            ///< Nested runs past kMaxEffectDepth (dispatch skipped).
   Suspend,          ///< Yield or Ask in a run that may not suspend.
   BadSubject,       ///< A subject code that does not exist, or a local that holds no battler.
   BadOperand,       ///< Another immediate out of range (ref, kind, set, mode).
   NUM_FAULTS        ///< Number of faults.
};

/// How a run (or one resume of it) ended.
enum class Status : uint8_t
{
   Done,      ///< End / Ret at the outermost level.
   Stopped,   ///< Stop.
   Failed,    ///< Fail.
   Blocked,   ///< Block.
   Immune,    ///< Immune.
   Yielded,   ///< Yield: resume() later.
   Asked,     ///< Ask: answer() later.
   Fault,     ///< A limit was broken (Machine::faults counts it).
   Skipped,   ///< invoke() at kMaxEffectDepth: not run (a fault too).
   NUM_STATUSES ///< Number of statuses.
};

/// Nesting and fault bookkeeping shared by every run of one battle (plain data).
struct Machine
{
   EffectRun frames[kMaxEffectDepth];   ///< Frames of nested (hook) runs.
   uint16_t faults;                     ///< Faults so far (saturates).
   uint8_t depth;                       ///< Runs executing right now.
   Fault last_fault;                    ///< The latest fault.
};

/// A primitive, decoded for host_action().
struct Action
{
   int32_t args[kMaxActionArgs];   ///< Operands, in push order.
   Place at;                       ///< The subject (Imm::Subj* layouts).
   Place about;                    ///< SayAt: the second subject.
   uint16_t id;                    ///< id8 / text16 / mode / kind immediate.
   uint8_t flags;                  ///< flags8, or n of SetTypes.
   Op op;                          ///< The opcode.
   uint8_t argc;                   ///< Operands in args.
   uint8_t reserved[kPadAfterOneByte];   ///< 0.
};
CAPSULE_POD(Action, 28);

/// What a primitive gave back.
struct Outcome
{
   int32_t value;   ///< Pushed when the op pushes (dealt, ok, applied, answer).
   bool suspend;    ///< Ask only: no answer yet, the run waits for answer().
   bool stop;       ///< The run ends here with Status::Stopped (the host decided; e.g. an immune target).
   bool fail;       ///< The run ends here with Status::Failed (a condition's try_start failed the move).
};

/// The embedder's state the host functions work on (defined by the embedder).
struct HostState;
/// The pipeline context generated point handlers read (defined by the embedder).
struct PointCtx;

/// @name Integer semantics (VM, generated point handlers and the compiler's evaluator agree)
///@{

/// Bits a Q8 number is scaled by.
inline constexpr int kQ8Shift = 8;
/// Largest shift count (counts are clamped to 0-31).
inline constexpr int32_t kMaxShift = 31;

/// The smaller value.
/// @param a  one
/// @param b  the other
/// @return min(a, b)
constexpr int32_t min_value(int32_t a, int32_t b)
{
   return a < b ? a : b;
}

/// The larger value.
/// @param a  one
/// @param b  the other
/// @return max(a, b)
constexpr int32_t max_value(int32_t a, int32_t b)
{
   return a > b ? a : b;
}

/// Table select: the entry at @p i clamped into the table (Op::Pick).
/// @tparam N    entries
/// @param i     index
/// @param table the entries
/// @return table[clamp(i, 0, N - 1)]
template <int N>
constexpr int32_t pick_value(int32_t i, const int32_t (&table)[N])
{
   return table[i < 0 ? 0 : (i >= N ? N - 1 : i)];
}

/// a + b, saturating.
/// @param a  left
/// @param b  right
/// @return the sum
constexpr int32_t sat_add(int32_t a, int32_t b)
{
   int32_t out = 0;
   if (__builtin_add_overflow(a, b, &out))
   {
      return b < 0 ? INT32_MIN : INT32_MAX;
   }
   return out;
}

/// a - b, saturating.
/// @param a  left
/// @param b  right
/// @return the difference
constexpr int32_t sat_sub(int32_t a, int32_t b)
{
   int32_t out = 0;
   if (__builtin_sub_overflow(a, b, &out))
   {
      return b < 0 ? INT32_MAX : INT32_MIN;
   }
   return out;
}

/// a * b, saturating.
/// @param a  left
/// @param b  right
/// @return the product
constexpr int32_t sat_mul(int32_t a, int32_t b)
{
   int32_t out = 0;
   if (__builtin_mul_overflow(a, b, &out))
   {
      return (a < 0) != (b < 0) ? INT32_MIN : INT32_MAX;
   }
   return out;
}

/// Clamps a 64-bit value into int32.
/// @param v  value
/// @return v limited to int32
constexpr int32_t sat_narrow(int64_t v)
{
   return v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : static_cast<int32_t>(v));
}

/// a / b truncated toward zero; 0 when b is 0; INT32_MIN / -1 saturates.
/// @param a  dividend
/// @param b  divisor
/// @return the quotient
constexpr int32_t safe_div(int32_t a, int32_t b)
{
   if (b == 0)
   {
      return 0;
   }
   if (b == -1)
   {
      return sat_sub(0, a);
   }
   return a / b;
}

/// a % b with the dividend's sign; 0 when b is 0 (and for b = -1).
/// @param a  dividend
/// @param b  divisor
/// @return the remainder
constexpr int32_t safe_mod(int32_t a, int32_t b)
{
   return (b == 0 || b == -1) ? 0 : a % b;
}

/// -a, saturating.
/// @param a  value
/// @return its negation
constexpr int32_t sat_neg(int32_t a)
{
   return sat_sub(0, a);
}

/// |a|, saturating.
/// @param a  value
/// @return its magnitude
constexpr int32_t sat_abs(int32_t a)
{
   return a < 0 ? sat_neg(a) : a;
}

/// max(lo, min(hi, v)).
/// @param v   value
/// @param lo  low bound
/// @param hi  high bound
/// @return the clamped value
constexpr int32_t clamp_value(int32_t v, int32_t lo, int32_t hi)
{
   const int32_t upper = v < hi ? v : hi;
   return upper > lo ? upper : lo;
}

/// (a * b) >> 8 (arithmetic shift), saturating.
/// @param a  value
/// @param q  Q8 factor
/// @return the scaled value
constexpr int32_t mul_q8(int32_t a, int32_t q)
{
   return sat_narrow((static_cast<int64_t>(a) * q) >> kQ8Shift);
}

/// a << b (b clamped to 0-31), saturating.
/// @param a  value
/// @param b  count
/// @return the shifted value
constexpr int32_t sat_shl(int32_t a, int32_t b)
{
   const int32_t n = clamp_value(b, 0, kMaxShift);
   return sat_narrow(static_cast<int64_t>(a) * (static_cast<int64_t>(1) << n));
}

/// a >> b (arithmetic, b clamped to 0-31).
/// @param a  value
/// @param b  count
/// @return the shifted value
constexpr int32_t arith_shr(int32_t a, int32_t b)
{
   return a >> clamp_value(b, 0, kMaxShift);
}

/// Applies a Modify to a value.
/// @param mode  how
/// @param v     the value
/// @param arg   Q8 factor (By), addend (Add) or new value (Set)
/// @return the new value
constexpr int32_t modify_value(ModifyMode mode, int32_t v, int32_t arg)
{
   switch (mode)
   {
   case ModifyMode::By:
      return mul_q8(v, arg);
   case ModifyMode::Add:
      return sat_add(v, arg);
   default:
      return arg;
   }
}
///@}

/// @name VM
///@{

/// Prepares a run of a program (it runs on the next resume()).
/// @param r          the run
/// @param program    program index
/// @param ctx        context registers
/// @param resumable  true for a top-level phase (may yield and ask)
void begin(EffectRun& r, uint16_t program, const Context& ctx, bool resumable);
/// Runs a begun or yielded run until it ends, yields or asks.
/// @param m     nesting and fault bookkeeping
/// @param bank  the code
/// @param r     the run
/// @param host  the embedder's state
/// @return how it stopped (Yielded / Asked: call resume() / answer() later)
Status resume(Machine& m, const Bank& bank, EffectRun& r, HostState& host);
/// Gives an asking run its answer and resumes it.
/// @param m      nesting and fault bookkeeping
/// @param bank   the code
/// @param r      the run (RunState::Asking)
/// @param host   the embedder's state
/// @param value  the answer (pushed)
/// @return as resume()
Status answer(Machine& m, const Bank& bank, EffectRun& r, HostState& host, int32_t value);
/// Runs a hook program to its end in a fresh frame of Machine::frames (it
/// may not suspend). At kMaxEffectDepth it is skipped and counted as a fault.
/// @param m        nesting and fault bookkeeping
/// @param bank     the code
/// @param host     the embedder's state
/// @param program  program index
/// @param ctx      context registers in; ctx.value / ctx.dealt as the program left them out
/// @return how it ended
Status invoke(Machine& m, const Bank& bank, HostState& host, uint16_t program, Context& ctx);
/// Whether a status ends the run.
/// @param s  a status
/// @return false for Yielded and Asked
constexpr bool finished(Status s)
{
   return s != Status::Yielded && s != Status::Asked;
}
///@}

/// @name Host functions (implemented by the embedder, resolved at link time)
///@{

/// A read (Op::Get; never Ref::Battler, Value, Dealt, Move or Owner, which the VM reads itself).
/// @param host  the embedder's state
/// @param r     the running run (context registers)
/// @param ref   what
/// @param at    of what
/// @param arg   stat, stage, slot, column or condition
/// @return the value
int32_t host_get(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg);
/// A write (Op::Set): counters, words, status counter and scratch. The host clamps to the storage.
/// @param host   the embedder's state
/// @param r      the running run
/// @param ref    what
/// @param at     of what
/// @param arg    slot
/// @param value  the value
void host_set(HostState& host, const EffectRun& r, Ref ref, Place at, uint8_t arg, int32_t value);
/// A membership test (Op::Has).
/// @param host  the embedder's state
/// @param r     the running run
/// @param kind  what kind of id
/// @param at    of what
/// @param id    the id
/// @return true when present
bool host_has(HostState& host, const EffectRun& r, HasKind kind, Place at, uint8_t id);
/// A record role of a battler's party record (Op::RoleGet).
/// @param host     the embedder's state
/// @param r        the running run
/// @param battler  battler
/// @param role     role ID
/// @param elem     element (multi-count roles)
/// @return the value
int32_t host_role_get(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem);
/// Writes a record role (Op::RoleSet).
/// @param host     the embedder's state
/// @param r        the running run
/// @param battler  battler
/// @param role     role ID
/// @param elem     element
/// @param value    the value
void host_role_set(HostState& host, const EffectRun& r, uint8_t battler, uint8_t role, uint8_t elem, int32_t value);
/// A battler set as a bit mask (bit n = battler n; Op::ForEach).
/// @param host  the embedder's state
/// @param r     the running run
/// @param set   which set
/// @param at    relative to which battler
/// @return the mask (bits at kMaxBattlers and above are ignored)
uint32_t host_battlers(HostState& host, const EffectRun& r, BattlerSet set, Place at);
/// Draws 0 to n - 1 (Op::Rand; called only with n >= 1).
/// @param host  the embedder's state
/// @param n     outcomes
/// @return the draw
uint32_t host_below(HostState& host, uint32_t n);
/// Draws lo to hi (Op::Between; called only with lo <= hi).
/// @param host  the embedder's state
/// @param lo    lowest
/// @param hi    highest
/// @return the draw
int32_t host_between(HostState& host, int32_t lo, int32_t hi);
/// Rolls n in m (Op::Chance; called only with m >= 1 and n >= 0).
/// @param host  the embedder's state
/// @param n     successes
/// @param m     outcomes
/// @return true with probability n / m
bool host_chance(HostState& host, uint32_t n, uint32_t m);
/// Rolls n in m effect odds of battler @p by against battler @p on (Op::Odds; called only with m >= 1 and n >= 1):
/// the battle runs the ruleset's effect_odds pipeline (E9).
/// @param host  the embedder's state
/// @param r     the running run (its context move)
/// @param n     successes
/// @param m     outcomes
/// @param by    the battler causing the effect
/// @param on    the battler it lands on
/// @return true when it succeeds
bool host_odds(HostState& host, const EffectRun& r, uint32_t n, uint32_t m, uint8_t by, uint8_t on);
/// Performs a primitive (battle primitives, results Immune / Block,
/// presentation, Ask). It may start nested runs with invoke().
/// @param host  the embedder's state
/// @param r     the running run
/// @param a     the decoded primitive
/// @return its value; Ask may set suspend
Outcome host_action(HostState& host, EffectRun& r, const Action& a);
/// A point handler's read (generated C++ calls it).
/// @param c    the pipeline context
/// @param ref  what
/// @param subj subject code (kSubjHolder ... kSubjNone, kSubjSide flag)
/// @param arg  stat, stage, slot, column or condition
/// @return the value
int32_t point_get(PointCtx& c, Ref ref, uint8_t subj, uint8_t arg);
/// A point handler's membership test.
/// @param c     the pipeline context
/// @param kind  what kind of id
/// @param subj  subject code
/// @param id    the id
/// @return true when present
bool point_has(PointCtx& c, HasKind kind, uint8_t subj, uint8_t id);
///@}

} // namespace capsule::battle::effect
