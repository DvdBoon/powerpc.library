// Copyright (c) 2019-2021, 2024 Dennis van der Boon
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/initializers.h>

#define VERSION     17
#define REVISION    16

#define LIBNAME "powerpc.library"
#if defined(G4_DSSALL)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 dssall test\r\n"
#elif defined(G4_AS_G3) && defined(G4_FORCE_VEC)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 as G3 VEC on test\r\n"
#elif defined(G4_AS_G3) && defined(G4_KEEP_ALTIVEC)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 as G3 AltiVec on test\r\n"
#elif defined(G4_AS_G3)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 as G3 test\r\n"
#elif defined(G4_FORCE_VEC) && defined(G4_GETINFO_G3)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 VEC on GetInfo G3 test\r\n"
#elif defined(G4_FORCE_VEC) && defined(G4_NO_VMX_COPY)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 VEC on no VMX copy test\r\n"
#elif defined(G4_FORCE_VEC)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 VEC on test\r\n"
#elif defined(G4_EARLY_VEC)
#define VSTRING "$VER: powerpc.library 17.16 (29.10.26) G4 early VEC test\r\n"
#else
#define VSTRING "$VER: powerpc.library 17.16 (31.10.26)\r\n"
#endif

#define VERPCI      13
#define REVPCI      8

#define VERPCIP     4
#define REVPCIP     5

#define LIB_PRIORITY 0

const UWORD LibInitData[] = {
INITBYTE(8, NT_LIBRARY),
INITBYTE(14, LIBF_SUMMING|LIBF_CHANGED),
INITWORD(20, VERSION),
INITWORD(22, REVISION),
(ULONG) 0};

/* The dummy warp.library carries its own version, independent of the defines
 * above. Revision bumped from 1 to 2 with the WarpVectors table growing from
 * four entries to sixty-nine: a warp.library reporting 5.1 never has the
 * address-space functions, one reporting 5.2 always does. */
const UWORD WarpInitData[] = {
INITBYTE(8, NT_LIBRARY),
INITBYTE(14, LIBF_SUMMING|LIBF_CHANGED),
INITWORD(20, 5),
INITWORD(22, 2),
(ULONG) 0};

