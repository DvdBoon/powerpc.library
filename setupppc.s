## Copyright (c) 2019-2020, 2024 Dennis van der Boon
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

#*********************************************************

.global     _GetNiceTable
.global     _setupPPC, _Reset, _GetExcTable, _GetVecEntry
.global     _getTableFX, _getTable100, _getTable66, _getTableVGer
.global     _getL2Size, _detectMem

#*********************************************************

.section "setupppc","acrx"

        bl  .SkipCom	        #0x3000 System initialization

        #below is a mirror of struct InitData in libstruct.h

        .long   0               #Used for initial communication
        .long   0               #MemStart
        .long   0               #MemLen
        .long   0               #RTGBase
        .long   0               #RTGLen
        .long   0               #RTGType
        .long   0               #RTGConfig
        .long   0               #Options1
        .long   0               #Options2
        .long   0               #Options3
        .long   0               #DeviceID
        .long   0               #ConfigBase
        .long   0               #MsgBase
        .long   0               #MPIC
        .long   0               #StartBAT
        .long   0               #SizeBAT

#*********************************************************

.SkipCom:
        li      r3,0
        loadreg r1,(HID0_DCFI | HID0_ICFI)
        mtspr   HID0,r1
        mtspr   HID0,r3
        sync
        sync

        mflr	r3
        subi    r3,r3,4         #Points to initialization data
        subi    r1,r3,256       #0x3000 bytes of initial stack

        bl   _setupPPC

        lwz     r1,8(r0)
        subi    r1,r1,0x20
        rfi

#*********************************************************

_Reset:
        mflr    r6
        mfmsr	r5
        andi.	r5,r5,PSL_IP
        mtmsr	r5              #Clear MSR, keep Interrupt Prefix for now
        isync

        li      r3,0            #Zero-out registers
        mtsprg0	r3
        mtsprg1 r3
        mtsprg2	r3
        mtsprg3	r3
        mtspr	HID0,r3         #Set HID0 to a known state
        sync

        loadreg	r3,PSL_FP       #Set MPU/MSR to a known state. Turn on FP
        or      r3,r5,r3
        mtmsr 	r3
        isync

        mtfsfi  7,0             #Init the floating point control/status register
        mtfsfi  6,0
        mtfsfi  5,0
        mtfsfi  4,0
        mtfsfi  3,0
        mtfsfi  2,0
        mtfsfi  1,0
        mtfsfi  0,0
        isync

        bl	ifpdr_value         #Initialize floating point data regs to known state

        .long   0x3f800000      #Value of 1.0

ifpdr_value:
        mflr	r3
        lfs     f0,0(r3)
        lfs     f1,0(r3)
        lfs     f2,0(r3)
        lfs     f3,0(r3)
        lfs     f4,0(r3)
        lfs     f5,0(r3)
        lfs     f6,0(r3)
        lfs     f7,0(r3)
        lfs     f8,0(r3)
        lfs     f9,0(r3)
        lfs     f10,0(r3)
        lfs     f11,0(r3)
        lfs     f12,0(r3)
        lfs     f13,0(r3)
        lfs     f14,0(r3)
        lfs     f15,0(r3)
        lfs     f16,0(r3)
        lfs     f17,0(r3)
        lfs     f18,0(r3)
        lfs     f19,0(r3)
        lfs     f20,0(r3)
        lfs     f21,0(r3)
        lfs     f22,0(r3)
        lfs     f23,0(r3)
        lfs     f24,0(r3)
        lfs     f25,0(r3)
        lfs     f26,0(r3)
        lfs     f27,0(r3)
        lfs     f28,0(r3)
        lfs     f29,0(r3)
        lfs     f30,0(r3)
        lfs     f31,0(r3)
        sync

        li      r3,0            #Clear BAT and Segment mapping registers
        mtibatu 0,r3
        mtibatu 1,r3
        mtibatu 2,r3
        mtibatu 3,r3
        mtibatl 0,r3
        mtibatl 1,r3
        mtibatl 2,r3
        mtibatl 3,r3
        mtdbatu 0,r3
        mtdbatu 1,r3
        mtdbatu 2,r3
        mtdbatu 3,r3
        mtdbatl 0,r3
        mtdbatl 1,r3
        mtdbatl 2,r3
        mtdbatl 3,r3

        mfpvr	r4
        rlwinm	r5,r4,16,16,31
        cmplwi	r5,0x8083
        beq     .ExtraBats
        cmplwi  r5,0x8085
        beq     .ExtraBats

        rlwinm	r5,r4,8,24,31
        cmpwi	r5,0x70
        bne     .SkipExtraBats

.ExtraBats:	
        mtspr	ibat4u,r3
        mtspr	ibat5u,r3
        mtspr	ibat6u,r3
        mtspr	ibat7u,r3
        mtspr	ibat4l,r3
        mtspr	ibat5l,r3
        mtspr	ibat6l,r3
        mtspr	ibat7l,r3
        mtspr	dbat4u,r3
        mtspr	dbat5u,r3
        mtspr	dbat6u,r3
        mtspr	dbat7u,r3
        mtspr	dbat4l,r3
        mtspr	dbat5l,r3
        mtspr	dbat6l,r3
        mtspr	dbat7l,r3

.SkipExtraBats:	
        isync
        sync
        sync

        mtsr	0,r3
        mtsr	1,r3
        mtsr	2,r3
        mtsr	3,r3
        mtsr	4,r3
        mtsr	5,r3
        mtsr	6,r3
        mtsr	7,r3
        mtsr	8,r3
        mtsr	9,r3
        mtsr	10,r3
        mtsr	11,r3
        mtsr	12,r3
        mtsr	13,r3
        mtsr	14,r3
        mtsr	15,r3
		
        isync
        sync
        sync

        mttbu   r3
        mttbl   r3

        mtlr	r6
        blr

#*********************************************************

#
# VOID getL2Size(ULONG scratch, APTR cz, ULONG l2Setting, ULONG allow2M)
#       r3 = scratch (a PHYSICAL address: S9), r4 = cz, r5 = l2Setting, r6 = allow2M
#
# S8 (REVIEW-LIB.md). THE L2'S SIZE, FOUND BY FUNCTION, LARGEST FIRST. The probe that was here wrote 1 MB into
# an L2 set to 1 MB and COUNTED the 256-byte steps that survived (0x1000, 0x800 or 0x400): the count cannot exceed
# 0x1000, so a 2 MB L2 was taken for a 1 MB one (the 2 MB test was commented out, as in 17.13b,
# setupppc.p:1172-1174), and the exact count could come out one or two short, because the probe's own code is
# fetched through the L2 after the test lines are in. Now, for each candidate size C from 2 MB (allow2M: the 7400
# and the 7410) down to 256 KB, with L2E clear: L2CR = l2Setting | the size bits of C | L2CR_DO (data only: an
# instruction miss does not reload the L2, so no line of this code is in an L2 whose size is wrong), then the
# global invalidate, then L2E; every 32-byte line of a C-byte block gets a word of its own, and every word is
# read back. A candidate larger than the SRAM indexes the data RAM beyond its range, so two lines share a cell
# and one of them reads back the other's word: the first candidate that reads back clean is the size. That is
# the manual's order (3.7.5): the size is set with L2E clear and the L2 is invalidated before it is enabled.
# THE FLUSH FIRST. The L1 data cache and the L2 are flushed by displacement before anything is invalidated (a
# dcbf of the range, a load of it, a dcbf again: kernel.c FlushDCache's recipe), so no modified line of anyone's
# is in a cache when the global invalidate throws the L2's lines away; the routine touches nothing but registers
# and the scratch block (its one other store is the result, after the L2 is off again). The scratch lines are
# flushed (dcbf) when a candidate is done, so no modified line of the block stays in a cache for memory that is
# somebody else's later.
# NOTHING READS BACK CLEAN (not even 256 KB): 1 MB, as S2 decided; the caller then programs 1 MB.
# Returns with the L2 DISABLED. cz_SizeBit is the L2SIZ field (0 for 2 MB: test cz_SizeBytes, not the bits),
# cz_SizeBytes the size in 256-byte lines. The caller writes the final L2CR, invalidates and enables.
# S9 (REVIEW-LIB.md): THE PASSES RUN IN REAL ADDRESSING MODE. A candidate larger than the SRAM makes two L2 lines
# share a cell, and the L2 holds other lines too: the page-table lines that the hardware table walk reads for the
# scratch pages (about 1500 walks per 2 MB pass), cast out of the L1 into the L2, which is a victim cache. A scratch
# line written into a cell that a page-table line shares leaves the next walk that hits it with garbage: a DSI in
# the set-up code, which has no handler yet. So MSR[DR] is cleared (and MSR[EE]) for the whole routine, the MSR
# kept in CTR: no table walks, and r3 is a PHYSICAL address (the caller passes the scratch's EA minus MemBase:
# system memory is mapped at MemBase + PA, setupppc.c mmuSetup). Real mode generates WIMG 0b0011 (write-back,
# cacheable, coherent, guarded: MPC7410UM PDF p. 188, p. 305), and the manual's own L2 SRAM test starts with
# MSR[DR] = 0 for that (3.7.9.2 step 1, PDF p. 245). The MSR is put back before the result is stored (r4 is an EA).
# 17.13b's probe (setupppc.p:1146-1170) has no mtmsr of its own; WarpOS: NO COUNTERPART (a search of warpos/ for
# l2cr, l2wt, mfl2, mtl2 and L2E, 12 Nov, returns 0 lines).
# P2 (REVIEW-LIB.md S8): L2E IS CLEARED ON ITS OWN, with a sync each side, before any L2I is set: the manual's order
# (3.7.3.7 steps 3 and 4, PDF p. 235; L2I must never be set while L2E is), for an L2 that is on at entry. 17.13b
# writes the configuration with E clear and syncs before it sets L2I in a second write (setupppc.p:1121-1127).

_getL2Size:
        mfmsr   r0
        mtctr   r0                      #the MSR as found, in CTR (r0 and r3-r12 are all in use below)
        rlwinm  r0,r0,0,17,15           #MSR[EE] clear
        rlwinm  r0,r0,0,28,26           #MSR[DR] clear: real addressing from here to the end, r3 is physical
        sync
        mtmsr   r0
        isync

        lis     r8,L2_SCRATCH_END@h     #the range: 2 MB plus two L1 data caches
        li      r7,0
.L2FlushA:
        dcbf    r7,r3
        addi    r7,r7,32
        cmplw   r7,r8
        blt     .L2FlushA
        sync
        li      r7,0
.L2FlushB:
        lwzx    r0,r7,r3
        addi    r7,r7,32
        cmplw   r7,r8
        blt     .L2FlushB
        li      r7,0
.L2FlushC:
        dcbf    r7,r3
        addi    r7,r7,32
        cmplw   r7,r8
        blt     .L2FlushC
        sync

        mfl2cr  r0                      #P2: L2E clear on its own, a sync each side, before any L2I (an L2 that is on)
        rlwinm  r0,r0,0,1,31
        mtl2cr  r0
        sync
        isync

        li      r12,0                   #the candidate: 0 2 MB, 1 1 MB, 2 512 KB, 3 256 KB
        cmpwi   r6,0
        bne     .L2Cand
        li      r12,1                   #no 2 MB on a part that does not have the setting

.L2Cand:
        cmpwi   r12,0
        bne     .L2Cand1
        li      r10,0                   #2 MB: L2SIZ 0b00
        lis     r11,0x20
        b       .L2CandSet

.L2Cand1:
        cmpwi   r12,1
        bne     .L2Cand2
        lis     r10,L2CR_L2SIZ_1M@h
        lis     r11,0x10
        b       .L2CandSet

.L2Cand2:
        cmpwi   r12,2
        bne     .L2Cand3
        lis     r10,L2CR_L2SIZ_HM@h
        lis     r11,0x8
        b       .L2CandSet

.L2Cand3:
        lis     r10,L2CR_L2SIZ_QM@h
        lis     r11,0x4

.L2CandSet:
        or      r9,r5,r10
        oris    r9,r9,L2CR_DO@h
        oris    r0,r9,L2CR_L2I@h
        mtl2cr  r0                      #L2E clear: the clock, the size, data only, and the invalidate
        sync

.L2WaitI:
        mfl2cr  r0
        andi.   r0,r0,L2CR_L2IP
        bne     .L2WaitI

        oris    r0,r9,L2CR_L2E@h
        mtl2cr  r0
        sync
        isync

        loadreg r9,0x5A5A5A5A           #the word of a line: its offset xor this xor the size, so another
        xor     r9,r9,r11               #candidate's words from an earlier pass never match
        li      r7,0
.L2Wr:
        add     r8,r3,r7
        dcbz    0,r8
        xor     r0,r7,r9
        stw     r0,0(r8)
        addi    r7,r7,32
        cmplw   r7,r11
        blt     .L2Wr
        sync

        li      r7,0
.L2Rd:
        add     r8,r3,r7
        lwz     r6,0(r8)
        xor     r0,r7,r9
        cmplw   r6,r0
        bne     .L2Bad
        addi    r7,r7,32
        cmplw   r7,r11
        blt     .L2Rd
        li      r6,0                    #clean
        b       .L2Done

.L2Bad:
        li      r6,1

.L2Done:
        li      r7,0
.L2FlushD:
        add     r8,r3,r7
        dcbf    0,r8
        addi    r7,r7,32
        cmplw   r7,r11
        blt     .L2FlushD
        sync

        or      r0,r5,r10               #the L2 off again: the candidate's bits, E clear
        oris    r0,r0,L2CR_DO@h
        mtl2cr  r0
        sync
        isync

        cmpwi   r6,0
        beq     .L2Found
        addi    r12,r12,1
        cmpwi   r12,4
        blt     .L2Cand

        lis     r10,L2CR_L2SIZ_1M@h     #nothing read back clean: 1 MB (S2)
        lis     r11,0x10

.L2Found:
        rlwinm  r11,r11,24,8,31         #the size in bytes to 256-byte lines
        mfctr   r0                      #S9: the MSR as found, translation back on, before r4 (an EA) is used
        mtmsr   r0
        isync
        stw     r10,0(r4)
        stw     r11,4(r4)
        blr

#*********************************************************

_getTable100:
        mflr    r4
        bl      .EndTable

        .long	0b1110,350000000,0b1010,400000000
        .long	0b0111,450000000,0b1011,500000000
        .long	0b1001,550000000,0b1101,600000000
        .long	0b0101,650000000,0b0010,700000000
        .long	0b0001,750000000,0b1100,800000000
        .long	0b0000,900000000,0

#*********************************************************

_getTableFX:
        mflr    r4
        bl      .EndTable

        .long	0b01110,700000000,0b01111,750000000
        .long	0b10000,800000000,0b10001,850000000
        .long	0b10010,900000000,0b10011,950000000
        .long	0b10100,1000000000,0

#*********************************************************

_getTable66:
        mflr    r4
        bl      .EndTable

        .long	0b1101,400000000,0b0001,500000000
        .long	0b0101,433333333,0b0010,466666666
        .long	0b1100,533333333,0

#*********************************************************

# S6: the MPC7450's table, 17.13b's .PLL_CFG_VGER (setupppc.p:1221-1224): PLL_CFG, HID1 bits 15-19, to the CPU's speed.
_getTableVGer:
        mflr    r4
        bl      .EndTable

        .long	0b11010,600000000,0b01010,650000000
        .long	0b00100,700000000,0b00010,750000000
        .long	0b11000,800000000,0b01100,850000000
        .long	0b01111,900000000,0

#*********************************************************

_GetVecEntry:
        mflr    r4
        bl      .EndTable

_VecEntry:
        mtsprg3 r0
        mflr    r0
.fake:  bl      .fake
.EndEntry:

#*********************************************************

#********************************************************************************************
#
# ULONG* GetNiceTable(void) -- the NICE weighting table, FIXES.md item 18.
#
# This is the original's NICETab, powerpc.p:17948, transcribed entry for entry.
# Its own comment there gives the rule, "2^(NICE/(40/6)) << 24", and
# ReSchedule indexes it at (nice + NICE_RANGE/2) * 4 -- so 41 entries for nice
# -20..+20, and entry[i] = round(2^(i*6/40) * 2^24). Three were checked against
# that formula before transcribing: entry[0] = 2^24 = 16777216, entry[20],
# which is nice 0, = 2^27 = 134217728, and entry[40] = 2^30 = 1073741824.
#
# IN ASSEMBLER, because a static const array in setupppc.c will not link --
# "Reference to undefined symbol _SDA_BASE_". Under -sc, C data is addressed
# through the small-data area, and this library is linked -nostdlib with no
# startup to establish one. Every table here is in assembler for that reason.
#
# AND IN THIS FILE RATHER THAN supportppc.s, which is what the init crash was.
# supportppc.s assembles into the "functions" section and setupppc.c into
# "setupppc"; those are separately loaded hunks at unrelated addresses, so a
# direct bl between them lands wherever. Every real cross-section call in this
# library goes through the LVO jump table instead -- kernel.c's RemHeadPPC
# emits no _RemHeadPPC symbol at all, because proto/powerpc.h turns it into
# "lwz r0,-LVO(r3) / mtlr r0 / blrl". A direct bl is only safe WITHIN a
# section, which is why _GetExcTable and _GetVecEntry live here beside their
# caller.
#
# S3: THIS COPY IS NO LONGER THE ONE USED. pp_NICETable held the address this hunk
# RUNS at, and on a Harrier that is the high alias 0xFFF13000 (MSR[IP] is set at
# reset, ENRV maps RAM at 0xFFF00000, the reset branch is relative): 0xFFF13404,
# which stayed that after IP was cleared and which user code reads only through
# the vector window's PTE. supportppc.s now carries the same 41 values as the
# label _NiceTable, in the functions hunk, and libinit.c stores its address in
# pp_NICETable, which setupppc.c no longer overwrites. Nothing calls GetNiceTable
# now; it stays, with this copy, until the reverse engineering is done.
#
#********************************************************************************************

_GetNiceTable:
        mflr    r4
        bl      .skipNice

        .long   16777216,18615487,20655176,22918352,25429504
        .long   28215802,31307392,34737726,38543921,42767158
        .long   47453133,52652548,58421659,64822890,71925500
        .long   79806339,88550677,98253128,109018671,120963790
        .long   134217728,148923894,165241406,183346820,203436034
        .long   225726413,250459137,277901812,308351367,342137263
        .long   379625063,421220380,467373275,518583118,575403996
        .long   638450708,708405415,786025023,872149370,967710316
        .long   1073741824

.skipNice:
        mflr    r3
        mtlr    r4
        blr

_GetExcTable:
        mflr    r4
        bl      .EndTable

        .word 0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0900, 0x0c00
        .word 0x0d00, 0x0e00, 0x0f00, 0x0f20, 0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1600
        .word 0x1700, -1

.EndTable:
        mflr    r3
        mtlr    r4
        blr

#*********************************************************

_detectMem:
        stwu    r1,-256(r1)
        stw     r13,100(r1)
        stw     r14,104(r1)
        stw     r15,108(r1)
        stw     r16,112(r1)
        stw     r17,116(r1)
        stw     r18,120(r1)
        stw     r19,124(r1)
        stw     r20,128(r1)
        stw     r21,132(r1)
        stw     r22,136(r1)
        stw     r23,140(r1)
        stw     r24,144(r1)
        stw     r25,148(r1)
        stw     r26,152(r1)
        stw     r27,156(r1)
        stw     r28,160(r1)
        stw     r29,164(r1)
        stw     r30,168(r1)
        stw     r31,172(r1)

        mr      r15,r3
        lwz     r26,MS_MCCR1TESTHIGH(r3)
        lwz     r28,MS_MCCR1TESTLOW1(r3)
        lwz     r30,MS_MCCR1TESTLOW2(r3)
        lwz     r31,MS_MCCR1TESTLOW3(r3)

		li      r3,0
        loadreg r4,'Boon'
		li      r5,1
		li      r8,0
		li      r9,0
		li      r10,0
		li      r11,0
		li      r12,0
		mr      r13,r26
		li      r14,0
		li      r16,0
		li      r17,0
		li      r18,0
		li      r19,0
        lis     r22,CMD_BASE@h

loc_3BD8:
        setpcireg MPC107_MBEN		   #Memory Bank Enable register
		mr      r25,r5
        configwrite8                   #enable Bank as given in r5

		stw     r4,0(r3)               #try to store "Boon" at address 0x0
		eieio

		stw     r3,4(r3)               #try to store 0x0 at 0x4
		eieio
		lwz     r7,0(r3)               #read from 0x0
		cmplw   r4,r7                  #is it "Boon", long compare
		bne     loc_4184

		or      r14,r14,r5             #continue if found
		setpcireg MPC107_MCCR1         #0x800000f0
		or      r25,r26,r28
        configwrite32                  #set all banks to 12 or 13 row bits

		lis     r6,0x40
		stw     r3,0(r6)		#set 0x400000 to 0x0
		lis     r6,0x80
		stw     r3,0(r6)		#set 0x800000 to 0x0
		lis     r6,0x100
		stw     r3,0(r6)		#set 0x1000000 to 0x0
		lis     r6,0x200
		stw     r3,0(r6)		#set 0x2000000 to 0x0
		lis     r6,0x400
		stw     r3,0(r6)		#set 0x4000000 to 0x0
		lis     r6,0x800
		stw     r3,0(r6)		#set 0x8000000 to 0x0
		eieio
		stw     r4,0(r3)		#set 0x0 to "Boon"
		eieio
		lis     r6,0x40
		lwz     r7,0(r6)		#read from 0x400000
		cmplw   r4,r7			#is it "Boon"
		beq     loc_3CBC		#if yes goto loc_3CBC
		lis     r6,0x80
		lwz     r7,0(r6)		#read form 0x800000
		cmplw   r4,r7			#is it "Boon"
		beq     loc_3E24		#if yes goto loc_3E24
		lis     r6,0x100
		lwz     r7,0(r6)		#read from 0x1000000
		cmplw   r4,r7			#is it "Boon"
		beq     loc_3E24		#if yes goto loc_3E24
		lis     r6,0x200
		lwz     r7,0(r6)		#read from 0x2000000
		cmplw   r4,r7
		beq     loc_3E24		#if its "Boon" goto loc_3E24
		lis     r6,0x400
		lwz     r7,0(r6)		#read from 0x4000000
		cmplw   r4,r7
		beq     loc_3E24		#if its "Boon" goto loc_3E24
		lis     r6,0x800
		lwz     r7,0(r6)		#read from 0x8000000
		cmplw   r4,r7
		beq     loc_3E24		#if its "Boon" goto loc_3E24
		lis     r6,0x1000
		cmpwi   r7,0
		beq     loc_3E24
		b       loc_4184		#goto loc_4184

#********************************************************************************************
loc_3CBC:					#CODE XREF: findSetMem+1D0
		or      r25,r26,r30
        configwrite32
		lis     r6,0x20			#continue tests
		stw     r3,0(r6)
		lis     r6,0x40
		stw     r3,0(r6)
		lis     r6,0x80
		stw     r3,0(r6)
		lis     r6,0x100
		stw     r3,0(r6)
		lis     r6,0x200
		stw     r3,0(r6)
		eieio
		stw     r4,0(r3)
		eieio
		lis     r6,0x20
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3D50
		lis     r6,0x40
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x80
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x100
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x200
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		b       loc_4184

#********************************************************************************************
loc_3D50:					#CODE XREF: findSetMem+274
		or      r25,r26,r31
        configwrite32
		lis     r6,0x10			#continue tests
		stw     r3,0(r6)
		lis     r6,0x20
		stw     r3,0(r6)
		lis     r6,0x40
		stw     r3,0(r6)
		lis     r6,0x80
		stw     r3,0(r6)
		eieio
		stw     r4,0(r3)
		eieio
		lis     r6,0x10
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3DCC
		lis     r6,0x20
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x40
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x80
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		b       loc_4184

#********************************************************************************************
loc_3DCC:					#CODE XREF: findSetMem+300
		lis     r6,8
		stw     r3,0(r6)
		lis     r6,0x10
		stw     r3,0(r6)
		lis     r6,0x20
		stw     r3,0(r6)
		eieio
		stw     r4,0(r3)
		eieio
		lis     r6,8
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_4184
		lis     r6,0x10
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		lis     r6,0x20
		lwz     r7,0(r6)
		cmplw   r4,r7
		beq     loc_3E24
		b       loc_4184

#********************************************************************************************
loc_3E24:					#CODE XREF: findSetMem+1E0
						#findSetMem+1F0 ...
		cmplwi  r5,1
		bne     loc_3E84
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		or      r9,r9,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		or      r16,r16,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		or      r11,r11,r7
		mr      r7,r8
		addi    r7,r7,0xFF
		srwi    r7,r7,28
		andi.   r7,r7,3
		or      r18,r18,r7
		andi.   r25,r25,3
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_3E84:					#CODE XREF: findSetMem+394
		cmplwi  r5,2
		bne     loc_3EF4
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,8
		or      r9,r9,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,8
		or      r16,r16,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,8
		or      r11,r11,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,8
		or      r18,r18,r7
		andi.   r25,r25,0xC
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_3EF4:					#CODE XREF: findSetMem+3F4
		cmplwi  r5,4
		bne     loc_3F64
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,16
		or      r9,r9,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,16
		or      r16,r16,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,16
		or      r11,r11,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,16
		or      r18,r18,r7
		andi.   r25,r25,0x30
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_3F64:					#CODE XREF: findSetMem+464
		cmplwi  r5,8
		bne     loc_3FD4
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,24
		or      r9,r9,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,24
		or      r16,r16,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,24
		or      r11,r11,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,24
		or      r18,r18,r7
		andi.   r25,r25,0xC0
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_3FD4:					#CODE XREF: findSetMem+4D4
		cmplwi  r5,0x10
		bne     loc_4034
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		or      r10,r10,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		or      r17,r17,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		or      r12,r12,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		or      r19,r19,r7
		andi.   r25,r25,0x300
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_4034:
		cmplwi  r5,0x20
		bne     loc_40A4
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,8
		or      r10,r10,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,8
		or      r17,r17,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,8
		or      r12,r12,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,8
		or      r19,r19,r7
		andi.   r25,r25,0xC00
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_40A4:
		cmplwi  r5,0x40
		bne     loc_4114
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,16
		or      r10,r10,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,16
		or      r17,r17,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,16
		or      r12,r12,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,16
		or	    r19,r19,r7
		andi.   r25,r25,0x3000
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_4114:
		cmplwi  r5,0x80
		bne     loc_4184
		mr      r7,r8
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,24
		or      r10,r10,r7
		mr      r7,r8
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,24
		or      r17,r17,r7
		add     r8,r8,r6
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,20
		andi.   r7,r7,0xFF
		slwi    r7,r7,24
		or      r12,r12,r7
		mr      r7,r8
		addi    r7,r7,-1
		srwi    r7,r7,28
		andi.   r7,r7,3
		slwi    r7,r7,24
		or      r19,r19,r7
		andi.   r25,r25,0xc000
		or      r13,r13,r25
		b       loc_4184

#********************************************************************************************
loc_4184:
		slwi    r5,r5,1
		cmplwi  r5,0x100
		bne     loc_3BD8

        stw     r9,MS_MSAR1(r15)
        stw     r10,MS_MSAR2(r15)
        stw     r11,MS_MEAR1(r15)
        stw     r12,MS_MEAR2(r15)
        stw     r16,MS_MESAR1(r15)
        stw     r17,MS_MESAR2(r15)
        stw     r18,MS_MEEAR1(r15)
        stw     r19,MS_MEEAR2(r15)
        stw     r14,MS_MBEN(r15)
        stw     r13,MS_MCCR1(r15)
        stw     r8,MS_MEMSIZE(r15)

        lwz     r13,100(r1)
        lwz     r14,104(r1)
        lwz     r15,108(r1)
        lwz     r16,112(r1)
        lwz     r17,116(r1)
        lwz     r18,120(r1)
        lwz     r19,124(r1)
        lwz     r20,128(r1)
        lwz     r21,132(r1)
        lwz     r22,136(r1)
        lwz     r23,140(r1)
        lwz     r24,144(r1)
        lwz     r25,148(r1)
        lwz     r26,152(r1)
        lwz     r27,156(r1)
        lwz     r28,160(r1)
        lwz     r29,164(r1)
        lwz     r30,168(r1)
        lwz     r31,172(r1)
        lwz     r1,0(r1)

		blr

#********************************************************************************************
