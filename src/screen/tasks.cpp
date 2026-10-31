/// @file
/// A screen's task list (see capsule/screen/tasks.h).
#include "capsule/screen/tasks.h"

#include "capsule/core/assert.h"

namespace capsule::screen::tasks
{
namespace
{

Task g_tasks[kMaxTasks];   ///< The list's entries.

/// The task that runs first.
/// @return its entry, or kNoTask when none is active
uint8_t head()
{
   for (int i = 0; i < kMaxTasks; ++i)
   {
      if (g_tasks[i].active && g_tasks[i].prev == kNoTask)
      {
         return static_cast<uint8_t>(i);
      }
   }
   return kNoTask;
}

/// Links a new task into the run order: before the first task with a larger priority number, else at the tail.
/// @param id  the new task's entry (not active yet)
void link(uint8_t id)
{
   Task& n = g_tasks[id];
   uint8_t at = head();
   if (at == kNoTask)
   {
      n.prev = kNoTask;
      n.next = kNoTask;
      return;
   }
   for (;;)
   {
      Task& t = g_tasks[at];
      if (n.priority < t.priority)
      {
         n.prev = t.prev;
         n.next = at;
         if (t.prev != kNoTask)
         {
            g_tasks[t.prev].next = id;
         }
         t.prev = id;
         return;
      }
      if (t.next == kNoTask)
      {
         n.prev = at;
         n.next = kNoTask;
         t.next = id;
         return;
      }
      at = t.next;
   }
}

}   // namespace

void reset()
{
   for (Task& t : g_tasks)
   {
      t = Task{};
      t.prev = kNoTask;
      t.next = kNoTask;
   }
}

uint8_t create(Func func, uint8_t priority)
{
   for (int i = 0; i < kMaxTasks; ++i)
   {
      Task& t = g_tasks[i];
      if (t.active)
      {
         continue;
      }
      const auto id = static_cast<uint8_t>(i);
      t.func = func;
      t.priority = priority;
      link(id);
      for (int16_t& d : t.data)
      {
         d = 0;
      }
      t.active = true;
      return id;
   }
   return kNoTask;
}

void destroy(uint8_t id)
{
   ENGINE_ASSERT(id < kMaxTasks);
   Task& t = g_tasks[id];
   if (!t.active)
   {
      return;
   }
   t.active = false;
   if (t.prev != kNoTask)
   {
      g_tasks[t.prev].next = t.next;
   }
   if (t.next != kNoTask)
   {
      g_tasks[t.next].prev = t.prev;
   }
   // the entry's own links stay: run() follows next after a task destroys itself
}

void run()
{
   uint8_t id = head();
   while (id != kNoTask)
   {
      Task& t = g_tasks[id];
      if (t.func != nullptr)
      {
         t.func(id);
      }
      id = t.next;   // as linked now (a task destroyed by itself keeps its next)
   }
}

Task& task(uint8_t id)
{
   ENGINE_ASSERT(id < kMaxTasks);
   return g_tasks[id];
}

bool active(uint8_t id)
{
   return id < kMaxTasks && g_tasks[id].active;
}

uint8_t find(Func func)
{
   for (int i = 0; i < kMaxTasks; ++i)
   {
      if (g_tasks[i].active && g_tasks[i].func == func)
      {
         return static_cast<uint8_t>(i);
      }
   }
   return kNoTask;
}

int count()
{
   int n = 0;
   for (const Task& t : g_tasks)
   {
      n += t.active ? 1 : 0;
   }
   return n;
}

}   // namespace capsule::screen::tasks
