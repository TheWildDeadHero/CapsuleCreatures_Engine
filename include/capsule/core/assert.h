/// @file
/// The engine's single assertion macro (P-15).
///
/// Active in debug builds, compiled to nothing in release (define NDEBUG, or
/// set CAPSULE_ASSERTS explicitly). The condition is never evaluated when
/// disabled. The HAL decides how a failure is reported: GBA and Dreamcast halt
/// and show the expression, file and line; desktop prints a message and aborts.
#pragma once

#ifndef CAPSULE_ASSERTS
#ifdef NDEBUG
#define CAPSULE_ASSERTS 0
#else
/// 1 when ENGINE_ASSERT checks its condition, 0 when it compiles to nothing.
/// Defaults to 0 when NDEBUG is defined, else 1; may be set on the command line.
#define CAPSULE_ASSERTS 1
#endif
#endif

namespace capsule::hal::system
{
/// Reports a failed ENGINE_ASSERT and never returns (implemented by each HAL).
/// @param expr  the failed condition, as written
/// @param file  source file, repo-relative
/// @param line  source line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line);
}   // namespace capsule::hal::system

#if CAPSULE_ASSERTS
/// Checks @p cond in debug builds and calls hal::system::assert_failed() when
/// it is false. In release builds @p cond is type-checked but never evaluated.
/// @param cond  condition that must hold
#define ENGINE_ASSERT(cond)                                                                                            \
   do                                                                                                                  \
   {                                                                                                                   \
      if (!(cond)) [[unlikely]]                                                                                        \
      {                                                                                                                \
         ::capsule::hal::system::assert_failed(#cond, __FILE__, __LINE__);                                             \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)
#else
/// Release form: @p cond is type-checked but never evaluated.
/// @param cond  condition that must hold
#define ENGINE_ASSERT(cond)                                                                                            \
   do                                                                                                                  \
   {                                                                                                                   \
      (void)sizeof(!(cond));                                                                                           \
   }                                                                                                                   \
   while (false)
#endif
