/// @file
/// A screen's task list (UC, D-33): small per-frame functions with a priority and a few words of data, run in
/// priority order once a frame (the screen's Step::Tasks).
///
/// The list follows the reference games' task list: kMaxTasks entries; create() takes the lowest free entry and
/// links it into a list ordered by priority (a lower number runs first; a new task goes after the tasks of its own
/// priority); run() calls each task from the head, following the links as they are when each task returns, so a
/// task created during run() with a priority after the current task's runs in the same frame, and one before it
/// waits for the next frame. destroy() unlinks a task (its entry is free at once; its data stays until reused).
#pragma once

#include <cstdint>

/// Screen tasks.
namespace capsule::screen::tasks
{

inline constexpr int kMaxTasks = 16;        ///< Entries of the list.
inline constexpr int kTaskData = 16;        ///< Data words of a task.
inline constexpr uint8_t kNoTask = 0xFF;    ///< create() when the list is full; a link to no entry.

/// A task's function: called once a frame with its entry.
using Func = void (*)(uint8_t id);

/// One entry.
struct Task
{
   Func func;                  ///< Called by run().
   int16_t data[kTaskData];    ///< The task's own words (cleared by create()).
   uint8_t priority;           ///< Lower runs first.
   uint8_t prev;               ///< Entry before it in run order, or kNoTask (the head).
   uint8_t next;               ///< Entry after it, or kNoTask (the tail).
   bool active;                ///< The entry holds a task.
};

/// Empties the list.
void reset();

/// Creates a task.
/// @param func      its function
/// @param priority  lower runs first
/// @return its entry, or kNoTask when every entry is in use
uint8_t create(Func func, uint8_t priority);

/// Destroys a task (nothing for an inactive entry).
/// @param id  its entry
void destroy(uint8_t id);

/// Runs every task once, in priority order.
void run();

/// A task's entry (its data and function may be changed in place).
/// @param id  0 to kMaxTasks - 1
/// @return the entry
Task& task(uint8_t id);

/// Whether an entry holds a task.
/// @param id  the entry
/// @return true while it does
bool active(uint8_t id);

/// The first active entry running a function.
/// @param func  the function
/// @return its entry, or kNoTask
uint8_t find(Func func);

/// Active tasks.
/// @return the count
int count();

}   // namespace capsule::screen::tasks
