## Copyright (c) 2019, 2020, 2024 Dennis van der Boon
##
## Permission is hereby granted, free of charge, to any person obtaining a copy
## of this software and associated documentation files (the "Software"), to deal
## in the Software without restriction, including without limitation the rights
## to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
## copies of the Software, and to permit persons to whom the Software is
## furnished to do so, subject to the following conditions:
##
## The above copyright notice and this permission notice shall be included in all
## copies or substantial portions of the Software.
##
## THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
## IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
## FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
## AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
## LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
## OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
## SOFTWARE.

.include    constantsppc.i

.global     _Exception_Entry, _SmallExcHandler, _DoAlign, _DoDataStore, _FinDataStore
.global     _FlushICache

.section "kernel","acrx"

#********************************************************************************************
#
#
#
#********************************************************************************************

_ExcCommon:

        b       ExcITLBMiss
        b       ExcDLoadTLBMiss
        b       ExcDStoreTLBMiss

        mtsprg2 r0                                         #LR; sprg3 = r0
.ifdef EXC_TIMING
        mftb    r0                                         #T0 entry, untranslated
        stw     r0,ZP_TIME_T0(r0)
.endif

        mfsrr0  r0
        stw     r0,ZP_EXCSRR0(r0)                  #was mtsprg0; SPRG0 is iFUSION's
        mfsrr1  r0
        stw     r0,ZP_EXCSRR1(r0)                  #was mtsprg1; SPRG1 is clobbered by it

#       THE INTERRUPTED TASK'S MMU STATE OUT, THE LIBRARY'S IN. Untranslated:
#       the only free register is r0 (its value is in SPRG3), and CR, CTR and
#       r3 are the task's, so they are parked in page 0x2000 around the SDR1
#       compare. FIXES.md "THE MSR UNDER THE EXCEPTION PATH", 23 Oct 2026.
#
#       From the mtmsr below on, every fetch and every load goes through
#       translation. The original never faces this: its exception path runs
#       untranslated to the rfi (warp.p:3455-3458 sets only FP; powerpc.p has
#       no mtmsr in the path), and on a CyberStorm EA == PA. Here the stack at
#       0(r1), the frame, PowerPCBase and every handler are 68k-view
#       addresses, EA = PA + id_MemBase, so translation is needed -- and it
#       must be the LIBRARY'S. iFUSION enters its guest with every BAT upper
#       zeroed, the guest's SDR1 live and N set in all sixteen segment
#       registers (recon/ppc/mmuswitch.c:133-145, exchandlers.c:698); under
#       that, this routine's own next instruction, at 0x3000 through BAT0,
#       cannot be fetched, and the first exception in guest mode re-enters
#       here for ever.
#
#       The task's sixteen BAT words, sixteen segment registers and SDR1 go
#       to ZP_MMU_TASK_*, which _StoreFrame copies into the frame. The exit
#       loads them back from the frame and installs them after switching
#       translation off, so the task resumes under its own state.
        mfibatu r0,0
        stw     r0,ZP_MMU_TASK_BATS+0(r0)
        mfibatl r0,0
        stw     r0,ZP_MMU_TASK_BATS+4(r0)
        mfdbatu r0,0
        stw     r0,ZP_MMU_TASK_BATS+8(r0)
        mfdbatl r0,0
        stw     r0,ZP_MMU_TASK_BATS+12(r0)
        mfibatu r0,1
        stw     r0,ZP_MMU_TASK_BATS+16(r0)
        mfibatl r0,1
        stw     r0,ZP_MMU_TASK_BATS+20(r0)
        mfdbatu r0,1
        stw     r0,ZP_MMU_TASK_BATS+24(r0)
        mfdbatl r0,1
        stw     r0,ZP_MMU_TASK_BATS+28(r0)
        mfibatu r0,2
        stw     r0,ZP_MMU_TASK_BATS+32(r0)
        mfibatl r0,2
        stw     r0,ZP_MMU_TASK_BATS+36(r0)
        mfdbatu r0,2
        stw     r0,ZP_MMU_TASK_BATS+40(r0)
        mfdbatl r0,2
        stw     r0,ZP_MMU_TASK_BATS+44(r0)
        mfibatu r0,3
        stw     r0,ZP_MMU_TASK_BATS+48(r0)
        mfibatl r0,3
        stw     r0,ZP_MMU_TASK_BATS+52(r0)
        mfdbatu r0,3
        stw     r0,ZP_MMU_TASK_BATS+56(r0)
        mfdbatl r0,3
        stw     r0,ZP_MMU_TASK_BATS+60(r0)
        mfsr    r0,0
        stw     r0,ZP_MMU_TASK_SEGS+0(r0)
        mfsr    r0,1
        stw     r0,ZP_MMU_TASK_SEGS+4(r0)
        mfsr    r0,2
        stw     r0,ZP_MMU_TASK_SEGS+8(r0)
        mfsr    r0,3
        stw     r0,ZP_MMU_TASK_SEGS+12(r0)
        mfsr    r0,4
        stw     r0,ZP_MMU_TASK_SEGS+16(r0)
        mfsr    r0,5
        stw     r0,ZP_MMU_TASK_SEGS+20(r0)
        mfsr    r0,6
        stw     r0,ZP_MMU_TASK_SEGS+24(r0)
        mfsr    r0,7
        stw     r0,ZP_MMU_TASK_SEGS+28(r0)
        mfsr    r0,8
        stw     r0,ZP_MMU_TASK_SEGS+32(r0)
        mfsr    r0,9
        stw     r0,ZP_MMU_TASK_SEGS+36(r0)
        mfsr    r0,10
        stw     r0,ZP_MMU_TASK_SEGS+40(r0)
        mfsr    r0,11
        stw     r0,ZP_MMU_TASK_SEGS+44(r0)
        mfsr    r0,12
        stw     r0,ZP_MMU_TASK_SEGS+48(r0)
        mfsr    r0,13
        stw     r0,ZP_MMU_TASK_SEGS+52(r0)
        mfsr    r0,14
        stw     r0,ZP_MMU_TASK_SEGS+56(r0)
        mfsr    r0,15
        stw     r0,ZP_MMU_TASK_SEGS+60(r0)
        mfsdr1  r0
        stw     r0,ZP_MMU_TASK_SDR1(r0)
        lwz     r0,ZP_MMU_LIB_BATS+0(r0)
        mtibatu 0,r0
        lwz     r0,ZP_MMU_LIB_BATS+4(r0)
        mtibatl 0,r0
        lwz     r0,ZP_MMU_LIB_BATS+8(r0)
        mtdbatu 0,r0
        lwz     r0,ZP_MMU_LIB_BATS+12(r0)
        mtdbatl 0,r0
        lwz     r0,ZP_MMU_LIB_BATS+16(r0)
        mtibatu 1,r0
        lwz     r0,ZP_MMU_LIB_BATS+20(r0)
        mtibatl 1,r0
        lwz     r0,ZP_MMU_LIB_BATS+24(r0)
        mtdbatu 1,r0
        lwz     r0,ZP_MMU_LIB_BATS+28(r0)
        mtdbatl 1,r0
        lwz     r0,ZP_MMU_LIB_BATS+32(r0)
        mtibatu 2,r0
        lwz     r0,ZP_MMU_LIB_BATS+36(r0)
        mtibatl 2,r0
        lwz     r0,ZP_MMU_LIB_BATS+40(r0)
        mtdbatu 2,r0
        lwz     r0,ZP_MMU_LIB_BATS+44(r0)
        mtdbatl 2,r0
        lwz     r0,ZP_MMU_LIB_BATS+48(r0)
        mtibatu 3,r0
        lwz     r0,ZP_MMU_LIB_BATS+52(r0)
        mtibatl 3,r0
        lwz     r0,ZP_MMU_LIB_BATS+56(r0)
        mtdbatu 3,r0
        lwz     r0,ZP_MMU_LIB_BATS+60(r0)
        mtdbatl 3,r0
        lwz     r0,ZP_MMU_LIB_SEGS+0(r0)
        mtsr    0,r0
        lwz     r0,ZP_MMU_LIB_SEGS+4(r0)
        mtsr    1,r0
        lwz     r0,ZP_MMU_LIB_SEGS+8(r0)
        mtsr    2,r0
        lwz     r0,ZP_MMU_LIB_SEGS+12(r0)
        mtsr    3,r0
        lwz     r0,ZP_MMU_LIB_SEGS+16(r0)
        mtsr    4,r0
        lwz     r0,ZP_MMU_LIB_SEGS+20(r0)
        mtsr    5,r0
        lwz     r0,ZP_MMU_LIB_SEGS+24(r0)
        mtsr    6,r0
        lwz     r0,ZP_MMU_LIB_SEGS+28(r0)
        mtsr    7,r0
        lwz     r0,ZP_MMU_LIB_SEGS+32(r0)
        mtsr    8,r0
        lwz     r0,ZP_MMU_LIB_SEGS+36(r0)
        mtsr    9,r0
        lwz     r0,ZP_MMU_LIB_SEGS+40(r0)
        mtsr    10,r0
        lwz     r0,ZP_MMU_LIB_SEGS+44(r0)
        mtsr    11,r0
        lwz     r0,ZP_MMU_LIB_SEGS+48(r0)
        mtsr    12,r0
        lwz     r0,ZP_MMU_LIB_SEGS+52(r0)
        mtsr    13,r0
        lwz     r0,ZP_MMU_LIB_SEGS+56(r0)
        mtsr    14,r0
        lwz     r0,ZP_MMU_LIB_SEGS+60(r0)
        mtsr    15,r0
        isync

#       SDR1 only when it differs, with the TLB flushed the way EnablePageTable
#       does it (warp.p:1697-1704). A task on the library's table pays nothing.
        mfcr    r0
        stw     r0,ZP_MMU_SAVE_CR(r0)
        stw     r3,ZP_MMU_SAVE_R3(r0)
        lwz     r3,ZP_MMU_LIB_SDR1(r0)
        lwz     r0,ZP_MMU_TASK_SDR1(r0)
        cmpw    r0,r3
        beq     .MMUInSame
#       A TAGGED TASK NEEDS NO FLUSH (iFUSION only; FIXES.md "STAGE 3").
#       Its segment registers carry VSID_TAG, a VSID bit the library's never
#       do, so none of its TLB entries can answer a library access, and
#       the library's own entries are still right for the library's table.
#       WarpOS flushes at every table switch (warp.p:1697-1703); this
#       DIVERGES on purpose for the tagged case only. CR is parked; r3
#       is the library's SDR1.
        lwz     r0,ZP_MMU_TASK_SEGS+0(r0)
        andis.  r0,r0,VSID_TAG_HI
        beq     .MMUInFlush
        sync
        mtsdr1  r3
        sync
        isync
        b       .MMUInSame
.MMUInFlush:
        mfctr   r0
        stw     r0,ZP_MMU_SAVE_CTR(r0)
        li      r0,64
        mtctr   r0
        li      r3,0                                       #r3, NOT r0: addi with rA=r0 adds to the literal
.MMUInTLB:                                                 #zero, so "addi r0,r0,0x1000" flushed only EA 0 and
        tlbie   r3                                         #0x1000. warp.p:1697-1703 counts in r3. The task's
        addi    r3,r3,0x1000                               #r3 is parked in ZP_MMU_SAVE_R3 and comes back at
        bdnz    .MMUInTLB                                  #.MMUInSame.
        sync
        lwz     r3,ZP_MMU_LIB_SDR1(r0)
        mtsdr1  r3
        sync
        isync
        lwz     r0,ZP_MMU_SAVE_CTR(r0)
        mtctr   r0
.MMUInSame:
        lwz     r3,ZP_MMU_SAVE_R3(r0)
        lwz     r0,ZP_MMU_SAVE_CR(r0)
        mtcr    r0

        mfmsr   r0
        ori     r0,r0,(PSL_IR|PSL_DR|PSL_FP)
        mtmsr   r0			                   #Reenable MMU
        sync                                               #Also reenable FPU
        isync
.ifdef EXC_TIMING
        mftb    r0                                         #T1 translation on, the library's MMU installed
        stw     r0,ZP_TIME_T1(r0)
.endif

#       THE KERNEL'S OWN STACK, NOT THE INTERRUPTED r1. WarpOS's stubs take
#       theirs from SPRG0 (warp.p:3453 "mfsprg stack,0", the EXCSTACK of
#       warp.p:14 and :171) and never touch the interrupted r1. iFUSION's
#       guest runs with its OWN r1 live, a guest address, which under the
#       library's translation points into the low 2 MB: "stwu r1,-2048(r1)"
#       on it put this frame over page zero and the kernel. The outermost
#       entry takes ZP_EXCSTACK; a nested one stays on the stack it is on,
#       and so does everything until setup has written the word. The back
#       chain at 0(r1) still holds the interrupted r1, which _StoreFrame
#       reads as GPR[1]. FIXES.md "THE LIBRARY HALF OF R", 24 Oct 2026.
        mfcr    r0
        stw     r0,ZP_ENTRY_CR(r0)
        stw     r4,ZP_ENTRY_R4(r0)
        lwz     r4,ZP_EXCDEPTH(r0)
        cmpwi   r4,0
        addi    r4,r4,1
        stw     r4,ZP_EXCDEPTH(r0)
        bne     .Nested
        lwz     r4,ZP_EXCSTACK(r0)
        cmpwi   r4,0
        beq     .Nested
        stw     r1,0(r4)                                   #back chain: the interrupted r1
        mr      r1,r4
        b       .Stacked
.Nested:
        stwu    r1,-2048(r1)
.Stacked:
        lwz     r0,ZP_ENTRY_CR(r0)
        stw     r0,104(r1)
        lwz     r4,ZP_ENTRY_R4(r0)
        stw     r4,108(r1)

        lwz     r4,0x24(r0)
        addi    r4,r4,1
        stw     r4,0x24(r0)

        lwz     r4,POWERPCBASE(r0)
        lwz     r4,POWERPCBASE_THISPPCTASK(r4)
        mr.     r4,r4
        bne     .NoIdl

        lwz     r3,PPCMEMHEADER(r0)
        subi    r4,r3,0x2000
        subi    r3,r4,0x5000
        subi    r3,r3,0x5000                               #coming from idle, just dump everything.
        stw     r4,0(r1)
        mr      r4,r3
        b       .DoExc

.NoIdl: lwz     r4,PPCTASK_CONTEXMEM(r4)                   #iFrame
        lwz     r0,108(r1)
        stw     r0,IF_CONTEXT_GPR+GPR4(r4)                 #GPR[4]
        stw     r3,IF_CONTEXT_GPR+GPR3(r4)                 #GPR[3]
        mfsprg3 r0
        stw     r0,IF_CONTEXT_GPR+GPR0(r4)                 #GPR[0]
        mr      r3,r4

.DoExc: mflr    r0
        stw     r0,IF_CONTEXT(r3)
        bl      _StoreFrame                  #r0, r3 and r4 are skipped in this routine and were saved above

        lwz     r3,POWERPCBASE(r0)                      #Loads PowerPCBase
.ifdef EXC_TIMING
        mftb    r0                                         #T2 frame saved, into the C kernel
        stw     r0,ZP_TIME_T2(r0)
.endif
        bl      _Exception_Entry
.ifdef EXC_TIMING
        mftb    r0                                         #T5 back from the C kernel
        stw     r0,ZP_TIME_T5(r0)
.endif

        lwz     r31,POWERPCBASE(r0)
        lwz     r31,POWERPCBASE_THISPPCTASK(r31)
        mr.     r31,r31
        bne     .NI2                                       #trash everything, is idle task.

        li      r0,0                                       #idle: no task, so no tp_Table
        stw     r0,ZP_MMU_EXIT_TABLE(r0)
        lwz     r31,PPCMEMHEADER(r0)
        subi    r31,r31,0x7000
        subi    r31,r31,0x5000                             #go to idle, don't care about registers
        b       .Idle

.NI2:   lwz     r0,PPCTASK_TABLE(r31)                      #tp_Table: the task's page table, 0 = the library's
        stw     r0,ZP_MMU_EXIT_TABLE(r0)
        lwz     r31,PPCTASK_CONTEXMEM(r31)
.Idle:  mr      r1,r31

        bl      _LoadFrame                   #LR, r0,r1 and r3 are skipped in this routine and are loaded below

.ifdef EXC_TIMING
        mftb    r0                                         #T6 frame restored
        stw     r0,ZP_TIME_T6(r0)
.endif
        bl      _FlushICache
.ifdef EXC_TIMING
        mftb    r0                                         #T7 after the instruction-cache flush
        stw     r0,ZP_TIME_T7(r0)
.endif

#       THE TASK'S MMU STATE BACK, WITH TRANSLATION OFF. Everything the exit
#       still needs from the frame -- CR, CTR, r0, r1, r3 -- is staged in page
#       0x2000 first, because the frame is a 68k-view address and is out of
#       reach once translation is off. _LoadFrame has already put the frame's
#       sixteen BAT words, sixteen segment registers and SDR1 in ZP_MMU_TASK_*.
#       The registers are then written while nothing translates through them,
#       which is also why the original zeroes and reloads its BATs where it
#       does (powerpc.p:5034-5084): with translation off, the order of the
#       upper and lower words of a BAT pair cannot matter.
        lwz     r0,IF_CONTEXT_CR(r1)
        stw     r0,ZP_MMU_EXIT_CR(r0)
        mfctr   r0
        stw     r0,ZP_MMU_SAVE_CTR(r0)
        lwz     r0,IF_CONTEXT_GPR+GPR3(r1)                 #GPR[3]
        stw     r0,ZP_MMU_EXIT_R3(r0)
        lwz     r0,IF_CONTEXT_GPR+GPR0(r1)                 #GPR[0]
        stw     r0,ZP_MMU_EXIT_R0(r0)
        lwz     r0,IF_CONTEXT_GPR+GPR1(r1)                 #GPR[1]
        stw     r0,ZP_MMU_EXIT_R1(r0)

#       Which page table the task resumes under: tp_Table if it set one
#       (tasksPPC.h:38; iFUSION writes it at 0x01F5D8 and 0x01FA88), else the
#       SDR1 it was interrupted with, else the library's own. tp_Table is the
#       bare HTABORG, so the HTABMASK is taken from the library's SDR1, as
#       EnablePageTable derives it from PPC_TABLESIZE (powerpc.p:5015,
#       warp.p:1688-1690).
        lwz     r3,ZP_MMU_EXIT_TABLE(r0)
        mr.     r3,r3
        beq     .ExitNoTable
        lwz     r0,ZP_MMU_LIB_SDR1(r0)
        andi.   r0,r0,0x1ff
        or      r3,r3,r0
        b       .ExitSDR1
.ExitNoTable:
        lwz     r3,ZP_MMU_TASK_SDR1(r0)
        mr.     r3,r3
        bne     .ExitSDR1
        lwz     r3,ZP_MMU_LIB_SDR1(r0)
.ExitSDR1:
        stw     r3,ZP_MMU_TASK_SDR1(r0)

        mfmsr   r0
        ori     r0,r0,(PSL_IR|PSL_DR)
        xori    r0,r0,(PSL_IR|PSL_DR)                      #translation off, as warp.p:1692-1694
        mtmsr   r0
        isync

        lwz     r0,ZP_MMU_TASK_BATS+0(r0)
        mtibatu 0,r0
        lwz     r0,ZP_MMU_TASK_BATS+4(r0)
        mtibatl 0,r0
        lwz     r0,ZP_MMU_TASK_BATS+8(r0)
        mtdbatu 0,r0
        lwz     r0,ZP_MMU_TASK_BATS+12(r0)
        mtdbatl 0,r0
        lwz     r0,ZP_MMU_TASK_BATS+16(r0)
        mtibatu 1,r0
        lwz     r0,ZP_MMU_TASK_BATS+20(r0)
        mtibatl 1,r0
        lwz     r0,ZP_MMU_TASK_BATS+24(r0)
        mtdbatu 1,r0
        lwz     r0,ZP_MMU_TASK_BATS+28(r0)
        mtdbatl 1,r0
        lwz     r0,ZP_MMU_TASK_BATS+32(r0)
        mtibatu 2,r0
        lwz     r0,ZP_MMU_TASK_BATS+36(r0)
        mtibatl 2,r0
        lwz     r0,ZP_MMU_TASK_BATS+40(r0)
        mtdbatu 2,r0
        lwz     r0,ZP_MMU_TASK_BATS+44(r0)
        mtdbatl 2,r0
        lwz     r0,ZP_MMU_TASK_BATS+48(r0)
        mtibatu 3,r0
        lwz     r0,ZP_MMU_TASK_BATS+52(r0)
        mtibatl 3,r0
        lwz     r0,ZP_MMU_TASK_BATS+56(r0)
        mtdbatu 3,r0
        lwz     r0,ZP_MMU_TASK_BATS+60(r0)
        mtdbatl 3,r0
        lwz     r0,ZP_MMU_TASK_SEGS+0(r0)
        mtsr    0,r0
        lwz     r0,ZP_MMU_TASK_SEGS+4(r0)
        mtsr    1,r0
        lwz     r0,ZP_MMU_TASK_SEGS+8(r0)
        mtsr    2,r0
        lwz     r0,ZP_MMU_TASK_SEGS+12(r0)
        mtsr    3,r0
        lwz     r0,ZP_MMU_TASK_SEGS+16(r0)
        mtsr    4,r0
        lwz     r0,ZP_MMU_TASK_SEGS+20(r0)
        mtsr    5,r0
        lwz     r0,ZP_MMU_TASK_SEGS+24(r0)
        mtsr    6,r0
        lwz     r0,ZP_MMU_TASK_SEGS+28(r0)
        mtsr    7,r0
        lwz     r0,ZP_MMU_TASK_SEGS+32(r0)
        mtsr    8,r0
        lwz     r0,ZP_MMU_TASK_SEGS+36(r0)
        mtsr    9,r0
        lwz     r0,ZP_MMU_TASK_SEGS+40(r0)
        mtsr    10,r0
        lwz     r0,ZP_MMU_TASK_SEGS+44(r0)
        mtsr    11,r0
        lwz     r0,ZP_MMU_TASK_SEGS+48(r0)
        mtsr    12,r0
        lwz     r0,ZP_MMU_TASK_SEGS+52(r0)
        mtsr    13,r0
        lwz     r0,ZP_MMU_TASK_SEGS+56(r0)
        mtsr    14,r0
        lwz     r0,ZP_MMU_TASK_SEGS+60(r0)
        mtsr    15,r0
        isync

        mfsdr1  r0
        cmpw    r0,r3
        beq     .ExitSame
        lwz     r0,ZP_MMU_TASK_SEGS+0(r0)                  #a tagged task: no flush, as at entry
        andis.  r0,r0,VSID_TAG_HI
        beq     .ExitFlush
        sync
        mtsdr1  r3                                         #r3 is the task's SDR1, from .ExitSDR1
        sync
        isync
        b       .ExitSame
.ExitFlush:
        li      r0,64
        mtctr   r0
        li      r3,0                                       #r3, NOT r0, as at .MMUInTLB
.ExitTLB:
        tlbie   r3
        addi    r3,r3,0x1000
        bdnz    .ExitTLB
        sync
        lwz     r3,ZP_MMU_TASK_SDR1(r0)                     #the SDR1 chosen at .ExitSDR1
        mtsdr1  r3
        sync
        isync
.ExitSame:
        lwz     r0,ZP_MMU_SAVE_CTR(r0)
        mtctr   r0
        lwz     r3,ZP_EXCDEPTH(r0)                          #one level up; the stack is simply abandoned.
        cmpwi   r3,0                                       #An exit without an entry (setup's own first rfi)
        beq     .DepthZero                                 #leaves it at 0. r3, NOT r0: "subi r0,r0,1" is
        subi    r3,r3,1                                    #"li r0,-1", which made every second exception
        stw     r3,ZP_EXCDEPTH(r0)                          #skip the kernel stack. r3 is reloaded below.
.DepthZero:
        lwz     r0,ZP_MMU_EXIT_CR(r0)
        mtcr    r0
        lwz     r3,ZP_MMU_EXIT_R3(r0)
        lwz     r0,ZP_MMU_EXIT_R0(r0)
        mtsprg2 r0
        lwz     r1,ZP_MMU_EXIT_R1(r0)

        lwz     r0,ZP_EXCSRR0(r0)                  #rA=0 is the literal zero
        mtsrr0  r0
        lwz     r0,ZP_EXCSRR1(r0)
        mtsrr1  r0
.ifdef EXC_TIMING
        mftb    r0                                         #T8 task MMU and registers back, at the rfi
        stw     r0,ZP_TIME_T8(r0)
.endif
        mfsprg3 r0
        mtlr    r0
        mfsprg2 r0

        rfi

#********************************************************************************************

_StoreFrame:

        stfd    f0,IF_CONTEXT_FPR(r3)
        lwz     r0,ZP_EXCSRR0(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_EXCSRR1(r0)
        stwu    r0,4(r3)
        mfdar   r0
        stwu    r0,4(r3)
        mfdsisr r0
        stwu    r0,4(r3)
        lwz     r0,104(r1)                   #cr
        stwu    r0,4(r3)
        mfctr   r0
        stwu    r0,4(r3)
        mfsprg2 r0
        stwu    r0,4(r3)                     #lr
        mffs    f0
        stfdu   f0,4(r3)
        mfxer   r0
        stw     r0,0(r3)

        lwz     r0,0(r1)
        stwu    r0,12(r3)                    #r1, skipped r0
        stwu    r2,4(r3)
        stwu    r5,12(r3)                    #skipped r3 and r4. Need to be stored seperately
        stwu    r6,4(r3)
        stwu    r7,4(r3)
        stwu    r8,4(r3)
        stwu    r9,4(r3)
        stwu    r10,4(r3)
        stwu    r11,4(r3)
        stwu    r12,4(r3)
        stwu    r13,4(r3)
        stwu    r14,4(r3)
        stwu    r15,4(r3)
        stwu    r16,4(r3)
        stwu    r17,4(r3)
        stwu    r18,4(r3)
        stwu    r19,4(r3)
        stwu    r20,4(r3)
        stwu    r21,4(r3)
        stwu    r22,4(r3)
        stwu    r23,4(r3)
        stwu    r24,4(r3)
        stwu    r25,4(r3)
        stwu    r26,4(r3)
        stwu    r27,4(r3)
        stwu    r28,4(r3)
        stwu    r29,4(r3)
        stwu    r30,4(r3)
        stwu    r31,4(r3)

        stfdu   f1,12(r3)                    #skipped f0 (see above)
        stfdu   f2,8(r3)
        stfdu   f3,8(r3)
        stfdu   f4,8(r3)
        stfdu   f5,8(r3)
        stfdu   f6,8(r3)
        stfdu   f7,8(r3)
        stfdu   f8,8(r3)
        stfdu   f9,8(r3)
        stfdu   f10,8(r3)
        stfdu   f11,8(r3)
        stfdu   f12,8(r3)
        stfdu   f13,8(r3)
        stfdu   f14,8(r3)
        stfdu   f15,8(r3)
        stfdu   f16,8(r3)
        stfdu   f17,8(r3)
        stfdu   f18,8(r3)
        stfdu   f19,8(r3)
        stfdu   f20,8(r3)
        stfdu   f21,8(r3)
        stfdu   f22,8(r3)
        stfdu   f23,8(r3)
        stfdu   f24,8(r3)
        stfdu   f25,8(r3)
        stfdu   f26,8(r3)
        stfdu   f27,8(r3)
        stfdu   f28,8(r3)
        stfdu   f29,8(r3)
        stfdu   f30,8(r3)
        stfdu   f31,8(r3)

        lwz     r0,ZP_EXCSRR1(r0)                  #SRR1, for the VEC bit
        andis.  r0,r0,PSL_VEC@h
        bne     .DoVMX
        addi    r3,r3,8+8+512+16           #f31, alignstore, vectors, vscr
        b       .NoVMX

.DoVMX: mfmsr   r0
        oris    r0,r0,PSL_VEC@h
        mtmsr   r0
        isync

        addi    r3,r3,32
        stvx	v0,r0,r3
        subi    r3,r3,16
        mfvscr  v0
        stvx    v0,r0,r3
        addi    r3,r3,32
		stvx	v1,r0,r3
		addi	r3,r3,16
		stvx	v2,r0,r3
		addi	r3,r3,16
		stvx	v3,r0,r3
		addi	r3,r3,16
		stvx	v4,r0,r3
		addi	r3,r3,16
		stvx	v5,r0,r3
		addi	r3,r3,16
		stvx	v6,r0,r3
		addi	r3,r3,16
		stvx	v7,r0,r3
		addi	r3,r3,16
		stvx	v8,r0,r3
		addi	r3,r3,16
		stvx	v9,r0,r3
		addi	r3,r3,16
		stvx	v10,r0,r3
		addi	r3,r3,16
		stvx	v11,r0,r3
		addi	r3,r3,16
		stvx	v12,r0,r3
		addi	r3,r3,16
		stvx	v13,r0,r3
		addi	r3,r3,16
		stvx	v14,r0,r3
		addi	r3,r3,16
		stvx	v15,r0,r3
		addi	r3,r3,16
		stvx	v16,r0,r3
		addi	r3,r3,16
		stvx	v17,r0,r3
		addi	r3,r3,16
		stvx	v18,r0,r3
		addi	r3,r3,16
		stvx	v19,r0,r3
		addi	r3,r3,16
		stvx	v20,r0,r3
		addi	r3,r3,16
		stvx	v21,r0,r3
		addi	r3,r3,16
		stvx	v22,r0,r3
		addi	r3,r3,16
		stvx	v23,r0,r3
		addi	r3,r3,16
		stvx	v24,r0,r3
		addi	r3,r3,16
		stvx	v25,r0,r3
		addi	r3,r3,16
		stvx	v26,r0,r3
		addi	r3,r3,16
		stvx	v27,r0,r3
		addi	r3,r3,16
		stvx	v28,r0,r3
		addi	r3,r3,16
		stvx	v29,r0,r3
		addi	r3,r3,16
		stvx	v30,r0,r3
		addi	r3,r3,16
		stvx	v31,r0,r3
		mfspr	r0,VRSAVE
		stwu    r0,16(r3)

.NoVMX: lwz     r0,ZP_MMU_TASK_BATS+0(r0)            #the task's, captured at entry
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+4(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+8(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+12(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+16(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+20(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+24(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+28(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+32(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+36(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+40(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+44(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+48(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+52(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+56(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_BATS+60(r0)
        stwu    r0,4(r3)

        lwz     r0,ZP_MMU_TASK_SEGS+0(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+4(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+8(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+12(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+16(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+20(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+24(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+28(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+32(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+36(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+40(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+44(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+48(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+52(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+56(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SEGS+60(r0)
        stwu    r0,4(r3)
        lwz     r0,ZP_MMU_TASK_SDR1(r0)
        stwu    r0,8(r3)                     #if_SDR1, past if_ExcNum

        blr

#********************************************************************************************

_LoadFrame:
        lwzu    r3,4(r31)
        stw     r3,ZP_EXCSRR0(r0)                  #was mtsprg0
        lwzu    r3,4(r31)
        stw     r3,ZP_EXCSRR1(r0)                  #was mtsprg1
        lwzu    r3,4(r31)
        mtdar   r3
        lwzu    r3,4(r31)
        mtdsisr r3
        lwzu    r3,8(r31)                    #skip cr
        mtctr   r3
        lwzu    r3,4(r31)
        mtsprg3 r3
        lwzu    r3,4(r31)
        mtxer   r3
        lfd     f0,0(r31)
        mtfsf   0xff,f0

        lwzu    r2,16(r31)                   #skip r0-r1, is loaded seperately
        lwzu    r4,8(r31)                    #skip r3, is loaded seperately
        lwzu    r5,4(r31)
        lwzu    r6,4(r31)
        lwzu    r7,4(r31)
        lwzu    r8,4(r31)
        lwzu    r9,4(r31)
        lwzu    r10,4(r31)
        lwzu    r11,4(r31)
        lwzu    r12,4(r31)
        lwzu    r13,4(r31)
        lwzu    r14,4(r31)
        lwzu    r15,4(r31)
        lwzu    r16,4(r31)
        lwzu    r17,4(r31)
        lwzu    r18,4(r31)
        lwzu    r19,4(r31)
        lwzu    r20,4(r31)
        lwzu    r21,4(r31)
        lwzu    r22,4(r31)
        lwzu    r23,4(r31)
        lwzu    r24,4(r31)
        lwzu    r25,4(r31)
        lwzu    r26,4(r31)
        lwzu    r27,4(r31)
        lwzu    r28,4(r31)
        lwzu    r29,4(r31)
        lwzu    r30,4(r31)
        lwzu    r0,4(r31)                    #temp store r31 in r0

        lfdu    f0,4(r31)
        lfdu    f1,8(r31)
        lfdu    f2,8(r31)
        lfdu    f3,8(r31)
        lfdu    f4,8(r31)
        lfdu    f5,8(r31)
        lfdu    f6,8(r31)
        lfdu    f7,8(r31)
        lfdu    f8,8(r31)
        lfdu    f9,8(r31)
        lfdu    f10,8(r31)
        lfdu    f11,8(r31)
        lfdu    f12,8(r31)
        lfdu    f13,8(r31)
        lfdu    f14,8(r31)
        lfdu    f15,8(r31)
        lfdu    f16,8(r31)
        lfdu    f17,8(r31)
        lfdu    f18,8(r31)
        lfdu    f19,8(r31)
        lfdu    f20,8(r31)
        lfdu    f21,8(r31)
        lfdu    f22,8(r31)
        lfdu    f23,8(r31)
        lfdu    f24,8(r31)
        lfdu    f25,8(r31)
        lfdu    f26,8(r31)
        lfdu    f27,8(r31)
        lfdu    f28,8(r31)
        lfdu    f29,8(r31)
        lfdu    f30,8(r31)
        lfdu    f31,8(r31)

        lwz     r3,ZP_EXCSRR1(r0)                  #SRR1, for the VEC bit
        andis.  r3,r3,PSL_VEC@h
        bne     .DoAV
        addi    r31,r31,8+8+512+16
        b       .NoAV

.DoAV:  mfmsr   r3
        oris    r3,r3,PSL_VEC@h
        mtmsr   r3
        isync

        addi    r31,r31,16
        lvx     v0,r0,r31
        mtvscr  v0
        addi    r31,r31,16
        lvx     v0,r0,r31
		addi    r31,r31,16
		lvx     v1,r0,r31
		addi    r31,r31,16
		lvx     v2,r0,r31
		addi    r31,r31,16
		lvx     v3,r0,r31
		addi    r31,r31,16
		lvx     v4,r0,r31
		addi    r31,r31,16
		lvx     v5,r0,r31
		addi    r31,r31,16
		lvx     v6,r0,r31
		addi    r31,r31,16
		lvx     v7,r0,r31
		addi    r31,r31,16
		lvx     v8,r0,r31
		addi    r31,r31,16
		lvx     v9,r0,r31
		addi    r31,r31,16
		lvx     v10,r0,r31
		addi    r31,r31,16
		lvx     v11,r0,r31
		addi    r31,r31,16
		lvx     v12,r0,r31
		addi    r31,r31,16
		lvx     v13,r0,r31
		addi    r31,r31,16
		lvx     v14,r0,r31
		addi    r31,r31,16
		lvx     v15,r0,r31
		addi    r31,r31,16
		lvx     v16,r0,r31
		addi    r31,r31,16
		lvx     v17,r0,r31
		addi    r31,r31,16
		lvx     v18,r0,r31
		addi    r31,r31,16
		lvx     v19,r0,r31
		addi    r31,r31,16
		lvx     v20,r0,r31
		addi    r31,r31,16
		lvx     v21,r0,r31
		addi    r31,r31,16
		lvx     v22,r0,r31
		addi    r31,r31,16
		lvx     v23,r0,r31
		addi    r31,r31,16
		lvx     v24,r0,r31
		addi    r31,r31,16
		lvx     v25,r0,r31
		addi    r31,r31,16
		lvx     v26,r0,r31
		addi    r31,r31,16
		lvx     v27,r0,r31
		addi    r31,r31,16
		lvx     v28,r0,r31
		addi    r31,r31,16
		lvx     v29,r0,r31
		addi    r31,r31,16
		lvx     v30,r0,r31
        addi    r31,r31,16
        lvx     v31,r0,r31
        lwzu    r3,16(r31)
		mtspr	VRSAVE,r3

.NoAV:  lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+0(r0)            #staged; the exit installs them untranslated
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+4(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+8(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+12(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+16(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+20(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+24(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+28(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+32(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+36(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+40(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+44(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+48(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+52(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+56(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_BATS+60(r0)

        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+0(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+4(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+8(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+12(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+16(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+20(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+24(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+28(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+32(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+36(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+40(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+44(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+48(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+52(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+56(r0)
        lwzu    r3,4(r31)
        stw     r3,ZP_MMU_TASK_SEGS+60(r0)
        lwzu    r3,8(r31)                    #if_SDR1, past if_ExcNum
        stw     r3,ZP_MMU_TASK_SDR1(r0)

        mr      r31,r0
        blr

#********************************************************************************************
#
#
#
#********************************************************************************************

_SmallExcHandler:  #debugdebug to be redone

        stwu    r1,-512(r1)                  #Enough?

#       SAVE THE CALLER'S NON-VOLATILES. _LoadFrame below loads r14-r30 from
#       the frame and uses r31 as its walking pointer, and _StoreFrame stores
#       r14-r31, so between them every one of this function's caller's
#       non-volatile registers is replaced by the INTERRUPTED PROGRAM's value.
#       CommonExcHandler, the caller, keeps iframe in r27, PowerPCBase in r24
#       and currNode/nxtNode in r28/r29.
#
#       Reported as a data access exception at _CommonExcHandler+0x12C,
#       "lwz r11,0(r27)" -- the "ec_ExcID != EXCF_DACCESS" test of
#       kernel.c:912 -- with r27 holding an Amiga address. Reading ec_ExcID out
#       of the iframe is a PPC-memory read and cannot fault, so the pointer was
#       wrong rather than the access.
#
#       It hid behind a short-circuit: a handler returning EXCRETURN_ABORT
#       leaves status non-zero, so that test's "bne" skips the only use of r27
#       after the call, and "break" leaves the loop before currNode is read
#       again. One ABORT handler therefore passes while every register is
#       still wrong. tests/smallexc registers two handlers to defeat that.
#
#       The interrupted values stay live in r14-r31 through the middle of this
#       function, which is what _StoreFrame needs to write them back; only the
#       caller's copies are parked here. 340+72 = 412, inside the 512-byte
#       frame, and clear of 300-336.
#
#       stmw/lmw ARE SAFE HERE, and the question has been settled rather than
#       assumed. They are base 32-bit PowerPC Book I, not an optional
#       extension: vasm accepts them under -mppc32, -m603e, -m750 and -m7400.
#       The form is well-made too -- the EA is word-aligned, and rA is r1,
#       outside the r14-r31 range, an lmw whose base register falls inside the
#       loaded range being an invalid form.
#
#       The original uses them NOWHERE in powerpc.p, warp.p or the includes,
#       and that is NOT evidence against them: WarpOS ran on Phase5 603e/604e
#       boards -- warpos/blackbox/ holds BlizzardPPC, CyberStormPPC and
#       P5DeveloperBoard -- and never on the G3/G4 this library targets. So its
#       silence says nothing about these cores. They are microcoded on
#       603e/750/74xx, which costs cycles on this path; explicit stw/lwz
#       sequences would avoid that at ~136 bytes, and were weighed and
#       declined.
        stmw    r14,340(r1)

        mr      r31,r4
        stw     r4,316(r1)                   #iframe. WAS mtsprg0, which _LoadFrame
        stw     r3,320(r1)                   #ExcData. WAS mtsprg3, which it also takes
        mflr    r3
        stw     r3,-12(r4)                   #Store this function's lr

        bl      _LoadFrame                   #LR, r0, r1 and r3 are skipped in this routine and are loaded below

        lwz     r4,316(r1)                   #iframe, intact across _LoadFrame
        lwz     r3,IF_CONTEXT_GPR+GPR3(r4)   #GPR[3]
        stw     r3,SEH_XCONTEXT+XCO_R3(r1)   #+4, per powerpc.i:233
        lwz     r3,IF_CONTEXT_EXCID(r4)      #the exception ID, kernel.c:49
        stw     r3,SEH_XCONTEXT+XCO_EXCID(r1) #+0. THIS SLOT HELD r3 BEFORE, and
                                             #+4 held a status word not written
                                             #until after the handler returned,
                                             #so iFUSION's exc_sc read nothing
                                             #useful at 4(r3). FIXES.md item 11
        lwz     r3,320(r1)                   #ExcData
        lwz     r0,IF_CONTEXT_LR(r4)         #EXC_LR
        stw     r0,324(r1)
        stw     r0,-8(r4)                    #Will be used later in StoreFrame
        lwz     r0,IF_CONTEXT_GPR+GPR1(r4)   #GPR[1]
        stw     r0,328(r1)
        stw     r2,-16(r4)
        lwz     r0,IF_CONTEXT_GPR+GPR2(r4)   #GPR[2]
        lwz     r2,18(r3)                    #DATA   (ed_Data; Node is 14 under pack(2))
        stw     r0,332(r1)
        lwz     r0,14(r3)                    #CODE
        mtlr    r0
        la      r3,SEH_XCONTEXT(r1)          #XCONTEXT
        stw     r4,308(r1)
        lwz     r0,IF_CONTEXT_GPR+GPR0(r4)   #GPR[0]
        lwz     r4,IF_CONTEXT_GPR+GPR4(r4)   #GPR[4]

        blrl

        stw     r3,SEH_STATUS(r1)            #Status. WAS 304, which is XCO_R3 now
        lwz     r3,SEH_XCONTEXT+XCO_R3(r1)   #r3, as the handler may have changed it
        stw     r4,312(r1)                   #Temp store r4
        lwz     r4,316(r1)                   #iframe
        stw     r3,IF_CONTEXT_GPR+GPR3(r4)   #GPR[3]
        stw     r0,IF_CONTEXT_GPR+GPR0(r4)   #GPR[0]

#       _StoreFrame READS THREE THINGS FROM ITS CALLER and this function
#       supplied none of them. _ExcCommon does, which is why the common path
#       works and this one faulted at _StoreFrame's first instruction:
#
#         r3        the iframe, which it walks with stwu. HERE IT HELD THE
#                   HANDLER'S RETURNED r3, with the iframe in r4, so
#                   "stfd f0,IF_CONTEXT_FPR(r3)" stored through the handler's
#                   return value. tests/smallexc found it as an alignment
#                   exception at that instruction with r3 = its R3_OUT magic.
#         104(r1)   the CR to write into IF_CONTEXT_CR. This function's frame
#                   has nothing there, so the frame's CR was overwritten with
#                   whatever that slot held. The XCONTEXT exposes only the
#                   exception ID and r3, so a SMALLCONTEXT handler has no
#                   sanctioned way to change CR; reloading the frame's own
#                   value makes _StoreFrame write it back unchanged.
#         0(r1)     the interrupted code's r1. Wrong here too -- it is this
#                   function's back-chain -- but harmless, because the
#                   epilogue below overwrites GPR[1] from 328(r1) afterwards.
#                   Left alone: 0(r1) is what "lwz r1,0(r1)" returns on.
#
#       r0 is free, GPR[0] having just been stored, and r3 is free, GPR[3]
#       having been stored on the line above.

        lwz     r0,IF_CONTEXT_CR(r4)         #preserve the frame's CR
        stw     r0,104(r1)
        mr      r3,r4                        #_StoreFrame walks the iframe in r3

        bl      _StoreFrame

        lwz     r4,312(r1)                   #Really store r4
        lwz     r5,316(r1)                   #iframe
        stw     r4,IF_CONTEXT_GPR+GPR4(r5)   #GPR[4]
        lwz     r6,324(r1)                   #EXC_LR
        lwz     r4,-12(r5)                   #Retrieve this function's lr
        lwz     r2,-16(r5)
        stw     r6,IF_CONTEXT_LR(r5)         #EXC_LR
        lwz     r7,328(r1)                   #GPR[1]
        mtlr    r4
        lwz     r6,332(r1)                   #GPR[2]
        stw     r7,IF_CONTEXT_GPR+GPR1(r5)   #GPR[1]
        lwz     r3,SEH_STATUS(r1)            #Return Status
        stw     r6,IF_CONTEXT_GPR+GPR2(r5)   #GPR[2]

#       Restore the caller's non-volatiles. Everything above uses r0-r7 and r2
#       only, all volatile or separately restored, so this is the last moment
#       they are needed and the first at which they may be overwritten.
        lmw     r14,340(r1)

        lwz     r1,0(r1)

        blr

#********************************************************************************************
#
#
#
#********************************************************************************************

_DoAlign:

        lwz     r5,0(r4)
        la      r11,IF_CONTEXT_GPR(r3)       #reg table
        la      r12,IF_CONTEXT_FPR(r3)       #freg table

        rlwinm  r6,r5,14,24,28               #get floating point register offset
        rlwinm  r7,r5,18,25,29               #get destination register offset
        mr.     r10,r7
        beq     .ItsR0
        lwzx    r10,r11,r7                   #get address from destination register

.ItsR0:	
        rlwinm  r8,r5,0,16,31                #get displacement
        rlwinm  r0,r5,6,26,31
        cmpwi   r0,0x34                      #test for stfs
        beq     .stfs
        cmpwi   r0,0x35
        beq     .stfsu
        cmpwi   r0,0x30
        beq     .lfs
        cmpwi   r0,0x31
        beq     .lfsu
        cmpwi   r0,0x1f
        beq     .lstfsx
        cmpwi   r0,0x32
        beq     .lfd
        cmpwi   r0,0x36
        beq     .stfd
        b       .HaltAlign

.stfd:		
        lwzx    r9,r12,r6
        stwx    r9,r10,r8
        addi    r6,r6,4
        addi    r8,r8,4
        lwzx    r9,r12,r6
        stwx    r9,r10,r8
        b       .AligExit

.stfsu:		
        add     r9,r10,r8
        stwx    r9,r11,r7

.stfs:		
        lfdx    f1,r12,r6                    #get value from fp register
        stfs    f1,IF_ALIGNSTORE(r3)         #store it on correct aligned spot
        lwz     r6,IF_ALIGNSTORE(r3)         #Get the correct 32 bit value
        stwx    r6,r10,r8                    #Store correct value
        b       .AligExit

.lfd:
        lwzx    r9,r10,r8
        stw     r9,IF_ALIGNSTORE(r3)
        addi    r8,r8,4
        lwzx    r9,r10,r8
        stw     r9,IF_ALIGNSTORE+4(r3)
        lfd     f1,IF_ALIGNSTORE(r3)
        stfdx   f1,r12,r6
        b       .AligExit

.lfsu:	
        add     r5,r10,r8                    #Add displacement
        stwx    r5,r11,r7

.lfs:	
        lwzx    r9,r10,r8                    #Get 32 bit value
        stw     r9,IF_ALIGNSTORE(r3)         #Store it on aligned spot
        lfs     f1,IF_ALIGNSTORE(r3)         #Get it and convert it to 64 bit
        stfdx   f1,r12,r6                    #Store the 64 bit value
        b       .AligExit

.lstfsx:
        rlwinm. r0,r5,25,31,31               #0 = s; 1 = d
        bne     .HaltAlign
        rlwinm. r0,r5,26,31,31               #0 = x; 1 = ux
        bne     .HaltAlign

        rlwinm  r8,r5,23,25,29               #get index register
        lwzx    r8,r11,r8                    #get index register value
        rlwinm. r0,r5,24,31,31
        bne     .stfs
        b       .lfs

.HaltAlign:
        li      r3,0
        blr
.AligExit:
        li      r3,1
        blr

#********************************************************************************************
#
#
#
#********************************************************************************************

_DoDataStore:

        li      r12,0                        #r3 = iframe, r4 = srr0, r5 = datastruct
        stw     r12,IF_ALIGNSTORE(r3)        #to get an offset of 0 (reusing an unused variable)
        la      r11,IF_CONTEXT_GPR(r3)       #get reg table in r11
        lwz     r12,0(r4)                    #get offending instruction in r12
        li      r10,0
        lis     r0,0xc000                    #check for load or store instruction
        mr      r7,r5
        and.    r0,r12,r0
        lis     r6,0x8000
        cmpw    r6,r0
        beq     .LoadStore

        rlwinm  r6,r12,0,25,5
        loadreg r8,0x7c00002e                #check for stbx/sthx/stwx/lbzx/lhzx/lwzx
        cmpw    r6,r8
        bne     .NotSupported

        li      r10,1
        rlwinm  r6,r12,13,25,29              #Source reg
        rlwinm  r8,r12,18,25,29              #Dest 1
        rlwinm  r4,r12,23,25,29              #Dest 2
        lwzx    r4,r11,r4                    #Displacement (word)
        li      r5,0                         #Update bit
        li      r9,1                         #Mark as Store instruction

        b       .GoStore

.LoadStore:	
        rlwinm  r6,r12,13,25,29              #Get Destination Reg (l) or Source (s)
        rlwinm  r8,r12,18,25,29              #Get Source Reg (l) or Destination (s)
        rlwinm  r9,r12,4,31,31               #Check load or store
        rlwinm  r5,r12,6,31,31               #Check for update bit
        rlwinm  r4,r12,0,16,31               #Displacement (halfword)
        extsh   r4,r4                        #Extend sign of displacement

.GoStore:
        mr.     r5,r5
        bne     .NoZero                      #When update r0 = r0 and not 0
        mr.     r8,r8
        bne     .NoZero
        li      r8,IF_ALIGNSTORE-IF_CONTEXT_GPR             #Point to 0 in frame

.NoZero:
        lwzx    r3,r11,r8                    #Get Destination Address
        add     r3,r3,r4                     #Add displacement
        mr.     r5,r5
        beq     .NoUpdate
        stwx    r3,r11,r8                    #Update Destination Reg

.NoUpdate:
        lwzx    r5,r11,r6                    #Get value to store
        mr.     r9,r9
        beq     .GoLoad                      #Load or store?

        mr.     r10,r10                      #Normal store or with index?
        beq     .NoStxx

        rlwinm  r0,r12,25,29,31              #Indexed store
        cmpwi   r0,6
        beq     .StoreHalf
        cmpwi   r0,3
        beq     .StoreByte
        cmpwi   r0,2
        beq     .StoreWord
        mr.     r0,r0
        beq     .GoLoadx                     #Lwzx
        cmpwi   r0,1
        beq     .GoLoadx                     #lbzx
        cmpwi   r0,4
        beq     .GoLoadx                     #lhzx
        cmpwi   r0,5
        beq     .GoLoadx                     #lhax
        b       .NotSupported                #Not Supported

.NoStxx:
        rlwinm. r0,r12,3,31,31               #Normal store
        bne     .StoreHalf
        rlwinm. r0,r12,5,31,31
        bne     .StoreByte

.StoreWord:	
        loadreg r9,'PUTW'
        b       .DoStore

.StoreHalf:	
        loadreg r9,'PUTH'
        b       .DoStore

.StoreByte:	
        loadreg r9,'PUTB'

.DoStore:
        stw     r9,0(r7)
        stw     r5,4(r7)
        stw     r3,8(r7)
        li      r4,1
        stw     r4,20(r7)
        b       .DoneDSI                     #We're done

.GoLoad:
        li      r5,0
        mr      r0,r5
        b       .NoX

.GoLoadx:	
        li      r5,1
.NoX:   stw     r0,12(r7)
        stw     r6,16(r7)
        loadreg r9,'GETV'
        stw     r9,0(r7)
        stw     r3,8(r7)
        li      r4,0
        stw     r4,20(r7)
        stw     r5,24(r7)
        b      .DoneDSI

.NotSupported:
        li      r3,0
        blr

#********************************************************************************************

_FinDataStore:
        lwz     r12,0(r5)                    #r3 = value, r4 = iframe, r5 = srr0, r6 = data
        la      r11,IF_CONTEXT_GPR(r4)
        lwz     r9,24(r6)
        lwz     r4,12(r6)
        mr      r10,r3
        lwz     r6,16(r6)
        mr.     r9,r9
        beq     .Normal

        mr.     r4,r4
        beq     .FixedValue                  #Word
        rlwinm  r10,r10,16,16,31
        cmpwi   r4,4
        beq     .FixedValue                  #halfword
        extsh   r10,r10
        cmpwi   r4,5                         #halfword algebraic
        beq     .FixedValue
        rlwinm  r10,r10,24,24,31
        b       .FixedValue                  #byte

.Normal:
        rlwinm  r9,r12,16,16,31
        andi.   r9,r9,0xa800
        cmplwi  r9,0x8000                    #lwz/lwzu 0x8000
        beq     .FixedValue
        rlwinm  r10,r10,16,16,31
        cmplwi  r9,0xa000                    #lhz/lhzu 0xa000
        beq     .FixedValue
        extsh   r10,r10
        cmplwi  r9,0xa800                    #lha/lhau 0xa800
        beq     .FixedValue
        rlwinm  r10,r10,24,24,31
        cmplwi  r9,0x8800                    #lbz/lbzu 0x8800
        bne     .NotSupported                #Not Supported

.FixedValue:
    	stwx    r10,r11,r6                   #Store gotten value in correct register

.DoneDSI:
        li      r3,1
        blr

#********************************************************************************************
#
#
#
#********************************************************************************************

ExcITLBMiss:
		mfspr	r2,HASH1                #get first pointer
		li      r1,8                    #load 8 for counter
		mfctr	r0                      #save counter
		mfspr	r3,ICMP                 #get first compare value
		addi	r2,r2,-8                #pre dec the pointer
im0:
		mtctr	r1                      #load counter
im1:
		lwzu	r1,8(r2)                #get next pte
		cmpw	r1,r3                   #see if found pte
		bdnzf	eq,im1                  #dec count br if cmp ne and if count not zero
		bne     instrSecHash            #if not found set up second hash or exit

		lwz     r1,4(r2)                #load tlb entry lower-word
		andi.	r3,r1,PTE_GUARDED<<3    #check G bit
		bne     doISIp                  #if guarded, take an ISI

		mtctr	r0                      #restore counter
		mfspr	r0,IMISS                #get the miss address for the tlbl
		mfsrr1	r3                      #get the saved cr0 bits
		mtcrf	0x80,r3                 #restore CR0
		mtspr	RPA,r1                  #set the pte
		ori     r1,r1,PTE_REFERENCED    #set reference bit
		srwi	r1,r1,8                 #get byte 7 of pte
		tlbli	r0                      #load the itlb
		stb     r1,6(r2)                #update page table
		rfi                             #return to executing program

instrSecHash:
		andi.	r1,r3,PTE_HASHID        #see if we have done second hash
		bne     doISI                   #if so, go to ISI interrupt

		mfspr	r2,HASH2                #get the second pointer
		ori     r3,r3,PTE_HASHID        #change the compare value
		li      r1,8                    #load 8 for counter
		addi	r2,r2,-8                #pre dec for update on load
		b       im0                     #try second hash

doISIp:
		mfsrr1	r3                      #get srr1
		andi.	r2,r3,0xffff            #clean upper srr1
		addis	r2,r2,DSISR_PROTECT@h   #or in srr<4> = 1 to flag prot violation
		b       isi1
doISI:
 		mfsrr1	r3                      #get srr1
 		andi.	r2,r3,0xffff            #clean srr1
		addis	r2,r2,DSISR_NOTFOUND@h  #or in srr1<1> = 1 to flag not found
isi1:
		mtctr	r0                      #restore counter
		mtsrr1	r2                      #set srr1
		mfmsr	r0                      #get msr
		xoris	r0,r0,PSL_TGPR@h        #flip the msr<tgpr> bit
		mtcrf	0x80,r3                 #restore CR0
		mtmsr	r0                      #flip back to the native gprs
        isync
		ba      0x400                   #go to instr. access interrupt

#********************************************************************************************
#
#
#
#********************************************************************************************

ExcDLoadTLBMiss:
		mfspr	r2,HASH1                #get first pointer
		li      r1,8                    #load 8 for counter
		mfctr	r0                      #save counter
		mfspr	r3,DCMP                 #get first compare value
		addi	r2,r2,-8                #pre dec the pointer
dm0:
		mtctr	r1                      #load counter
dm1:
		lwzu	r1,8(r2)                #get next pte
		cmpw	r1,r3                   #see if found pte
		bdnzf	eq,dm1                  #dec count br if cmp ne and if count not zero
		bne     dataSecHash             #if not found set up second hash or exit

		lwz     r1,4(r2)                #load tlb entry lower-word
		mtctr	r0                      #restore counter
		mfspr	r0,DMISS                #get the miss address for the tlbld
		mfsrr1	r3                      #get the saved cr0 bits
		mtcrf	0x80,r3                 #restore CR0
		mtspr	RPA,r1                  #set the pte
		ori     r1,r1,PTE_REFERENCED    #set reference bit
		srwi	r1,r1,8                 #get byte 7 of pte
		tlbld	r0                      #load the dtlb
		stb     r1,6(r2)                #update page table
		rfi                             #return to executing program

dataSecHash:
 		andi.	r1,r3,PTE_HASHID        #see if we have done second hash
		bne     doDSI                   #if so, go to DSI interrupt

		mfspr	r2,HASH2                #get the second pointer
		ori     r3,r3,PTE_HASHID        #change the compare value
		li      r1,8                    #load 8 for counter
		addi	r2,r2,-8                #pre dec for update on load
		b       dm0                     #try second hash

#********************************************************************************************
#
#
#
#********************************************************************************************

ExcDStoreTLBMiss:
		mfspr	r2,HASH1                #get first pointer
		li      r1,8                    #load 8 for counter
		mfctr	r0                      #save counter
		mfspr	r3,DCMP                 #get first compare value
		addi	r2,r2,-8                #pre dec the pointer
ceq0:
		mtctr	r1                      #load counter
ceq1:
		lwzu	r1,8(r2)                #get next pte
		cmpw	r1,r3                   #see if found pte
		bdnzf	eq,ceq1                 #dec count br if cmp ne and if count not zero
		bne     cEq0SecHash             #if not found set up second hash or exit

		lwz     r1,4(r2)                #load tlb entry lower-word
		andi.	r3,r1,PTE_CHANGED       #check the C-bit
		beq     cEq0ChkProt             #if (C==0) go check protection modes
ceq2:
		mtctr	r0                      #restore counter
		mfspr	r0,DMISS                #get the miss address for the tlbld
		mfsrr1	r3                      #get the saved cr0 bits
		mtcrf	0x80,r3                 #restore CR0
		mtspr	RPA,r1                  #set the pte
		tlbld	r0                      #load the dtlb
		rfi                             #return to executing program

cEq0SecHash:
		andi.	r1,r3,PTE_HASHID        #see if we have done second hash
		bne     doDSI                   #if so, go to DSI interrupt

		mfspr	r2,HASH2                #get the second pointer
		ori     r3,r3,PTE_HASHID        #change the compare value
		li      r1,8                    #load 8 for counter
		addi	r2,r2,-8                #pre dec for update on load
		b       ceq0                    #try second hash

cEq0ChkProt:
		rlwinm.	r3,r1,30,0,1            #test PP
		bge-    chk0                    #if (PP == 00 or PP == 01) goto chk0:

		andi.	r3,r1,1                 #test PP[0]
		beq+	chk2                    #return if PP[0] == 0
		b       doDSIp                  #else DSIp

chk0:
  		mfsrr1	r3                      #get old msr
		andis.	r3,r3,SRR1_KEY@h        #test the KEY bit (SRR1-bit 12)
		beq     chk2                    #if (KEY==0) goto chk2:
		b       doDSIp                  #else DSIp
chk2:
   		ori     r1,r1,(PTE_REFERENCED|PTE_CHANGED)    #set reference and change bit
		sth     r1,6(r2)                #update page table
		b       ceq2                    #and back we go

doDSI:
 		mfsrr1	r3                      #get srr1
		rlwinm	r1,r3,9,6,6             #get srr1<flag> to bit 6
		addis	r1,r1,DSISR_NOTFOUND@h  #or in dsisr<1> = 1 to flag not found
		b       dsi1
doDSIp:
		mfsrr1	r3                      #get srr1
		rlwinm	r1,r3,9,6,6             #get srr1<flag> to bit 6
		addis	r1,r1,DSISR_PROTECT@h   #or in dsisr<4> = 1 to flag prot violation
dsi1:
  		mtctr	r0                      #restore counter
		andi.	r2,r3,0xffff            #clear upper bits of srr1
		mtsrr1	r2                      #set srr1
		mtdsisr	r1                      #load the dsisr
		mfspr	r1,DMISS                #get miss address
		rlwinm.	r2,r2,0,31,31           #test LE bit
		beq     dsi2                    #if little endian then:

		xori	r1,r1,0x07              #de-mung the data address
dsi2:
  		mtdar	r1                      #put in dar
		mfmsr	r0                      #get msr
		xoris	r0,r0,PSL_TGPR@h        #flip the msr<tgpr> bit
		mtcrf	0x80,r3                 #restore CR0
		mtmsr	r0                      #flip back to the native gprs
		isync
		sync
		sync
		ba      0x300                   #branch to DSI interrupt

#********************************************************************************************
#
#
#
#********************************************************************************************

_FlushICache:
        b       .Mojo1					#Some L1 mojo

.Mojo2: mfspr   r0,HID0
        ori     r0,r0,HID0_ICFI
        mtspr   HID0,r0
        xori    r0,r0,HID0_ICFI
        mtspr   HID0,r0
        sync
        b       .Mojo3

.Mojo1: b       .Mojo2

.Mojo3: blr

#********************************************************************************************
