## Copyright (c) 2019, 2020 Dennis van der Boon
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


.macro loadreg register, value
	.if 	(\value >= -0x8000) && (\value <= 0x7fff)
        	li      \register, \value
	.else
		lis     \register, \value@h
		ori     \register, \register, \value@l
	.endif
.endm

.macro setpcireg pcivalue
		ori r23,r22,\pcivalue
.endm

.macro configwrite8
       lis      r20,CONFIG_ADDR@h
       stwbrx   r23,0,r20
       sync
       andi.    r23,r23,3
       oris     r21,r23,CONFIG_DAT@h
       stb      r25,0(r21)
       sync
.endm

.macro configwrite16
       lis      r20,CONFIG_ADDR@h
       lis      r21,CONFIG_DAT@h
       stwbrx   r23,0,r20
       sync
       sthbrx   r25,0,r21
       sync
.endm

.macro configwrite32
       lis      r20,CONFIG_ADDR@h
       lis      r21,CONFIG_DAT@h
       stwbrx   r23,0,r20
       sync
       stwbrx   r25,0,r21
       sync
.endm

.set IF_CONTEXT,                0                 #Align this with the if_Frame struct in libstructs.h
.set IF_CONTEXT_EXCID,          IF_CONTEXT
.set IF_CONTEXT_SRR0,           IF_CONTEXT+4
.set IF_CONTEXT_SRR1,           IF_CONTEXT+8
.set IF_CONTEXT_DAR,            IF_CONTEXT+12
.set IF_CONTEXT_DSISR,          IF_CONTEXT+16
.set IF_CONTEXT_CR,             IF_CONTEXT+20
.set IF_CONTEXT_CTR,            IF_CONTEXT+24
.set IF_CONTEXT_LR,             IF_CONTEXT+28
.set IF_CONTEXT_XER,            IF_CONTEXT+32
.set IF_CONTEXT_FPSCR,          IF_CONTEXT+36
.set IF_CONTEXT_GPR,            IF_CONTEXT+40
.set IF_CONTEXT_FPR,            IF_CONTEXT+40+128
.set IF_ALIGNSTORE,             IF_CONTEXT+40+128+256
.set IF_VSCR,                   IF_ALIGNSTORE+8
.set IF_VPR,                    IF_VSCR+16
.set IF_VRSAVE,                 IF_VPR+(32*16)
.set IF_BATS,                   IF_VRSAVE+4
.set IF_SEGMENTS,               IF_BATS+64
.set IF_EXCNUM,                 IF_SEGMENTS+64
.set IF_SDR1,                   IF_EXCNUM+4       #the task's page table as SDR1; 0 = the library's
.set IF_SIZE,                   IF_SDR1+4

.set MS_MSAR1,                  0
.set MS_MSAR2,                  4
.set MS_MEAR1,                  8
.set MS_MEAR2,                  12
.set MS_MESAR1,                 16
.set MS_MESAR2,                 20
.set MS_MEEAR1,                 24
.set MS_MEEAR2,                 28
.set MS_MBEN,                   32
.set MS_MCCR1,                  36
.set MS_MCCR1TESTHIGH,          40
.set MS_MCCR1TESTLOW1,          44
.set MS_MCCR1TESTLOW2,          48
.set MS_MCCR1TESTLOW3,          52
.set MS_MEMSIZE,                56

.set VRSAVE,                    256

.set PP_FLAGS,                  8
.set PP_STACK,                  12
.set PP_STACKSIZE,              16
.set PP_REGS,                   20
.set PP_FREGS,                  80

.set PPB_LINEAR,                1
.set PPB_THROW,                 2

.set WAITTIME_UPPER,            14                #Align with struct WaitTime in libstructs.h
.set WAITTIME_LOWER,            18

.set GPR0,                      0*4
.set GPR1,                      1*4
.set GPR2,                      2*4
.set GPR3,                      3*4
.set GPR4,                      4*4

# struct Task's HOOK FIELDS, for FIXES.md item 10. Derived, not guessed:
# struct Node is 14 bytes (ln_Succ 0, ln_Pred 4, ln_Type 8, ln_Pri 9,
# ln_Name 10), so tc_Flags is 0x0E, and counting exec/tasks.h forward from
# there -- tc_State 0x0F, tc_IDNestCnt 0x10, tc_TDNestCnt 0x11, tc_SigAlloc
# 0x12, tc_SigWait 0x16, tc_SigRecvd 0x1A, tc_SigExcept 0x1E, tc_TrapAlloc
# 0x22, tc_TrapAble 0x24, tc_ExceptData 0x26, tc_ExceptCode 0x2A, tc_TrapData
# 0x2E, tc_TrapCode 0x32, tc_SPReg 0x36, tc_SPLower 0x3A, tc_SPUpper 0x3E --
# puts tc_Switch at 0x42 and tc_Launch at 0x46.
#
# iFUSION'S OWN OFFSETS AGREE, INDEPENDENTLY: it writes 0x46(r3) at 0x01BF64 to
# install its tc_Launch and reads 0x0E(r3) at 0x01F2F8 to clear TF_LAUNCH, with
# r3 being FindTaskPPC(0)'s answer. And PPCTASK_CONTEXMEM = 0x64 below confirms
# the prefix, since sizeof(struct Task) is 0x5C.
# THE STRUCTURE A SMALLCONTEXT HANDLER RECEIVES, from WarpOS's own public
# include warpos/include/powerpc/powerpc.i:233 --
#
#         STRUCTURE       XCONTEXT,0
#         ULONG           XCO_EXCID       ;exception ID (see above)
#         ULONG           XCO_R3          ;r3
#         LABEL           XCO_SIZE        ;don't depend on this size
#
# kernel.s builds this by hand with raw stack offsets and has the two fields
# the other way round: GPR[3] at +0 and the status at +4. iFUSION's exc_sc
# reads 4(r3) for its 'HOST' magic -- 0x01E614 -- and so reads the status
# slot, a word kernel.s does not write until after the handler returns. See
# FIXES.md item 11; these two names exist so the fix can be read.
.set XCO_EXCID,                 0
.set XCO_R3,                    4

# _SmallExcHandler's own 512-byte frame. The XCONTEXT it hands the handler is
# built in it, and the handler's return status needs a slot that is NOT part of
# the XCONTEXT -- it used to be written over XCO_R3.
.set SEH_XCONTEXT,              300
.set SEH_STATUS,                336

.set PPCTASK_FLAGS,             0x0E
.set PPCTASK_SWITCH,            0x42
.set PPCTASK_LAUNCH,            0x46

# exec/tasks.h's tc_Flags bits -- TB_SWITCH 6 and TB_LAUNCH 7, so 0x40 and
# 0x80. FIXES.md item 10 is about these two.
#
# THE ORIGINAL CALLS THEM TP_SWITCH AND TP_LAUNCH, and that is not a different
# pair of bits. powerpc.p:4740 and :5133 are "bittst r6,TP_SWITCH" and
# "bittst r6,TP_LAUNCH" against TC_FLAGS, but NEITHER NAME IS DEFINED ANYWHERE
# IN THE SOURCES WE HAVE, so the bit numbers cannot be read off the original.
# Two pieces of evidence settle it:
#
#   - TP_ is a POWERPC BIT POSITION, counted from the MSB. WarpOS's own
#     BITDEFPPC macro, warpos/include/powerpc/ppcmacros.i:8, emits all three
#     spellings from one line:
#         \1P_\2 = \3              ; PowerPC position, MSB = 0
#         \1B_\2 = 31-(\3)         ; Amiga bit number
#         \1F_\2 = 1<<(31-(\3))    ; mask
#     and bittst at ppcmacros.i:373 is "extrwi. trash,\1,1,\2", which consumes
#     the P_ form. tc_Flags is loaded with lbz, so the byte's bit 6 lands at
#     PowerPC position 25 -- the same physical bit as exec's TF_SWITCH.
#
#     Neither TP_ name is in this repo because the exec includes are not:
#     powerpc.p:74 includes exec/tasks.i and there is no warpos/exec/ at all.
#     WarpOS built against PPC-flavoured copies using BITDEFPPC, which is why
#     ppcmacros.i defines that macro while nothing here uses it. The NDK's own
#     BITDEF yields TB_/TF_ only.
#   - iFUSION, built against real WarpOS, clears the bit at 0x01F2F8:
#         lbz  r4,0xe(r3)     ; tc_Flags
#         li   r5,0x80
#         andc r4,r4,r5
#         stb  r4,0xe(r3)
#     0x80 is exec's TF_LAUNCH. So TP_LAUNCH is position 7, and TP_SWITCH is
#     position 6 by the same token -- exec reserves exactly those two bits for
#     exactly this purpose.
.set TF_SWITCH,                 0x40
.set TF_LAUNCH,                 0x80

# ABSOLUTE PAGE-ZERO SLOTS. The base field of a PPC load/store encodes rA=0
# as the LITERAL zero, not as GPR0, so "lwz r0,ZP_EXCSRR0(r0)" is an absolute
# access and works whatever r0 holds and whether the MMU is on or off. That is
# the same idiom as the 0x24(r0) decrementer counter below.
#
# ZP_EXCSRR0/ZP_EXCSRR1 replace SPRG0/SPRG1 as the staging slots for SRR0 and
# SRR1 across exception entry and exit. SPRG0 and SPRG1 cannot be used for it:
# iFUSION's guest exception trampoline at h2:0x0058 does "mfspr r1,SPRG0" and
# "mtspr SPRG1,r1", so it reads SPRG0 expecting a usable pointer and destroys
# SPRG1. Under real WarpOS SPRG0 is a persistent pointer set once at HAL init
# (warp.p:170-172) and exception entry never writes it.
#
# ZP_EXCSCRATCH is the eight words SPRG0 now points at -- iFUSION needs four,
# written at 0(r1)..12(r1). warp.p:170 reserves 32 bytes the same way.
.set ZP_EXCSRR0,                0x28
.set ZP_EXCSRR1,                0x2C
.set ZP_EXCSCRATCH,             0x30

# THE LIBRARY'S OWN MMU STATE, AND THE INTERRUPTED TASK'S, at physical 0x2000:
# the free page between the last vector (0x1700, setupppc.s's table) and the
# kernel at OFFSET_KERNEL 0x3000. Exception entry reads and writes it
# UNTRANSLATED, before translation is enabled, and the exit writes the
# registers from it after translation is switched off -- so, like ZP_*, every
# access is rA=0 absolute and works with the MMU on or off (page zero and this
# page are PA == EA both ways: BAT0 maps the low 2 MB one to one).
#
# The library's set is filled once by setupppc.c from pp_SystemBATs,
# pp_SystemSegs and SDR1, and read at every exception entry. The task's set is
# written at entry, copied into the frame by _StoreFrame, put back by
# _LoadFrame and installed by the exit. FIXES.md "THE MSR UNDER THE EXCEPTION
# PATH", 23 Oct 2026. Keep in step with MMU_ZERO_* in constants.h.
.set ZP_MMU_LIB_BATS,           0x2000            #16 words: ibatu/ibatl/dbatu/dbatl 0..3, frame order
.set ZP_MMU_LIB_SEGS,           0x2040            #16 words: SR0..SR15
.set ZP_MMU_LIB_SDR1,           0x2080
.set ZP_MMU_TASK_BATS,          0x2100            #the interrupted task's, same layout
.set ZP_MMU_TASK_SEGS,          0x2140
.set ZP_MMU_TASK_SDR1,          0x2180
.set ZP_MMU_EXIT_TABLE,         0x2190            #tp_Table of the task being resumed, 0 for none
.set ZP_MMU_EXIT_CR,            0x21A0            #CR, r0, r1, r3 staged for the untranslated exit
.set ZP_MMU_EXIT_R0,            0x21A4
.set ZP_MMU_EXIT_R1,            0x21A8
.set ZP_MMU_EXIT_R3,            0x21AC
.set ZP_MMU_SAVE_CR,            0x21B0            #parked across the entry's SDR1 compare
.set ZP_MMU_SAVE_CTR,           0x21B4            #and across both tlbie loops
.set ZP_MMU_SAVE_R3,            0x21B8
.set ZP_EXCSTACK,               0x21C0            #r1 for the outermost exception: the kernel stack, see constants.h
.set ZP_EXCDEPTH,               0x21C4            #nesting depth: entry +1, exit -1; 0 = take ZP_EXCSTACK
.set ZP_ENTRY_CR,               0x21C8            #the interrupted CR and r4, parked while the entry picks its stack
.set ZP_ENTRY_R4,               0x21CC
.set VSID_TAG_HI,               0x0080            #VSID bit 23 >> 16: a task whose SR0 has it needs no TLB flush
.set ZP_TIME_T0,                0x2200            #the timing build only: constants.h MMU_ZERO_TIME_*
.set ZP_TIME_T1,                0x2204
.set ZP_TIME_T2,                0x2208
.set ZP_TIME_T5,                0x2214
.set ZP_TIME_T6,                0x2218
.set ZP_TIME_T7,                0x221C
.set ZP_TIME_T8,                0x2220

.set POWERPCBASE,               0x10              #Align this number with the zp struct in libstructs.h
.set POWERPCBASE_THISPPCTASK,   0x94              #debugdebug need to find to link this to libstruct.h
.set PPCTASK_CONTEXMEM,         0x64
# tp_Table, tasksPPC.h:38, under pack(2): Task 0x5C, then StackSize, StackMem,
# ContextMem (0x64), TaskPtr, Flags, TaskLink (18 bytes: MinNode, APTR, ULONG,
# UWORD, no tail padding), BATStorage 0x82, Core 0x86, TableLink 0x8A, Table
# 0x92. Unaligned on purpose, and iFUSION stores to it as "stw r0,0x92(r5)".
.set PPCTASK_TABLE,             0x92
.set PPCMEMHEADER,              8

.set HID0,                      1008
.set HID0_NHR,	                0x00010000
.set HID0_ICFI,                 0x00000800
.set HID0_DCFI,                 0x00000400

.set PSL_VEC,	                0x02000000	      #/* ..6. AltiVec vector unit available */
.set PSL_FP,	                0x00002000        #/* B.6. floating point enable */
.set PSL_IP,	                0x00000040        #/* ..6. interrupt prefix */
.set PSL_TGPR,               	0x00020000        #/* ..6. temp. gpr remapping (mpc603e) */
.set PSL_IR,	                0x00000020        #/* .468 instruction address relocation */
.set PSL_DR,                    0x00000010        #/* .468 data address relocation */

.set DMISS,                     976
.set DCMP,                      977
.set HASH1,                     978
.set HASH2,                     979
.set IMISS,                     980
.set ICMP,                      981
.set RPA,                       982

.set HID0_ICFI,                 0x00000800

.set PTE_GUARDED,               0x00000001
.set PTE_REFERENCED,            0x00000100
.set PTE_CHANGED,               0x00000080
.set PTE_HASHID,                0x00000040

.set DSISR_NOTFOUND,            0x40000000
.set DSISR_PROTECT,             0x08000000

.set SRR1_KEY,	                0x00080000

.set L2_ADR_INCR,               0x100
.set L2_SIZE_2M,                0x2000
.set L2_SIZE_1M,                0x1000
.set L2_SIZE_HM,                0x800
.set L2_SIZE_QM,                0x400
.set L2_SIZE_1M_U,              0x0010
.set L2_SIZE_2M_U,              0x0020

.set L2CR_L2SIZ_2M,             0x00000000      # bits 2-3 2 MB; MPC7400 ONLY!
.set L2CR_L2SIZ_1M,             0x30000000      # bits 2-3 1MB
.set L2CR_L2SIZ_HM,             0x20000000      # bits 2-3 512K
.set L2CR_L2SIZ_QM,             0x10000000      # bits 2-3 256K; MPC750 ONLY

.set ibat4u,560
.set ibat4l,561
.set ibat5u,562
.set ibat5l,563
.set ibat6u,564
.set ibat6l,565
.set ibat7u,566
.set ibat7l,567
.set dbat4u,568
.set dbat4l,569
.set dbat5u,570
.set dbat5l,571
.set dbat6u,572
.set dbat6l,573
.set dbat7u,574
.set dbat7l,575

.set CMD_BASE,                  0x80000000
.set CONFIG_ADDR,               0xFEC00000
.set CONFIG_DAT,                0xFEE00000
.set MPC107_MBEN,               0xA0
.set MPC107_MCCR1,              0xF0

