/* UM (D-33): the GBA link's memory checks, read after devkitARM's gba_cart.ld (capsule_game() passes this file to   */
/* the link as an input script, so it adds to the default script instead of replacing it).                         */
/*                                                                                                                 */
/* gba_cart.ld places .sbss (CAPSULE_EWRAM_BSS) "(NOLOAD) AT>ewram" without binding it to the region, so the link   */
/* does not fail when EWRAM's 256 KB are passed: objects above 0x02040000 mirror onto low EWRAM. Its .bss (IWRAM)    */
/* is placed the same way, and the stack grows down from __sp_usr into whatever IWRAM data ends below it. These      */
/* assertions make both a link error:                                                                              */
/*   EWRAM: .ewram + .sbss end at or below the region's end;                                                       */
/*   IWRAM: from the end of IWRAM's code and data (__iheap_start) to the user stack's top (__sp_usr) at least       */
/*          __capsule_stack_reserve bytes are left (the cmake option CAPSULE_GBA_STACK_RESERVE, given with --defsym). */

__capsule_ewram_limit = 0x02040000;   /* ORIGIN(ewram) + LENGTH(ewram) of gba_cart.ld */

ASSERT(__sbss_end__ <= __capsule_ewram_limit,
       "GBA memory: EWRAM (.ewram + .sbss) is over 256 KB; move screen state into capsule/screen/arena.h")
ASSERT(__sp_usr - __iheap_start >= __capsule_stack_reserve,
       "GBA memory: IWRAM leaves less stack than CAPSULE_GBA_STACK_RESERVE; move statics to CAPSULE_EWRAM_BSS")
