# Third-party notices: CapsuleCreatures Engine

The engine itself is licensed under the Mozilla Public License 2.0 (`LICENSE`).
This file lists the third-party code that its builds link or use, the licence
of each, and what a game built with the engine has to do about it. None of it
is vendored in this repository: the libraries come from the toolchains and
from SDL2's pinned release (`cmake/sdl2.cmake`).

| Part | Used by | Licence | What a shipped game must do |
|---|---|---|---|
| libgba (devkitPro) | every GBA ROM (linked statically) | LGPL 2 with a static-linking exception (below) | identify its use of libgba, e.g. "based in part on the work of devkitPro (https://devkitpro.org)" |
| devkitARM startup code (`gba_crt0.s`, `gba.specs`, from devkitarm-crtls) | every GBA ROM | MPL-2.0 | say where its source is: https://github.com/devkitPro/devkitarm-crtls |
| newlib (C library, maths library) | every GBA ROM (from devkitARM) | a collection of BSD-style and similar notices | keep its notices with the ROM: `THIRD_PARTY_NOTICES.newlib.txt` (newlib's `COPYING.NEWLIB`) |
| GCC runtime (libgcc, libstdc++ / libsupc++) | GBA ROMs; Windows executables built with MinGW-w64 GCC | GPL-3 with the GCC Runtime Library Exception | nothing (the exception covers programs compiled with GCC) |
| SDL2 2.32.x | every desktop build (built from source, linked statically) | zlib (below) | nothing required; a credit is appreciated |
| MinGW-w64 runtime and winpthreads | Windows executables (`-static`) | MinGW-w64 runtime: mostly public domain / ZPL; winpthreads: MIT-style | ship winpthreads' copyright and permission notice with the executable (the MSYS2 package installs it as `share/licenses/winpthreads/COPYING`) |
| KallistiOS | Dreamcast builds only (not part of the prebuilt downloads) | KOS License (BSD-style, below) | give credit in the documentation of a binary |
| mGBA | installed by the toolkit's installers to run ROMs; never shipped with a game | MPL-2.0 | nothing |

A game built with the engine for the GBA must therefore carry, in its credits
or its documentation, a line such as:

> This game is based in part on the work of devkitPro (https://devkitpro.org).

The engine's Starter sample shows that line on its title screen, and the
engine's README says the same.

## libgba licence agreement (excerpt)

The libgba library and included programs are provided under the terms of the
GNU Library General Public License (LGPL) with the following exceptions:

1. Modifications to the build scripts and makefiles by themselves to support
   a specific platform do not constitute a modified or derivative work.
2. Static linking of applications to the libgba library does not constitute a
   derivative work and does not require the author to provide source code for
   the application or link their applications against a user-supplied version
   of libgba. If you link the application to a modified version of libgba,
   then the changes must be provided under the terms of the LGPL in sections
   1, 2, and 4.
3. You do not have to provide a copy of the libgba license with programs that
   are linked to the libgba library, nor do you have to identify the libgba
   license in your program or documentation as required by section 6 of the
   LGPL. However, programs must still identify their use of libgba. The
   following example statement can be included in user documentation to
   satisfy this requirement: "[program/work] is based in part on the work of
   devkitPro (https://devkitpro.org)."

The full agreement ships with libgba (`libgba_license.txt` in devkitPro's
libgba sources).

## SDL2

```
Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

Parts of SDL carry their own BSD-style notices (for example its HIDAPI and
maths code); they are listed in SDL's source release.

## newlib

newlib is a collection of software from several sources; each file carries its
own copyright and licence. Its collected notices (`COPYING.NEWLIB` of newlib
4.4.0, as devkitARM ships it) are in `THIRD_PARTY_NOTICES.newlib.txt` beside
this file.

## devkitARM startup code

`gba_crt0.s` and the GBA link specs come from devkitPro's devkitarm-crtls and
are subject to the Mozilla Public License, v. 2.0. Their source is at
https://github.com/devkitPro/devkitarm-crtls.

## KallistiOS (Dreamcast builds)

```
Copyright (C) 1997-2024 KallistiOS Contributors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of Cryptic Allusion nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

## mGBA

mGBA (https://mgba.io) is licensed under the Mozilla Public License 2.0. The
toolkit's installers install it from the operating system's packages so ROMs
can be run; the engine and the games built with it neither include nor link it.
