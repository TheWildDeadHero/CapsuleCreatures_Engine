/// @file
/// The screen arena (UM, D-33): one static block of memory (EWRAM on the GBA) that screens borrow their large state
/// from while they are open, as the reference games allocate a screen's buffers from their heap when it opens and
/// free them when it closes. No heap: blocks are handed out last-in first-out and given back in that order.
///
/// Size: the build setting screen_arena_bytes (a game's reference/project.json "build" object, or the cache variable
/// CAPSULE_SCREEN_ARENA_BYTES; tools/build_settings.py), compiled per game (src/screen/arena_storage.cpp is a
/// runtime source). capacity() is the size; high_water() the most it held at once since start-up (or reset()).
///
/// Ownership follows the screen stack (capsule/screen/screen.h):
/// - A block taken while a screen is on the stack belongs to the top screen (whatever its phase). When that screen
///   leaves for good (close() and its out fade, close_now(), or replace()), the arena goes back to where it stood
///   when the screen's setup first ran: its blocks, and any taken after them, are given back.
/// - A screen suspended under another (Screen::leave(true): a child screen opens) keeps its blocks. One that rebuilds
///   on return, as most of the reference games' screens do (they free their buffers before the child opens and
///   allocate them again on the way back), calls release() on its first block in its leave(true): the child has
///   taken nothing yet at that point (leave(true) runs on the hand-over frame, before the child's first setup call),
///   so its blocks are the top ones; its setup with resumed = true takes them again.
/// - A block taken while no screen is open (the field's own state) belongs to the code that took it, which gives it
///   back with release(); screens may open above it (last in, first out still holds).
///
/// Blocks: Block<T> is a typed lease that fits in any static (all zero when empty, so CAPSULE_EWRAM_BSS can hold it):
/// take() returns a zeroed T, value-initialised (its default member initialisers applied); the lease knows when the
/// arena gave its memory back (taken() is false, get() asserts in debug builds). ScopedBlock<T> gives its block back
/// when it goes out of scope (a function's scratch buffer). A full arena (or kMaxBlocks live blocks) is a build
/// configuration error: take() halts with a message (hal::system::assert_failed()) in every build, never returns an
/// empty block. T must be trivially destructible (nothing runs when the arena takes the memory back).
#pragma once

#include "capsule/core/assert.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

/// The screen arena.
namespace capsule::screen::arena
{

inline constexpr uint32_t kAlign = 4;       ///< Every block starts on a multiple of this (the GBA's word).
/// The storage's own alignment: the most any block may ask for (a scalar type's largest).
inline constexpr uint32_t kStorageAlign = alignof(std::max_align_t);
inline constexpr int kMaxBlocks = 32;       ///< Blocks live at once.

/// Bytes the arena holds (the build setting screen_arena_bytes).
/// @return them
uint32_t capacity();

/// Bytes in use (also where the next block starts, before alignment).
/// @return 0 to capacity()
uint32_t used();

/// The most bytes in use at once since start-up or reset().
/// @return them
uint32_t high_water();

/// Live blocks.
/// @return 0 to kMaxBlocks
int blocks();

/// Whether a block of this size would fit now.
/// @param bytes  its size
/// @param align  its alignment (a power of two)
/// @return true when take() would succeed
bool fits(uint32_t bytes, uint32_t align = kAlign);

/// Takes a zeroed block (Block<T>::take() is the typed form). Halts with a message when it does not fit.
/// @param bytes        its size
/// @param align        its alignment (a power of two, at most kStorageAlign; at least kAlign is used)
/// @param[out] index   its entry in the live list
/// @param[out] serial  its serial (never 0)
/// @return its first byte
void* take(uint32_t bytes, uint32_t align, uint16_t& index, uint16_t& serial);

/// Whether a block is still live (not given back).
/// @param index   its entry
/// @param serial  its serial
/// @return true while it is
bool live(uint16_t index, uint16_t serial);

/// Gives back a block and every block taken after it (nothing when it is no longer live).
/// @param index   its entry
/// @param serial  its serial
void release(uint16_t index, uint16_t serial);

/// Gives back every block that starts at or above a mark (the screen stack's use: a screen leaves for good).
/// @param mark  a value used() returned
void release_to(uint32_t mark);

/// Gives back every block and clears the high-water mark (tests; screen::reset() calls it).
void reset();

/// C13a (R9 L3): halts with a message: a Block was used after its memory went back (in every build, as a full arena
/// does: a released block's memory belongs to another screen by then).
[[noreturn]] void used_after_release();

}   // namespace capsule::screen::arena

/// Full-screen scenes.
namespace capsule::screen
{

/// A typed lease on a block of the screen arena (see capsule/screen/arena.h). All zero when empty: a static of this
/// type needs no constructor and may be CAPSULE_EWRAM_BSS.
/// @tparam T  what the block holds (trivially destructible)
template <typename T>
class Block
{
   static_assert(std::is_trivially_destructible_v<T>, "the arena takes its memory back without running destructors");

public:
   /// Takes the block: zeroed, then value-initialised. It must not be taken already (debug builds assert).
   /// @return the new T
   T& take()
   {
      ENGINE_ASSERT(!taken());
      void* p = arena::take(static_cast<uint32_t>(sizeof(T)), static_cast<uint32_t>(alignof(T)), index_, serial_);
      p_ = ::new (p) T();   // T(): its default member initialisers over the zeroed bytes
      return *p_;
   }

   /// Whether it holds live memory (taken, and not given back by release() or the screen stack).
   /// @return true while it does
   bool taken() const
   {
      return p_ != nullptr && arena::live(index_, serial_);
   }

   /// What it holds. C13a (R9 L3): in every build a Block that is not taken (never taken, released, or given back
   /// by the screen stack) halts here instead of handing out memory another screen owns (its cost: the live check).
   /// @return it
   T* get() const
   {
      if (!taken()) [[unlikely]]
      {
         arena::used_after_release();
      }
      return p_;
   }

   /// What it holds.
   /// @return it
   T& operator*() const
   {
      return *get();
   }

   /// What it holds.
   /// @return it
   T* operator->() const
   {
      return get();
   }

   /// Gives the block back, and every block taken after it (nothing when it is not taken).
   void release()
   {
      if (p_ != nullptr)
      {
         arena::release(index_, serial_);
      }
      p_ = nullptr;
   }

private:
   T* p_;              ///< Its memory, or nullptr (never taken, or released by this lease).
   uint16_t index_;    ///< Its entry in the arena's live list.
   uint16_t serial_;   ///< Its serial (0: none; the arena's serials start at 1).
};

/// A Block that gives its memory back when it goes out of scope (a function's scratch buffer: last in, first out, so
/// the blocks a function takes go back in the reverse order of their scopes).
/// @tparam T  what the block holds
template <typename T>
class ScopedBlock
{
public:
   /// Takes the block.
   ScopedBlock()
      : block_{}
   {
      block_.take();
   }

   /// Gives it back.
   ~ScopedBlock()
   {
      block_.release();
   }

   ScopedBlock(const ScopedBlock&) = delete;              ///< One owner.
   ScopedBlock& operator=(const ScopedBlock&) = delete;   ///< One owner.

   /// What it holds.
   /// @return it
   T& operator*() const
   {
      return *block_;
   }

   /// What it holds.
   /// @return it
   T* operator->() const
   {
      return block_.get();
   }

private:
   Block<T> block_;   ///< The lease.
};

}   // namespace capsule::screen
